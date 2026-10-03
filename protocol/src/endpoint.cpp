#include "gamebridge/endpoint.h"

#include <chrono>
#include <cstdio>
#include <random>
#include <vector>

namespace gamebridge {

namespace {

using Clock = std::chrono::steady_clock;

std::int64_t msSince(Clock::time_point t)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count();
}

Json errorPayload(std::string const& code, std::string const& relatedRequestId, std::string const& description)
{
    Json p = Json::object();
    p.set("code", Json::string(code));
    p.set("related_request_id", relatedRequestId.empty() ? Json::null() : Json::string(relatedRequestId));
    p.set("description", Json::string(description.empty() ? code : description));
    return p;
}

bool isDedupedRequest(MsgType t)
{
    return t == MsgType::CastRequest;
}

} // namespace

std::string makeEpoch(std::uint64_t counter)
{
    static std::mt19937_64 rng(std::random_device {}() ^
                               static_cast<std::uint64_t>(Clock::now().time_since_epoch().count()));
    char buf[64];
    std::snprintf(buf, sizeof(buf), "e%llu-%016llx", static_cast<unsigned long long>(counter),
                  static_cast<unsigned long long>(rng()));
    return buf;
}

// ===========================================================================
// CoreEndpoint
// ===========================================================================

CoreEndpoint::CoreEndpoint(Config cfg, LogFn log)
    : _cfg(std::move(cfg)), _log(std::move(log)), _inbound(_cfg.inboundCapacity), _outbound(_cfg.outboundCapacity)
{
}

CoreEndpoint::~CoreEndpoint()
{
    stop();
}

bool CoreEndpoint::start()
{
    if (_thread.joinable())
        return true;
    if (_cfg.token.size() < 16)
    {
        log("refusing to start: bridge token missing or shorter than 16 characters");
        return false;
    }
    // A restarted endpoint keeps its port so hosts can reconnect.
    std::uint16_t const port = _boundPort ? _boundPort : _cfg.port;
    if (!_listener.listenLoopback(port))
    {
        log("failed to bind 127.0.0.1:" + std::to_string(port));
        return false;
    }
    _boundPort = _listener.port();
    _stop = false;
    _thread = std::thread([this] { run(); });
    log("listening on 127.0.0.1:" + std::to_string(_boundPort));
    return true;
}

void CoreEndpoint::stop()
{
    _stop = true;
    if (_thread.joinable())
        _thread.join();
    _listener.close();
}

std::string CoreEndpoint::currentEpoch() const
{
    std::lock_guard<std::mutex> g(_mx);
    return _epoch;
}

bool CoreEndpoint::pollInbound(Inbound& out)
{
    std::lock_guard<std::mutex> g(_mx);
    return _inbound.pop(out);
}

bool CoreEndpoint::send(std::string const& epoch, MsgType type, Json payload, std::string const& requestId,
                        std::string const& coalesceKey)
{
    std::lock_guard<std::mutex> g(_mx);
    if (epoch.empty() || epoch != _epoch)
        return true; // stale epoch: never deliver
    Outbound o;
    o.epoch = epoch;
    o.type = type;
    o.payload = std::move(payload);
    o.requestId = requestId;
    return _outbound.push(std::move(o), coalesceKey);
}

bool CoreEndpoint::pushInbound(Inbound in)
{
    std::lock_guard<std::mutex> g(_mx);
    return _inbound.push(std::move(in));
}

void CoreEndpoint::run()
{
    while (!_stop)
    {
        TcpStream stream;
        if (!_listener.accept(stream, 100))
            continue;
        ++_connections;
        serveConnection(stream);
        stream.closeGraceful();
    }
}

void CoreEndpoint::serveConnection(TcpStream& stream)
{
    EndpointTimings const& t = _cfg.timings;
    FrameDecoder decoder;
    SeqTracker hostSeq;
    RequestCache cache(_cfg.dedupeCapacity);
    std::string epoch;   // accepted epoch; empty until HELLO accepted
    std::int64_t outSeq = 0;
    std::uint64_t pingCounter = 0;
    std::string disconnectReason = "peer_closed";
    // Heartbeats start only once the game thread has sent WELCOME: WELCOME must be
    // the first core message of an epoch, and a slow world tick must not time out.
    bool welcomed = false;

    auto const connectedAt = Clock::now();
    auto lastRx = Clock::now();
    auto lastByte = Clock::now();
    auto lastPing = Clock::now();

    auto sendEnv = [&](MsgType type, Json payload, std::string const& requestId) -> bool {
        Envelope e;
        e.type = type;
        e.epoch = epoch;
        e.seq = ++outSeq;
        e.hasRequestId = !requestId.empty();
        e.requestId = requestId;
        e.payload = std::move(payload);
        return stream.sendFrame(e.serialize(), t.writeTimeoutMs);
    };
    auto sendError = [&](std::string const& code, std::string const& reqId, std::string const& desc) {
        log("ERROR -> host: " + code + (desc.empty() ? "" : " (" + desc + ")"));
        if (!reqId.empty())
            cache.complete(reqId, errorPayload(code, reqId, desc).dump());
        return sendEnv(MsgType::Error, errorPayload(code, reqId, desc), reqId);
    };

    std::vector<char> buf(8192);
    bool alive = true;
    while (alive && !_stop)
    {
        // ---- outbound (only once a host is accepted) ----
        if (!epoch.empty())
        {
            std::vector<Outbound> batch;
            bool overflow = false;
            {
                std::lock_guard<std::mutex> g(_mx);
                Outbound o;
                while (_outbound.pop(o))
                    batch.push_back(std::move(o));
                overflow = _outbound.overflowed();
                if (overflow)
                    _outbound.clear();
            }
            if (overflow)
            {
                // Never silently lose casts/deaths: freeze and force a fresh epoch + snapshot.
                Json p = Json::object();
                p.set("reason", Json::string("core_outbound_overflow"));
                sendEnv(MsgType::Reset, p, std::string());
                disconnectReason = "core_outbound_overflow";
                break;
            }
            for (Outbound& o : batch)
            {
                if (o.epoch != epoch)
                    continue;
                if (!welcomed && o.type != MsgType::Welcome)
                    continue;   // nothing precedes WELCOME in an epoch
                if (o.type == MsgType::Welcome)
                {
                    welcomed = true;
                    lastRx = Clock::now();
                    lastPing = Clock::now();
                }
                if (!o.requestId.empty())
                    cache.complete(o.requestId, o.payload.dump());
                if (!sendEnv(o.type, std::move(o.payload), o.requestId))
                {
                    alive = false;
                    disconnectReason = "write_failed";
                    break;
                }
            }
            if (!alive)
                break;

            if (welcomed && msSince(lastPing) >= t.heartbeatMs)
            {
                Json p = Json::object();
                p.set("nonce", Json::string("c" + std::to_string(++pingCounter)));
                if (!sendEnv(MsgType::Ping, p, std::string()))
                {
                    disconnectReason = "write_failed";
                    break;
                }
                lastPing = Clock::now();
            }
            if (welcomed && msSince(lastRx) > t.heartbeatTimeoutMs)
            {
                disconnectReason = "heartbeat_timeout";
                break;
            }
        }
        else if (msSince(connectedAt) > t.helloTimeoutMs)
        {
            disconnectReason = "hello_timeout";
            break;
        }

        // ---- inbound ----
        int n = stream.recvSome(buf.data(), buf.size(), 10);
        if (n < 0)
        {
            disconnectReason = "peer_closed";
            break;
        }
        if (n == 0)
        {
            if (decoder.midFrame() && msSince(lastByte) > t.stallTimeoutMs)
            {
                disconnectReason = "stalled_mid_frame";
                break;
            }
            continue;
        }
        lastByte = Clock::now();
        if (!decoder.feed(buf.data(), static_cast<std::size_t>(n)))
        {
            sendError(decoder.error() == FrameDecoder::Error::ZeroLength ? "zero_length_frame" : "oversize_frame",
                      std::string(), std::string());
            disconnectReason = "framing_error";
            break;
        }

        while (alive && decoder.hasFrame())
        {
            std::string body = decoder.pop();
            DecodeResult d = decodeEnvelope(body);
            if (!d.ok)
            {
                sendError(d.code, std::string(), d.detail);
                if (epoch.empty() || d.code == "unsupported_version")
                {
                    alive = false;
                    disconnectReason = d.code;
                }
                continue;
            }
            Envelope& env = d.env;
            lastRx = Clock::now();

            if (epoch.empty())
            {
                // Handshake: HELLO, empty epoch, seq 0, valid token. Nothing else.
                if (env.type != MsgType::Hello || !env.epoch.empty() || env.seq != 0)
                {
                    sendError("handshake_required", std::string(), "first message must be HELLO epoch=\"\" seq=0");
                    alive = false;
                    disconnectReason = "bad_handshake";
                    break;
                }
                if (env.payload.find("version")->asInt() != kProtocolVersion)
                {
                    sendError("unsupported_version", std::string(), "payload version");
                    alive = false;
                    disconnectReason = "unsupported_version";
                    break;
                }
                if (!tokenEquals(env.payload.find("token")->asString(), _cfg.token))
                {
                    sendError("unauthorized", std::string(), "bad token");
                    alive = false;
                    disconnectReason = "unauthorized";
                    break;
                }
                hostSeq.accept(env.seq);
                env.payload.set("token", Json::string("<redacted>"));
                {
                    std::lock_guard<std::mutex> g(_mx);
                    epoch = makeEpoch(++_epochCounter);
                    _epoch = epoch;
                    _outbound.clear();
                    _inbound.clear();   // nothing from an older epoch survives
                    Inbound in;
                    in.kind = Inbound::Kind::Connected;
                    in.epoch = epoch;
                    in.env = env;
                    _inbound.push(std::move(in));
                }
                log("host accepted, epoch " + epoch + " adapter=" + env.payload.find("adapter")->asString());
                lastPing = Clock::now();
                continue;
            }

            if (env.epoch != epoch)
            {
                sendError("stale_epoch", env.hasRequestId ? env.requestId : std::string(), "epoch mismatch");
                continue;
            }
            if (!hostSeq.accept(env.seq))
            {
                sendError("bad_seq", env.hasRequestId ? env.requestId : std::string(), "seq not increasing");
                continue;
            }
            if (msgDirection(env.type) == Direction::CoreToHost || env.type == MsgType::Hello)
            {
                sendError("bad_direction", env.hasRequestId ? env.requestId : std::string(), msgTypeName(env.type));
                continue;
            }
            if (env.type == MsgType::Ping)
            {
                Json p = Json::object();
                p.set("nonce", *env.payload.find("nonce"));
                sendEnv(MsgType::Pong, p, std::string());
                continue;
            }
            if (env.type == MsgType::Pong)
                continue;

            if (isDedupedRequest(env.type))
            {
                if (!env.hasRequestId)
                {
                    sendError("missing_request_id", std::string(), msgTypeName(env.type));
                    continue;
                }
                RequestCache::Entry const* existing = nullptr;
                switch (cache.check(env.requestId, payloadHash(env.payload), &existing))
                {
                    case RequestCache::Verdict::New:
                        break;
                    case RequestCache::Verdict::Duplicate:
                        if (existing && existing->completed)
                        {
                            JsonParseResult cached = parseJson(existing->resultJson);
                            MsgType rt = cached.value.find("code") ? MsgType::Error : MsgType::CastStatus;
                            sendEnv(rt, cached.value, env.requestId);
                        }
                        log("duplicate request " + env.requestId + " not re-executed");
                        continue;
                    case RequestCache::Verdict::PayloadMismatch:
                        sendError("request_payload_mismatch", env.requestId, "same id, different payload");
                        continue;
                    case RequestCache::Verdict::Replay:
                        sendError("request_replay", env.requestId, "request id outside replay window");
                        continue;
                    case RequestCache::Verdict::Malformed:
                        sendError("bad_request_id", env.requestId, "expected <prefix>-<counter>");
                        continue;
                }
            }

            Inbound in;
            in.kind = Inbound::Kind::Message;
            in.epoch = epoch;
            in.env = std::move(env);
            if (!pushInbound(std::move(in)))
            {
                Json p = Json::object();
                p.set("reason", Json::string("core_inbound_overflow"));
                sendEnv(MsgType::Reset, p, std::string());
                alive = false;
                disconnectReason = "core_inbound_overflow";
            }
        }
    }

    if (_stop)
        disconnectReason = "core_shutdown";

    if (!epoch.empty())
    {
        std::lock_guard<std::mutex> g(_mx);
        _epoch.clear();
        _outbound.clear();
        _inbound.clear();   // pending requests of the dead epoch are invalidated, never replayed
        Inbound in;
        in.kind = Inbound::Kind::Disconnected;
        in.epoch = epoch;
        in.reason = disconnectReason;
        _inbound.push(std::move(in));
    }
    log("host connection closed: " + disconnectReason);
}

// ===========================================================================
// HostClient
// ===========================================================================

HostClient::HostClient(Config cfg, LogFn log)
    : _cfg(std::move(cfg)), _log(std::move(log)), _inbound(_cfg.inboundCapacity), _outbound(_cfg.outboundCapacity)
{
}

HostClient::~HostClient()
{
    stop();
}

void HostClient::start()
{
    if (_thread.joinable())
        return;
    _stop = false;
    _thread = std::thread([this] { run(); });
}

void HostClient::stop()
{
    _stop = true;
    if (_thread.joinable())
        _thread.join();
}

bool HostClient::pollInbound(Inbound& out)
{
    std::lock_guard<std::mutex> g(_mx);
    return _inbound.pop(out);
}

std::string HostClient::epoch() const
{
    std::lock_guard<std::mutex> g(_mx);
    return _epoch;
}

std::string HostClient::nextRequestId(std::string const& prefix)
{
    std::lock_guard<std::mutex> g(_mx);
    return prefix + "-" + std::to_string(++_requestCounter);
}

bool HostClient::send(MsgType type, Json payload, std::string const& requestId, std::string const& coalesceKey)
{
    std::lock_guard<std::mutex> g(_mx);
    if (_epoch.empty())
        return false; // not connected: intents are not buffered across epochs
    Outbound o;
    o.epoch = _epoch;
    o.type = type;
    o.payload = std::move(payload);
    o.requestId = requestId;
    return _outbound.push(std::move(o), coalesceKey);
}

void HostClient::run()
{
    netInit();
    int backoffMs = 250;
    while (!_stop)
    {
        TcpStream stream;
        if (stream.connectLoopback(_cfg.port, 500))
        {
            backoffMs = 250;
            session(stream);
            stream.closeGraceful();
        }
        // Reconnect backoff, checking the stop flag frequently.
        for (int waited = 0; waited < backoffMs && !_stop; waited += 50)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        backoffMs = backoffMs < 4000 ? backoffMs * 2 : 4000;
    }
}

bool HostClient::session(TcpStream& stream)
{
    EndpointTimings const& t = _cfg.timings;
    FrameDecoder decoder;
    SeqTracker coreSeq;
    std::int64_t outSeq = 0;
    std::string epoch;
    std::string reason = "peer_closed";
    std::uint64_t pingCounter = 0;

    auto sendEnv = [&](MsgType type, Json payload, std::string const& requestId, std::int64_t seq) {
        Envelope e;
        e.type = type;
        e.epoch = epoch;
        e.seq = seq;
        e.hasRequestId = !requestId.empty();
        e.requestId = requestId;
        e.payload = std::move(payload);
        return stream.sendFrame(e.serialize(), t.writeTimeoutMs);
    };

    Json hello = Json::object();
    hello.set("adapter", Json::string(_cfg.adapter));
    hello.set("version", Json::integer(kProtocolVersion));
    hello.set("token", Json::string(_cfg.token));
    if (!sendEnv(MsgType::Hello, hello, std::string(), 0))
        return false;

    auto const started = Clock::now();
    auto lastRx = Clock::now();
    auto lastByte = Clock::now();
    auto lastPing = Clock::now();
    std::vector<char> buf(16384);
    bool alive = true;

    while (alive && !_stop)
    {
        if (!epoch.empty())
        {
            std::vector<Outbound> batch;
            bool overflow = false;
            {
                std::lock_guard<std::mutex> g(_mx);
                Outbound o;
                while (_outbound.pop(o))
                    batch.push_back(std::move(o));
                overflow = _outbound.overflowed();
                if (overflow)
                    _outbound.clear();
            }
            if (overflow)
            {
                Json p = Json::object();
                p.set("reason", Json::string("host_outbound_overflow"));
                sendEnv(MsgType::Reset, p, std::string(), ++outSeq);
                reason = "host_outbound_overflow";
                break;
            }
            for (Outbound& o : batch)
            {
                if (o.epoch != epoch)
                    continue;
                if (!sendEnv(o.type, std::move(o.payload), o.requestId, ++outSeq))
                {
                    alive = false;
                    reason = "write_failed";
                    break;
                }
            }
            if (!alive)
                break;
            if (msSince(lastPing) >= t.heartbeatMs)
            {
                Json p = Json::object();
                p.set("nonce", Json::string("h" + std::to_string(++pingCounter)));
                if (!sendEnv(MsgType::Ping, p, std::string(), ++outSeq))
                {
                    reason = "write_failed";
                    break;
                }
                lastPing = Clock::now();
            }
            if (msSince(lastRx) > t.heartbeatTimeoutMs)
            {
                reason = "heartbeat_timeout";
                break;
            }
        }
        else if (msSince(started) > t.helloTimeoutMs)
        {
            reason = "welcome_timeout";
            break;
        }

        int n = stream.recvSome(buf.data(), buf.size(), 10);
        if (n < 0)
            break;
        if (n == 0)
        {
            if (decoder.midFrame() && msSince(lastByte) > t.stallTimeoutMs)
            {
                reason = "stalled_mid_frame";
                break;
            }
            continue;
        }
        lastByte = Clock::now();
        if (!decoder.feed(buf.data(), static_cast<std::size_t>(n)))
        {
            reason = "framing_error";
            break;
        }
        while (alive && decoder.hasFrame())
        {
            DecodeResult d = decodeEnvelope(decoder.pop());
            if (!d.ok)
            {
                log("dropping invalid frame from core: " + d.code + " " + d.detail);
                continue;
            }
            Envelope& env = d.env;
            lastRx = Clock::now();
            if (msgDirection(env.type) == Direction::HostToCore)
            {
                log("dropping host-only message type from core");
                continue;
            }
            if (epoch.empty())
            {
                if (env.type == MsgType::Error)
                {
                    log("core rejected HELLO: " + env.payload.find("code")->asString());
                    reason = "rejected:" + env.payload.find("code")->asString();
                    alive = false;
                    break;
                }
                if (env.type != MsgType::Welcome || env.epoch.empty())
                {
                    reason = "expected_welcome";
                    alive = false;
                    break;
                }
                epoch = env.epoch;
                coreSeq.accept(env.seq);
                outSeq = 0; // HELLO was seq 0 in this epoch; continue from 1
                {
                    std::lock_guard<std::mutex> g(_mx);
                    _epoch = epoch;
                    _requestCounter = 0;
                    _outbound.clear();
                    _inbound.clear();
                    Inbound in;
                    in.kind = Inbound::Kind::Connected;
                    in.epoch = epoch;
                    in.env = env;
                    _inbound.push(std::move(in));
                }
                _connected = true;
                lastPing = Clock::now();
                continue;
            }
            if (env.epoch != epoch || !coreSeq.accept(env.seq))
            {
                log("dropping stale/out-of-order core message");
                continue;
            }
            if (env.type == MsgType::Ping)
            {
                Json p = Json::object();
                p.set("nonce", *env.payload.find("nonce"));
                sendEnv(MsgType::Pong, p, std::string(), ++outSeq);
                continue;
            }
            if (env.type == MsgType::Pong)
                continue;
            bool overflow;
            {
                std::lock_guard<std::mutex> g(_mx);
                Inbound in;
                in.kind = Inbound::Kind::Message;
                in.epoch = epoch;
                in.env = std::move(env);
                overflow = !_inbound.push(std::move(in));
            }
            if (overflow)
            {
                Json p = Json::object();
                p.set("reason", Json::string("host_inbound_overflow"));
                sendEnv(MsgType::Reset, p, std::string(), ++outSeq);
                reason = "host_inbound_overflow";
                alive = false;
            }
        }
    }

    _connected = false;
    if (!epoch.empty())
    {
        std::lock_guard<std::mutex> g(_mx);
        _epoch.clear();
        _outbound.clear();
        _inbound.clear();
        Inbound in;
        in.kind = Inbound::Kind::Disconnected;
        in.epoch = epoch;
        in.reason = reason;
        _inbound.push(std::move(in));
    }
    log("core connection closed: " + reason);
    return true;
}

} // namespace gamebridge
