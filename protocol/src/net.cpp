#include "gamebridge/net.h"

#include <chrono>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using socklen_t = int;
#define GB_POLL WSAPoll
#define GB_CLOSE closesocket
static bool wouldBlock() { int e = WSAGetLastError(); return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS; }
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#define GB_POLL poll
#define GB_CLOSE ::close
static bool wouldBlock() { return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS || errno == EINTR; }
#endif

namespace gamebridge {

namespace {

using Clock = std::chrono::steady_clock;

int remainingMs(Clock::time_point deadline)
{
    auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    return left < 0 ? 0 : static_cast<int>(left);
}

bool setNonBlocking(SocketHandle s)
{
#ifdef _WIN32
    u_long on = 1;
    return ioctlsocket(static_cast<SOCKET>(s), FIONBIO, &on) == 0;
#else
    int flags = fcntl(s, F_GETFL, 0);
    return flags >= 0 && fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

void setNoDelay(SocketHandle s)
{
    int one = 1;
    setsockopt(static_cast<decltype(socket(0, 0, 0))>(s), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char const*>(&one), sizeof(one));
#ifndef _WIN32
#ifdef SO_NOSIGPIPE
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
#endif
}

int pollOne(SocketHandle s, short events, int timeoutMs)
{
#ifdef _WIN32
    WSAPOLLFD p;
    p.fd = static_cast<SOCKET>(s);
#else
    pollfd p;
    p.fd = s;
#endif
    p.events = events;
    p.revents = 0;
    int r = GB_POLL(&p, 1, timeoutMs);
    if (r <= 0)
        return r;
    if (p.revents & (POLLERR | POLLNVAL))
        return -1;
    return (p.revents & (events | POLLHUP)) ? 1 : 0;
}

} // namespace

bool netInit()
{
#ifdef _WIN32
    static bool done = false;
    if (!done)
    {
        WSADATA d;
        if (WSAStartup(MAKEWORD(2, 2), &d) != 0)
            return false;
        done = true;
    }
#endif
    return true;
}

SocketHandle TcpStream::invalid()
{
#ifdef _WIN32
    return static_cast<SocketHandle>(INVALID_SOCKET);
#else
    return -1;
#endif
}

TcpStream& TcpStream::operator=(TcpStream&& o) noexcept
{
    if (this != &o)
    {
        close();
        _s = o._s;
        o._s = invalid();
    }
    return *this;
}

void TcpStream::close()
{
    if (valid())
    {
        GB_CLOSE(static_cast<decltype(socket(0, 0, 0))>(_s));
        _s = invalid();
    }
}

void TcpStream::closeGraceful(int lingerMs)
{
    if (!valid())
        return;
#ifdef _WIN32
    ::shutdown(static_cast<SOCKET>(_s), SD_SEND);
#else
    ::shutdown(_s, SHUT_WR);
#endif
    auto deadline = Clock::now() + std::chrono::milliseconds(lingerMs);
    char buf[4096];
    for (;;)
    {
        int left = remainingMs(deadline);
        if (left == 0 || recvSome(buf, sizeof(buf), left) < 0)
            break;
    }
    close();
}

bool TcpStream::connectLoopback(std::uint16_t port, int timeoutMs)
{
    close();
    netInit();
    auto s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (static_cast<SocketHandle>(s) == invalid())
        return false;
    _s = static_cast<SocketHandle>(s);
    setNonBlocking(_s);
    setNoDelay(_s);
    sockaddr_in a;
    std::memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int r = ::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    if (r != 0)
    {
        if (!wouldBlock())
        {
            close();
            return false;
        }
        if (pollOne(_s, POLLOUT, timeoutMs) != 1)
        {
            close();
            return false;
        }
        int err = 0;
        socklen_t len = sizeof(err);
        getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
        if (err != 0)
        {
            close();
            return false;
        }
    }
    return true;
}

bool TcpStream::sendAll(char const* data, std::size_t len, int timeoutMs)
{
    if (!valid())
        return false;
    auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    std::size_t off = 0;
    while (off < len)
    {
#ifdef _WIN32
        int n = ::send(static_cast<SOCKET>(_s), data + off, static_cast<int>(len - off), 0);
#else
        ssize_t n = ::send(_s, data + off, len - off, MSG_NOSIGNAL);
#endif
        if (n > 0)
        {
            off += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && wouldBlock())
        {
            int left = remainingMs(deadline);
            if (left == 0 || pollOne(_s, POLLOUT, left) != 1)
                return false;
            continue;
        }
        return false;
    }
    return true;
}

bool TcpStream::sendFrame(std::string const& body, int timeoutMs)
{
    std::string f = encodeFrame(body);
    if (f.empty())
        return false;
    return sendAll(f.data(), f.size(), timeoutMs);
}

int TcpStream::recvSome(char* buf, std::size_t cap, int timeoutMs)
{
    if (!valid())
        return -1;
    int p = pollOne(_s, POLLIN, timeoutMs);
    if (p == 0)
        return 0;
    if (p < 0)
        return -1;
#ifdef _WIN32
    int n = ::recv(static_cast<SOCKET>(_s), buf, static_cast<int>(cap), 0);
#else
    ssize_t n = ::recv(_s, buf, cap, 0);
#endif
    if (n > 0)
        return static_cast<int>(n);
    if (n < 0 && wouldBlock())
        return 0;
    return -1;
}

bool TcpListener::listenLoopback(std::uint16_t port)
{
    close();
    netInit();
    auto s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (static_cast<SocketHandle>(s) == TcpStream::invalid())
        return false;
    _s = static_cast<SocketHandle>(s);
#ifdef _WIN32
    BOOL excl = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<char const*>(&excl), sizeof(excl));
#else
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#endif
    sockaddr_in a;
    std::memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // never 0.0.0.0
    if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || ::listen(s, 4) != 0)
    {
        close();
        return false;
    }
    setNonBlocking(_s);
    socklen_t len = sizeof(a);
    getsockname(s, reinterpret_cast<sockaddr*>(&a), &len);
    _port = ntohs(a.sin_port);
    return true;
}

bool TcpListener::accept(TcpStream& out, int timeoutMs)
{
    if (_s == TcpStream::invalid())
        return false;
    if (pollOne(_s, POLLIN, timeoutMs) != 1)
        return false;
    sockaddr_in peer;
    socklen_t len = sizeof(peer);
    auto c = ::accept(static_cast<decltype(socket(0, 0, 0))>(_s), reinterpret_cast<sockaddr*>(&peer), &len);
    if (static_cast<SocketHandle>(c) == TcpStream::invalid())
        return false;
    TcpStream stream(static_cast<SocketHandle>(c));
    if (peer.sin_family != AF_INET || ntohl(peer.sin_addr.s_addr) != INADDR_LOOPBACK)
        return false;  // stream destructor closes it
    setNonBlocking(static_cast<SocketHandle>(c));
    setNoDelay(static_cast<SocketHandle>(c));
    out = std::move(stream);
    return true;
}

void TcpListener::close()
{
    if (_s != TcpStream::invalid())
    {
        GB_CLOSE(static_cast<decltype(socket(0, 0, 0))>(_s));
        _s = TcpStream::invalid();
    }
}

} // namespace gamebridge
