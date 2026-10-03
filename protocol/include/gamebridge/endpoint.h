// GameBridge protocol v1 endpoints.
//
// CoreEndpoint: loopback server used inside worldserver (mod-gamebridge). Its IO
// thread owns the socket, performs framing/validation/handshake/epoch/seq/dedupe
// and heartbeats, and exchanges validated messages with the owning game thread
// through bounded queues. It never touches game objects.
//
// HostClient: the GTA-side (and bridge-cli) counterpart. Same rules: the IO
// thread never touches game state; the game thread polls validated messages.
#pragma once

#include "gamebridge/net.h"
#include "gamebridge/protocol.h"
#include "gamebridge/session.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace gamebridge {

struct EndpointTimings
{
    int heartbeatMs = 1000;
    int heartbeatTimeoutMs = 3000;
    int stallTimeoutMs = 2000;     // peer stalled mid-frame
    int helloTimeoutMs = 3000;
    int writeTimeoutMs = 2000;
};

using LogFn = std::function<void(std::string const&)>;

// ---------------------------------------------------------------------------

class CoreEndpoint
{
public:
    struct Config
    {
        std::uint16_t port = kDefaultPort;
        std::string token;             // per-install local secret; never logged
        std::size_t inboundCapacity = 128;
        std::size_t outboundCapacity = 512;
        std::size_t dedupeCapacity = 256;
        EndpointTimings timings;
    };

    struct Inbound
    {
        enum class Kind { Connected, Disconnected, Message };
        Kind kind = Kind::Message;
        std::string epoch;
        Envelope env;
        std::string reason;  // Disconnected reason
    };

    explicit CoreEndpoint(Config cfg, LogFn log = nullptr);
    ~CoreEndpoint();

    bool start();   // binds 127.0.0.1:port and starts the IO thread
    void stop();
    std::uint16_t port() const { return _boundPort; }

    // --- game thread API (thread-safe) ---
    bool pollInbound(Inbound& out);
    // Queue a message for the given epoch. Dropped silently if that epoch is no
    // longer current (stale). Returns false on overflow (the IO thread then
    // resets the connection; the next connection is a new epoch + full snapshot).
    bool send(std::string const& epoch, MsgType type, Json payload,
              std::string const& requestId = std::string(), std::string const& coalesceKey = std::string());
    std::string currentEpoch() const;

    // Diagnostics
    std::uint64_t connectionsAccepted() const { return _connections.load(); }

private:
    struct Outbound
    {
        std::string epoch;
        MsgType type = MsgType::Ping;
        Json payload;
        std::string requestId;
    };

    void run();
    void serveConnection(TcpStream& stream);
    bool pushInbound(Inbound in);
    void log(std::string const& s) const { if (_log) _log(s); }

    Config _cfg;
    LogFn _log;
    TcpListener _listener;
    std::uint16_t _boundPort = 0;
    std::thread _thread;
    std::atomic<bool> _stop { false };
    std::atomic<std::uint64_t> _connections { 0 };

    mutable std::mutex _mx;
    BoundedQueue<Inbound> _inbound;
    BoundedQueue<Outbound> _outbound;
    std::string _epoch;           // current epoch ("" when no accepted host)
    std::uint64_t _epochCounter = 0;
};

// ---------------------------------------------------------------------------

class HostClient
{
public:
    struct Config
    {
        std::uint16_t port = kDefaultPort;
        std::string token;
        std::string adapter = "gta-sa";
        std::size_t inboundCapacity = 512;
        std::size_t outboundCapacity = 256;
        EndpointTimings timings;
    };

    struct Inbound
    {
        enum class Kind { Connected, Disconnected, Message };
        Kind kind = Kind::Message;
        std::string epoch;
        Envelope env;
        std::string reason;
    };

    explicit HostClient(Config cfg, LogFn log = nullptr);
    ~HostClient();

    // Starts the IO thread, which connects (and reconnects with backoff) on its own.
    void start();
    void stop();

    // --- game thread API (thread-safe, non-blocking) ---
    bool pollInbound(Inbound& out);
    bool send(MsgType type, Json payload, std::string const& requestId = std::string(),
              std::string const& coalesceKey = std::string());
    bool connected() const { return _connected.load(); }
    std::string epoch() const;
    // Generates "<prefix>-<n>" with n strictly increasing within the epoch.
    std::string nextRequestId(std::string const& prefix);

private:
    struct Outbound
    {
        std::string epoch;
        MsgType type = MsgType::Ping;
        Json payload;
        std::string requestId;
    };

    void run();
    bool session(TcpStream& stream);
    void log(std::string const& s) const { if (_log) _log(s); }

    Config _cfg;
    LogFn _log;
    std::thread _thread;
    std::atomic<bool> _stop { false };
    std::atomic<bool> _connected { false };

    mutable std::mutex _mx;
    BoundedQueue<Inbound> _inbound;
    BoundedQueue<Outbound> _outbound;
    std::string _epoch;
    std::int64_t _requestCounter = 0;
};

std::string makeEpoch(std::uint64_t counter);

} // namespace gamebridge
