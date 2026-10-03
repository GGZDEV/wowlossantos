// Minimal loopback-only TCP wrapper (POSIX + Winsock). Non-blocking sockets with
// poll-based timeouts so IO threads never block indefinitely and can observe a
// stop flag. Never binds anything other than 127.0.0.1.
#pragma once

#include "gamebridge/framing.h"

#include <cstdint>
#include <string>

namespace gamebridge {

#ifdef _WIN32
using SocketHandle = std::uintptr_t;
#else
using SocketHandle = int;
#endif

bool netInit();   // WSAStartup on Windows; no-op elsewhere

class TcpStream
{
public:
    TcpStream() = default;
    explicit TcpStream(SocketHandle s) : _s(s) { }
    ~TcpStream() { close(); }
    TcpStream(TcpStream&& o) noexcept : _s(o._s) { o._s = invalid(); }
    TcpStream& operator=(TcpStream&& o) noexcept;
    TcpStream(TcpStream const&) = delete;
    TcpStream& operator=(TcpStream const&) = delete;

    static SocketHandle invalid();
    bool valid() const { return _s != invalid(); }

    // Connect to 127.0.0.1:port within timeoutMs.
    bool connectLoopback(std::uint16_t port, int timeoutMs);

    // Write everything (handles partial writes) within timeoutMs total.
    bool sendAll(char const* data, std::size_t len, int timeoutMs);
    bool sendFrame(std::string const& body, int timeoutMs);

    // Wait up to timeoutMs for data. Returns >0 bytes read, 0 on timeout,
    // -1 on orderly close or error.
    int recvSome(char* buf, std::size_t cap, int timeoutMs);

    // Half-close for writing and drain incoming bytes for up to lingerMs before
    // closing, so a final ERROR/RESET is not destroyed by a TCP reset caused by
    // unread input.
    void closeGraceful(int lingerMs = 200);
    void close();

private:
    SocketHandle _s = invalid();
};

class TcpListener
{
public:
    ~TcpListener() { close(); }
    bool listenLoopback(std::uint16_t port);   // binds 127.0.0.1 only
    // Waits up to timeoutMs for a client. Rejects non-loopback peers.
    bool accept(TcpStream& out, int timeoutMs);
    std::uint16_t port() const { return _port; }
    void close();

private:
    SocketHandle _s = TcpStream::invalid();
    std::uint16_t _port = 0;
};

} // namespace gamebridge
