// Pure protocol tests: framing, strict JSON, envelope/payload validation,
// dedupe, revisions, event dedupe, bounded queues. No sockets, no gameplay.
#include "minitest.h"

#include "gamebridge/framing.h"
#include "gamebridge/json.h"
#include "gamebridge/protocol.h"
#include "gamebridge/session.h"

#include <string>

using namespace gamebridge;

namespace {

std::string env(std::string const& type, std::string const& payload, std::string const& epoch = "e", long long seq = 1,
                std::string const& reqId = "null")
{
    return "{\"version\":1,\"type\":\"" + type + "\",\"epoch\":\"" + epoch + "\",\"seq\":" + std::to_string(seq) +
           ",\"request_id\":" + reqId + ",\"payload\":" + payload + "}";
}

std::string castPayload(int spell = 133)
{
    return "{\"caster_id\":\"player-1\",\"target_id\":\"target-1\",\"spell_id\":" + std::to_string(spell) +
           ",\"host_generation\":1}";
}

} // namespace

// ---------------------------------------------------------------- framing

TEST_CASE("framing: split length header and split body")
{
    std::string f = encodeFrame("{\"a\":1}");
    FrameDecoder d;
    for (char c : f)
    {
        CHECK(!d.hasFrame());
        CHECK(d.feed(&c, 1));
    }
    CHECK(d.hasFrame());
    CHECK_EQ(d.pop(), std::string("{\"a\":1}"));
    CHECK(!d.midFrame());
}

TEST_CASE("framing: concatenated frames in one read")
{
    std::string all = encodeFrame("one") + encodeFrame("two") + encodeFrame("three");
    std::string partial = encodeFrame("four");
    all += partial.substr(0, 6);
    FrameDecoder d;
    CHECK(d.feed(all.data(), all.size()));
    CHECK_EQ(d.pending(), std::size_t(3));
    CHECK_EQ(d.pop(), std::string("one"));
    CHECK_EQ(d.pop(), std::string("two"));
    CHECK_EQ(d.pop(), std::string("three"));
    CHECK(d.midFrame());
    CHECK(d.feed(partial.data() + 6, partial.size() - 6));
    CHECK_EQ(d.pop(), std::string("four"));
}

TEST_CASE("framing: zero length frame rejected")
{
    char z[4] = { 0, 0, 0, 0 };
    FrameDecoder d;
    CHECK(!d.feed(z, 4));
    CHECK(d.error() == FrameDecoder::Error::ZeroLength);
    CHECK(!d.feed("x", 1));  // latched
    CHECK(encodeFrame("").empty());
}

TEST_CASE("framing: oversize frame rejected before buffering")
{
    unsigned char h[4] = { 0x00, 0x01, 0x00, 0x01 }; // 65537
    FrameDecoder d;
    CHECK(!d.feed(reinterpret_cast<char*>(h), 4));
    CHECK(d.error() == FrameDecoder::Error::Oversize);
    CHECK(encodeFrame(std::string(65537, 'x')).empty());
    CHECK(!encodeFrame(std::string(65536, 'x')).empty());
    unsigned char big[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
    FrameDecoder d2;
    CHECK(!d2.feed(reinterpret_cast<char*>(big), 4));
}

// ---------------------------------------------------------------- json

TEST_CASE("json: duplicate keys rejected")
{
    CHECK(!parseJson("{\"a\":1,\"a\":2}").ok);
    CHECK_EQ(parseJson("{\"a\":1,\"a\":2}").error, std::string("duplicate_key"));
    CHECK(!parseJson("{\"o\":{\"k\":1,\"k\":1}}").ok);
    CHECK(parseJson("{\"a\":1,\"b\":{\"a\":2}}").ok);
}

TEST_CASE("json: malformed utf-8 rejected")
{
    CHECK(!parseJson("{\"a\":\"\xC3\x28\"}").ok);          // bad continuation
    CHECK(!parseJson("{\"a\":\"\xC0\xAF\"}").ok);          // overlong
    CHECK(!parseJson("{\"a\":\"\xED\xA0\x80\"}").ok);      // UTF-16 surrogate
    CHECK(!parseJson("{\"a\":\"\xF4\x90\x80\x80\"}").ok);  // > U+10FFFF
    CHECK(!parseJson("{\"a\":\"\\ud800\"}").ok);           // lone surrogate escape
    CHECK(parseJson("{\"a\":\"caf\xC3\xA9 \\ud83d\\ude00\"}").ok);
}

TEST_CASE("json: excessive nesting rejected")
{
    std::string ok, bad;
    for (int i = 0; i < 16; ++i) ok += "[";
    for (int i = 0; i < 16; ++i) ok += "]";
    for (int i = 0; i < 17; ++i) bad += "[";
    for (int i = 0; i < 17; ++i) bad += "]";
    CHECK(parseJson(ok).ok);
    CHECK(!parseJson(bad).ok);
    CHECK_EQ(parseJson(bad).error, std::string("nesting_too_deep"));
    std::string huge(100000, '[');
    CHECK(!parseJson(huge).ok);  // no stack exhaustion
}

TEST_CASE("json: non-finite and malformed numbers rejected")
{
    CHECK(!parseJson("{\"x\":1e999}").ok);
    CHECK(!parseJson("{\"x\":-1e400}").ok);
    CHECK(!parseJson("{\"x\":NaN}").ok);
    CHECK(!parseJson("{\"x\":Infinity}").ok);
    CHECK(!parseJson("{\"x\":01}").ok);
    CHECK(!parseJson("{\"x\":1.}").ok);
    CHECK(!parseJson("{\"x\":+1}").ok);
    CHECK(parseJson("{\"x\":-0.5e3}").ok);
}

TEST_CASE("json: trailing data and truncation rejected; round trip")
{
    CHECK(!parseJson("{} {}").ok);
    CHECK(!parseJson("{\"a\":").ok);
    CHECK(!parseJson("").ok);
    JsonParseResult r = parseJson("{\"s\":\"a\\\"b\\n\\u0001\",\"n\":[1,2.5,true,null]}");
    CHECK(r.ok);
    JsonParseResult r2 = parseJson(r.value.dump());
    CHECK(r2.ok);
    CHECK(r.value == r2.value);
}

// ---------------------------------------------------------------- envelope

TEST_CASE("envelope: valid examples decode")
{
    DecodeResult r = decodeEnvelope(env("CAST_REQUEST", castPayload(), "e1", 3, "\"cast-1\""));
    CHECK(r.ok);
    CHECK(r.env.type == MsgType::CastRequest);
    CHECK_EQ(r.env.requestId, std::string("cast-1"));
    CHECK(decodeEnvelope(env("HELLO", "{\"adapter\":\"gta-sa\",\"version\":1,\"token\":\"t\"}", "", 0)).ok);
}

TEST_CASE("envelope: unsupported version rejected first")
{
    DecodeResult r = decodeEnvelope("{\"version\":2,\"type\":\"BOGUS\",\"payload\":5}");
    CHECK(!r.ok);
    CHECK_EQ(r.code, std::string("unsupported_version"));
}

TEST_CASE("envelope: invalid enum / unknown type / missing fields")
{
    CHECK_EQ(decodeEnvelope(env("FIREBALL", "{}")).code, std::string("unknown_type"));
    CHECK_EQ(decodeEnvelope("{\"version\":1,\"type\":\"PING\",\"epoch\":\"e\",\"seq\":1,\"payload\":{\"nonce\":\"n\"}}").code,
             std::string("invalid_envelope"));
    CHECK_EQ(decodeEnvelope(env("PING", "{\"nonce\":\"n\",\"extra\":1}")).code, std::string("invalid_payload"));
    CHECK_EQ(decodeEnvelope(env("CAST_STATUS", "{\"status\":\"exploded\",\"reason\":null,\"spell_id\":1}")).code,
             std::string("invalid_payload"));
    CHECK_EQ(decodeEnvelope(env("CAST_REQUEST", "{\"caster_id\":\"p\",\"target_id\":\"t\",\"spell_id\":1}")).code,
             std::string("invalid_payload"));
    CHECK_EQ(decodeEnvelope(env("PING", "{\"nonce\":\"n\"}", "e", -1)).code, std::string("invalid_envelope"));
    CHECK_EQ(decodeEnvelope(env("PING", "{\"nonce\":\"n\"}", "e", 9007199254740992LL)).code, std::string("invalid_envelope"));
    CHECK_EQ(decodeEnvelope(env("PING", "{\"nonce\":\"n\"}", std::string(129, 'e'))).code, std::string("invalid_envelope"));
}

TEST_CASE("envelope: wrong value types and non-finite / out of range positions")
{
    CHECK(!decodeEnvelope(env("CAST_REQUEST", "{\"caster_id\":\"p\",\"target_id\":\"t\",\"spell_id\":1.5,\"host_generation\":1}")).ok);
    CHECK(!decodeEnvelope(env("CAST_REQUEST", "{\"caster_id\":\"p\",\"target_id\":\"t\",\"spell_id\":-1,\"host_generation\":1}")).ok);
    CHECK(!decodeEnvelope(env("CAST_REQUEST", "{\"caster_id\":\"\",\"target_id\":\"t\",\"spell_id\":1,\"host_generation\":1}")).ok);
    std::string pos = "{\"entity_id\":\"player-1\",\"generation\":1,\"sample_id\":1,\"position\":{\"x\":1,\"y\":2,\"z\":%Z},\"orientation\":0}";
    std::string good = pos, inf = pos, far = pos;
    good.replace(good.find("%Z"), 2, "3");
    inf.replace(inf.find("%Z"), 2, "1e999");
    far.replace(far.find("%Z"), 2, "1e7");
    CHECK(decodeEnvelope(env("POSITION", good)).ok);
    CHECK(!decodeEnvelope(env("POSITION", inf)).ok);
    CHECK(!decodeEnvelope(env("POSITION", far)).ok);
}

TEST_CASE("envelope: serialize/decode round trip")
{
    Envelope e;
    e.type = MsgType::StateSnapshot;
    e.epoch = "e9";
    e.seq = 42;
    JsonParseResult p = parseJson("{\"full\":true,\"revision\":7,\"entities\":[{\"entity_id\":\"target-1\",\"generation\":1,"
                                  "\"revision\":7,\"hp\":40,\"max_hp\":55,\"alive\":true}]}");
    CHECK(p.ok);
    e.payload = p.value;
    DecodeResult r = decodeEnvelope(e.serialize());
    CHECK(r.ok);
    CHECK(r.env.payload == e.payload);
    CHECK(!r.env.hasRequestId);
}

TEST_CASE("token comparison")
{
    CHECK(tokenEquals("abcdefghijklmnop", "abcdefghijklmnop"));
    CHECK(!tokenEquals("abcdefghijklmnop", "abcdefghijklmnoq"));
    CHECK(!tokenEquals("abc", "abcd"));
    CHECK(!tokenEquals("", "x"));
}

// ---------------------------------------------------------------- dedupe

TEST_CASE("dedupe: duplicate CAST_REQUEST accepted once, mismatch rejected")
{
    RequestCache c(4);
    Json p1 = parseJson(castPayload(133)).value;
    Json p1reordered = parseJson("{\"spell_id\":133,\"host_generation\":1,\"target_id\":\"target-1\",\"caster_id\":\"player-1\"}").value;
    Json p2 = parseJson(castPayload(116)).value;
    CHECK(c.check("cast-1", payloadHash(p1)) == RequestCache::Verdict::New);
    CHECK(c.check("cast-1", payloadHash(p1)) == RequestCache::Verdict::Duplicate);
    CHECK(c.check("cast-1", payloadHash(p1reordered)) == RequestCache::Verdict::Duplicate);
    CHECK(c.check("cast-1", payloadHash(p2)) == RequestCache::Verdict::PayloadMismatch);
    CHECK(c.check("other-1", payloadHash(p1)) == RequestCache::Verdict::PayloadMismatch);
    c.complete("cast-1", "{\"status\":\"accepted\"}");
    RequestCache::Entry const* e = nullptr;
    CHECK(c.check("cast-1", payloadHash(p1), &e) == RequestCache::Verdict::Duplicate);
    CHECK(e && e->completed);
}

TEST_CASE("dedupe: replay outside window rejected, ids never reusable")
{
    RequestCache c(3);
    std::uint64_t h = 1;
    for (int i = 1; i <= 5; ++i)
        CHECK(c.check("cast-" + std::to_string(i), h) == RequestCache::Verdict::New);
    CHECK_EQ(c.size(), std::size_t(3));
    CHECK(c.check("cast-1", h) == RequestCache::Verdict::Replay);   // evicted
    CHECK(c.check("cast-2", h) == RequestCache::Verdict::Replay);   // evicted
    CHECK(c.check("cast-5", h) == RequestCache::Verdict::Duplicate);
    CHECK(c.check("cast-6", h) == RequestCache::Verdict::New);
    CHECK(c.check("cast-x", h) == RequestCache::Verdict::Malformed);
    CHECK(c.check("7", h) == RequestCache::Verdict::Malformed);
    c.reset();
    CHECK(c.check("cast-1", h) == RequestCache::Verdict::New);      // new epoch
}

TEST_CASE("seq tracker strictly increasing")
{
    SeqTracker s;
    CHECK(s.accept(0));
    CHECK(s.accept(1));
    CHECK(!s.accept(1));
    CHECK(!s.accept(0));
    CHECK(s.accept(10));
}

// ---------------------------------------------------------------- revisions & events

TEST_CASE("revision gate: reordered/duplicate state and recycled generation")
{
    RevisionGate g;
    CHECK(g.accept("target-1", 1, 5));
    CHECK(!g.accept("target-1", 1, 5));  // duplicate
    CHECK(!g.accept("target-1", 1, 4));  // reordered older
    CHECK(g.accept("target-1", 1, 6));
    CHECK(g.accept("target-1", 2, 1));   // new incarnation restarts revisions
    CHECK(!g.accept("target-1", 1, 99)); // late state for recycled/older generation
    g.reset();
    CHECK(g.accept("target-1", 1, 1));
}

TEST_CASE("event dedupe: duplicate presentation event ignored, bounded")
{
    EventDedupe d(2);
    CHECK(d.firstTime("ev-1"));
    CHECK(!d.firstTime("ev-1"));
    CHECK(d.firstTime("ev-2"));
    CHECK(d.firstTime("ev-3"));
    CHECK(!d.firstTime("ev-3"));
}

// ---------------------------------------------------------------- queues

TEST_CASE("bounded queue: telemetry coalesces, critical overflow latches freeze")
{
    BoundedQueue<int> q(3);
    CHECK(q.push(1, "pos:player"));
    CHECK(q.push(2, "pos:player"));  // coalesced
    CHECK_EQ(q.size(), std::size_t(1));
    CHECK(q.push(10));
    CHECK(q.push(11));
    CHECK(!q.push(12));              // critical item cannot be dropped -> overflow
    CHECK(q.overflowed());
    CHECK(!q.push(13));              // stays frozen until cleared (resync)
    int v = 0;
    CHECK(q.pop(v) && v == 2);
    CHECK(q.pop(v) && v == 10);
    CHECK(q.pop(v) && v == 11);
    CHECK(!q.pop(v));
    q.clear();
    CHECK(!q.overflowed());
    CHECK(q.push(1));
}
