// Loopback transport/lifecycle tests for CoreEndpoint and HostClient.
//
// The "fake core" here only echoes protocol responses so transport behaviour can
// be tested. It is NOT gameplay: native spell execution is proven separately
// against a real worldserver (tests/integration).
#include "minitest.h"

#include "gamebridge/endpoint.h"

#include <chrono>
#include <thread>

using namespace gamebridge;
using namespace std::chrono_literals;

namespace {

std::string const kToken = "test-token-0123456789abcdef";

CoreEndpoint::Config coreCfg()
{
    CoreEndpoint::Config c;
    c.port = 0;  // ephemeral
    c.token = kToken;
    c.timings.heartbeatMs = 200;
    c.timings.heartbeatTimeoutMs = 1500;
    c.timings.stallTimeoutMs = 300;
    c.timings.helloTimeoutMs = 1000;
    return c;
}

// Raw protocol client for adversarial tests.
struct Raw
{
    TcpStream s;
    FrameDecoder d;
    std::string epoch;
    long long seq = 0;

    bool connect(std::uint16_t port) { return s.connectLoopback(port, 1000); }
    bool sendBody(std::string const& b) { return s.sendFrame(b, 1000); }
    bool sendEnv(MsgType t, Json p, std::string const& req = std::string(), long long forceSeq = -1)
    {
        Envelope e;
        e.type = t;
        e.epoch = epoch;
        e.seq = forceSeq >= 0 ? forceSeq : ++seq;
        e.hasRequestId = !req.empty();
        e.requestId = req;
        e.payload = std::move(p);
        return sendBody(e.serialize());
    }
    // Next non-PING/PONG envelope within timeout. Returns false on timeout/close.
    bool next(Envelope& out, int timeoutMs = 1500)
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        char buf[4096];
        for (;;)
        {
            while (d.hasFrame())
            {
                DecodeResult r = decodeEnvelope(d.pop());
                if (!r.ok)
                    return false;
                if (r.env.type == MsgType::Ping)
                {
                    Json p = Json::object();
                    p.set("nonce", *r.env.payload.find("nonce"));
                    sendEnv(MsgType::Pong, p);
                    continue;
                }
                if (r.env.type == MsgType::Pong)
                    continue;
                out = r.env;
                return true;
            }
            if (std::chrono::steady_clock::now() > deadline)
                return false;
            int n = s.recvSome(buf, sizeof(buf), 20);
            if (n < 0)
                return false;
            if (n > 0 && !d.feed(buf, static_cast<std::size_t>(n)))
                return false;
        }
    }
    bool closedWithin(int timeoutMs)
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        char buf[4096];
        while (std::chrono::steady_clock::now() < deadline)
        {
            int n = s.recvSome(buf, sizeof(buf), 20);
            if (n < 0)
                return true;
        }
        return false;
    }
    Json hello(std::string const& token = kToken)
    {
        Json p = Json::object();
        p.set("adapter", Json::string("test"));
        p.set("version", Json::integer(1));
        p.set("token", Json::string(token));
        return p;
    }
};

Json castPayload(std::uint32_t spell = 133)
{
    Json p = Json::object();
    p.set("caster_id", Json::string("player-1"));
    p.set("target_id", Json::string("target-1"));
    p.set("spell_id", Json::integer(spell));
    p.set("host_generation", Json::integer(1));
    return p;
}

// Minimal fake core loop: answers Connected with WELCOME and each CAST_REQUEST
// with CAST_STATUS accepted. Counts how many casts reached the "game thread".
struct FakeCore
{
    CoreEndpoint ep;
    std::thread th;
    std::atomic<bool> stop { false };
    std::atomic<int> casts { 0 };
    std::atomic<int> connects { 0 };
    std::atomic<int> disconnects { 0 };
    std::atomic<bool> holdCasts { false };   // simulate a slow world thread
    int welcomeDelayMs = 0;                   // world thread slower than a heartbeat period
    std::string lastEpoch;

    explicit FakeCore(CoreEndpoint::Config c = coreCfg()) : ep(c) { }
    bool start()
    {
        if (!ep.start())
            return false;
        th = std::thread([this] {
            while (!stop)
            {
                if (holdCasts)
                {
                    std::this_thread::sleep_for(5ms);
                    continue;
                }
                CoreEndpoint::Inbound in;
                if (!ep.pollInbound(in))
                {
                    std::this_thread::sleep_for(2ms);
                    continue;
                }
                if (in.kind == CoreEndpoint::Inbound::Kind::Connected)
                {
                    ++connects;
                    lastEpoch = in.epoch;
                    if (welcomeDelayMs)
                        std::this_thread::sleep_for(std::chrono::milliseconds(welcomeDelayMs));
                    Json w = Json::object();
                    w.set("core_sha", Json::string("fake-transport-test"));
                    Json caps = Json::array();
                    caps.push(Json::string("transport_test_only"));
                    w.set("capabilities", caps);
                    w.set("character_id", Json::string("char-1"));
                    w.set("player_entity_id", Json::string("player-1"));
                    w.set("data_fixture", Json::string("none"));
                    ep.send(in.epoch, MsgType::Welcome, w);
                }
                else if (in.kind == CoreEndpoint::Inbound::Kind::Disconnected)
                    ++disconnects;
                else if (in.env.type == MsgType::CastRequest)
                {
                    ++casts;
                    Json s = Json::object();
                    s.set("status", Json::string("accepted"));
                    s.set("reason", Json::null());
                    s.set("spell_id", *in.env.payload.find("spell_id"));
                    ep.send(in.epoch, MsgType::CastStatus, s, in.env.requestId);
                }
            }
        });
        return true;
    }
    ~FakeCore()
    {
        stop = true;
        if (th.joinable())
            th.join();
        ep.stop();
    }
};

bool handshake(Raw& r, std::uint16_t port)
{
    if (!r.connect(port))
        return false;
    r.epoch.clear();
    if (!r.sendEnv(MsgType::Hello, r.hello(), std::string(), 0))
        return false;
    Envelope w;
    if (!r.next(w) || w.type != MsgType::Welcome)
        return false;
    r.epoch = w.epoch;
    r.seq = 0;
    return true;
}

template <typename F>
bool waitFor(F f, int ms = 2000)
{
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (f())
            return true;
        std::this_thread::sleep_for(5ms);
    }
    return f();
}

} // namespace

TEST_CASE("endpoint: refuses to start without a token")
{
    CoreEndpoint::Config c = coreCfg();
    c.token = "short";
    CoreEndpoint ep(c);
    CHECK(!ep.start());
}

TEST_CASE("endpoint: handshake, WELCOME epoch, seq continues")
{
    FakeCore core;
    CHECK(core.start());
    Raw r;
    CHECK(handshake(r, core.ep.port()));
    CHECK(!r.epoch.empty());
    CHECK(r.sendEnv(MsgType::CastRequest, castPayload(), "cast-1"));
    Envelope s;
    CHECK(r.next(s));
    CHECK(s.type == MsgType::CastStatus);
    CHECK_EQ(s.requestId, std::string("cast-1"));
}

TEST_CASE("endpoint: wrong token rejected before any game work")
{
    FakeCore core;
    CHECK(core.start());
    Raw r;
    CHECK(r.connect(core.ep.port()));
    CHECK(r.sendEnv(MsgType::Hello, r.hello("wrong-token-wrong-token"), std::string(), 0));
    Envelope e;
    CHECK(r.next(e));
    CHECK(e.type == MsgType::Error);
    CHECK_EQ(e.payload.find("code")->asString(), std::string("unauthorized"));
    CHECK(r.closedWithin(1000));
    CHECK_EQ(core.connects.load(), 0);
}

TEST_CASE("endpoint: unsupported version rejected and connection closed")
{
    FakeCore core;
    CHECK(core.start());
    Raw r;
    CHECK(r.connect(core.ep.port()));
    CHECK(r.sendBody("{\"version\":2,\"type\":\"HELLO\",\"epoch\":\"\",\"seq\":0,\"request_id\":null,\"payload\":{}}"));
    Envelope e;
    CHECK(r.next(e));
    CHECK(e.type == MsgType::Error && e.payload.find("code")->asString() == "unsupported_version");
    CHECK(r.closedWithin(1000));
    CHECK_EQ(core.connects.load(), 0);
}

TEST_CASE("endpoint: gameplay before HELLO rejected")
{
    FakeCore core;
    CHECK(core.start());
    Raw r;
    CHECK(r.connect(core.ep.port()));
    CHECK(r.sendEnv(MsgType::CastRequest, castPayload(), "cast-1", 0));
    Envelope e;
    CHECK(r.next(e));
    CHECK(e.type == MsgType::Error);
    CHECK(r.closedWithin(1000));
    CHECK_EQ(core.casts.load(), 0);
}

TEST_CASE("endpoint: duplicate CAST_REQUEST executes once, mismatch and replay rejected")
{
    CoreEndpoint::Config c = coreCfg();
    c.dedupeCapacity = 2;
    FakeCore core(c);
    CHECK(core.start());
    Raw r;
    CHECK(handshake(r, core.ep.port()));
    Envelope e;
    CHECK(r.sendEnv(MsgType::CastRequest, castPayload(), "cast-1"));
    CHECK(r.next(e) && e.type == MsgType::CastStatus);
    CHECK(r.sendEnv(MsgType::CastRequest, castPayload(), "cast-1"));        // retry
    CHECK(r.next(e) && e.type == MsgType::CastStatus && e.requestId == "cast-1");  // cached response
    CHECK(r.sendEnv(MsgType::CastRequest, castPayload(116), "cast-1"));     // same id, different payload
    CHECK(r.next(e) && e.type == MsgType::Error && e.payload.find("code")->asString() == "request_payload_mismatch");
    CHECK(r.sendEnv(MsgType::CastRequest, castPayload(), "cast-2"));
    CHECK(r.next(e) && e.type == MsgType::CastStatus);
    CHECK(r.sendEnv(MsgType::CastRequest, castPayload(), "cast-3"));
    CHECK(r.next(e) && e.type == MsgType::CastStatus);
    CHECK(r.sendEnv(MsgType::CastRequest, castPayload(), "cast-1"));        // evicted -> replay
    CHECK(r.next(e) && e.type == MsgType::Error && e.payload.find("code")->asString() == "request_replay");
    CHECK(waitFor([&] { return core.casts.load() == 3; }));
    std::this_thread::sleep_for(50ms);
    CHECK_EQ(core.casts.load(), 3);
}

TEST_CASE("endpoint: stale epoch and non-increasing seq rejected")
{
    FakeCore core;
    CHECK(core.start());
    Raw r;
    CHECK(handshake(r, core.ep.port()));
    std::string good = r.epoch;
    r.epoch = "old-epoch";
    CHECK(r.sendEnv(MsgType::CastRequest, castPayload(), "cast-1"));
    Envelope e;
    CHECK(r.next(e) && e.type == MsgType::Error && e.payload.find("code")->asString() == "stale_epoch");
    r.epoch = good;
    CHECK(r.sendEnv(MsgType::CastRequest, castPayload(), "cast-2", 1));  // seq 1 already used by the stale one? no: stale rejected before seq
    CHECK(r.next(e) && e.type == MsgType::CastStatus);
    CHECK(r.sendEnv(MsgType::CastRequest, castPayload(), "cast-3", 1));  // seq repeat
    CHECK(r.next(e) && e.type == MsgType::Error && e.payload.find("code")->asString() == "bad_seq");
    CHECK_EQ(core.casts.load(), 1);
}

TEST_CASE("endpoint: partial writes (byte-by-byte) are reassembled")
{
    FakeCore core;
    CHECK(core.start());
    Raw r;
    CHECK(handshake(r, core.ep.port()));
    Envelope e;
    e.type = MsgType::CastRequest;
    e.epoch = r.epoch;
    e.seq = ++r.seq;
    e.hasRequestId = true;
    e.requestId = "cast-1";
    e.payload = castPayload();
    std::string f = encodeFrame(e.serialize());
    for (char ch : f)
    {
        CHECK(r.s.sendAll(&ch, 1, 1000));
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    Envelope s;
    CHECK(r.next(s) && s.type == MsgType::CastStatus);
}

TEST_CASE("endpoint: peer stalling mid-frame expires")
{
    FakeCore core;
    CHECK(core.start());
    Raw r;
    CHECK(handshake(r, core.ep.port()));
    char hdr[3] = { 0, 0, 1 };
    CHECK(r.s.sendAll(hdr, 3, 1000));
    // Keep answering pings but never finish the frame: the 4th header byte never comes.
    CHECK(r.closedWithin(3000));
    CHECK(waitFor([&] { return core.disconnects.load() == 1; }));
}

TEST_CASE("endpoint: zero-length frame drops the connection")
{
    FakeCore core;
    CHECK(core.start());
    Raw r;
    CHECK(handshake(r, core.ep.port()));
    char z[4] = { 0, 0, 0, 0 };
    CHECK(r.s.sendAll(z, 4, 1000));
    CHECK(r.closedWithin(2000));
}

TEST_CASE("endpoint: malformed payload after handshake gets ERROR, connection kept")
{
    FakeCore core;
    CHECK(core.start());
    Raw r;
    CHECK(handshake(r, core.ep.port()));
    CHECK(r.sendBody("{\"version\":1,\"type\":\"CAST_REQUEST\",\"epoch\":\"" + r.epoch +
                     "\",\"seq\":5,\"request_id\":\"cast-1\",\"payload\":{\"caster_id\":\"p\",\"caster_id\":\"q\"}}"));
    Envelope e;
    CHECK(r.next(e) && e.type == MsgType::Error && e.payload.find("code")->asString() == "invalid_json");
    r.seq = 5;
    CHECK(r.sendEnv(MsgType::CastRequest, castPayload(), "cast-1"));
    CHECK(r.next(e) && e.type == MsgType::CastStatus);
}

TEST_CASE("endpoint: peer disappears mid-cast; reconnect gets new epoch, old pending dropped")
{
    FakeCore core;
    CHECK(core.start());
    std::string firstEpoch;
    {
        Raw r;
        CHECK(handshake(r, core.ep.port()));
        firstEpoch = r.epoch;
        core.holdCasts = true;                          // world thread busy
        CHECK(r.sendEnv(MsgType::CastRequest, castPayload(), "cast-1"));
        std::this_thread::sleep_for(50ms);
        r.s.close();                                    // host vanishes mid-cast
    }
    CHECK(waitFor([&] { return core.ep.currentEpoch().empty(); }));
    core.holdCasts = false;
    CHECK(waitFor([&] { return core.disconnects.load() == 1; }));
    CHECK_EQ(core.casts.load(), 0);                     // pending cast invalidated, never replayed

    Raw r2;
    CHECK(handshake(r2, core.ep.port()));
    CHECK(r2.epoch != firstEpoch);
    // A message carrying the old epoch is rejected on the new connection.
    std::string fresh = r2.epoch;
    r2.epoch = firstEpoch;
    CHECK(r2.sendEnv(MsgType::CastRequest, castPayload(), "cast-1"));
    r2.epoch = fresh;  // PONGs sent while reading must use the live epoch
    Envelope e;
    CHECK(r2.next(e) && e.type == MsgType::Error && e.payload.find("code")->asString() == "stale_epoch");
    // Game-thread output addressed to the old epoch is never delivered.
    Json s = Json::object();
    s.set("status", Json::string("completed"));
    s.set("reason", Json::null());
    s.set("spell_id", Json::integer(133));
    CHECK(core.ep.send(firstEpoch, MsgType::CastStatus, s, "cast-1"));
    bool got = r2.next(e, 300);
    if (got)
        std::printf("    unexpected: %s\n", e.serialize().c_str());
    CHECK(!got);
    CHECK_EQ(core.casts.load(), 0);
}

TEST_CASE("endpoint: inbound overflow freezes with RESET instead of losing casts")
{
    CoreEndpoint::Config c = coreCfg();
    c.inboundCapacity = 4;
    FakeCore core(c);
    CHECK(core.start());
    Raw r;
    CHECK(handshake(r, core.ep.port()));
    core.holdCasts = true;
    for (int i = 1; i <= 8; ++i)
        r.sendEnv(MsgType::CastRequest, castPayload(), "cast-" + std::to_string(i));
    Envelope e;
    bool sawReset = false;
    while (r.next(e, 1500))
        if (e.type == MsgType::Reset)
            sawReset = true;
    CHECK(sawReset);
    CHECK(r.closedWithin(1000));
    core.holdCasts = false;
    CHECK(waitFor([&] { return core.disconnects.load() == 1; }));
    CHECK_EQ(core.casts.load(), 0);   // queue of the frozen epoch discarded, not executed later
}

TEST_CASE("endpoint: heartbeat timeout drops silent peer")
{
    FakeCore core;
    CHECK(core.start());
    Raw r;
    CHECK(handshake(r, core.ep.port()));
    // Never read or answer pings.
    CHECK(waitFor([&] { return core.disconnects.load() == 1; }, 4000));
}

TEST_CASE("host client: connects, gets epoch, request ids increase, reconnects with new epoch")
{
    FakeCore core;
    CHECK(core.start());
    HostClient::Config hc;
    hc.port = core.ep.port();
    hc.token = kToken;
    hc.timings.heartbeatMs = 200;
    HostClient host(hc);
    host.start();
    CHECK(waitFor([&] { return host.connected(); }));
    HostClient::Inbound in;
    CHECK(waitFor([&] { return host.pollInbound(in); }));
    CHECK(in.kind == HostClient::Inbound::Kind::Connected);
    std::string e1 = host.epoch();
    std::string id = host.nextRequestId("cast");
    CHECK_EQ(id, std::string("cast-1"));
    CHECK(host.send(MsgType::CastRequest, castPayload(), id));
    CHECK(waitFor([&] { return host.pollInbound(in); }));
    CHECK(in.env.type == MsgType::CastStatus && in.env.requestId == id);

    // Core restarts: host sees Disconnected then a fresh epoch.
    core.ep.stop();
    CHECK(waitFor([&] { return !host.connected(); }, 3000));
    CHECK(!host.send(MsgType::CastRequest, castPayload(), host.nextRequestId("cast")));  // not buffered
    CHECK(core.ep.start());
    CHECK(waitFor([&] { return host.connected(); }, 6000));
    CHECK(host.epoch() != e1);
    CHECK_EQ(host.nextRequestId("cast"), std::string("cast-1"));
    host.stop();
}

TEST_CASE("endpoint: no heartbeat before WELCOME (slow world thread)")
{
    FakeCore core;
    core.welcomeDelayMs = 700;   // > heartbeatMs (200)
    CHECK(core.start());
    Raw r;
    CHECK(r.connect(core.ep.port()));
    CHECK(r.sendEnv(MsgType::Hello, r.hello(), std::string(), 0));
    // The first core message must be WELCOME, never a PING.
    char buf[4096];
    auto deadline = std::chrono::steady_clock::now() + 2s;
    bool got = false;
    while (!got && std::chrono::steady_clock::now() < deadline)
    {
        int n = r.s.recvSome(buf, sizeof(buf), 20);
        if (n > 0)
            r.d.feed(buf, static_cast<std::size_t>(n));
        if (r.d.hasFrame())
        {
            DecodeResult d = decodeEnvelope(r.d.pop());
            CHECK(d.ok && d.env.type == MsgType::Welcome);
            got = true;
        }
    }
    CHECK(got);
}

TEST_CASE("host client: survives a slow WELCOME")
{
    FakeCore core;
    core.welcomeDelayMs = 700;
    CHECK(core.start());
    HostClient::Config hc;
    hc.port = core.ep.port();
    hc.token = kToken;
    hc.timings.heartbeatMs = 200;
    HostClient host(hc);
    host.start();
    CHECK(waitFor([&] { return host.connected(); }, 3000));
    CHECK_EQ(core.connects.load(), 1);   // first attempt succeeded, no reconnect loop
    host.stop();
}
