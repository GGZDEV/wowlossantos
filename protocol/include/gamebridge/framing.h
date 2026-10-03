// GameBridge protocol v1 framing: 4-byte unsigned big-endian length N, then N bytes
// of UTF-8 JSON, 1 <= N <= kMaxFrameBytes. Independent of pointer size and ABI.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>

namespace gamebridge {

constexpr std::uint32_t kMaxFrameBytes = 65536;

// Returns header+body. Precondition: 1 <= body.size() <= maxFrame (otherwise empty string).
std::string encodeFrame(std::string const& body, std::uint32_t maxFrame = kMaxFrameBytes);

// Incremental decoder. Feed arbitrary byte chunks (split headers, split bodies,
// several frames per chunk). A protocol violation latches an error; the
// connection must then be dropped (no resynchronisation inside a byte stream).
class FrameDecoder
{
public:
    enum class Error { None, ZeroLength, Oversize };

    explicit FrameDecoder(std::uint32_t maxFrame = kMaxFrameBytes) : _max(maxFrame) { }

    // Returns false if the stream is (now) in error.
    bool feed(char const* data, std::size_t len);

    bool hasFrame() const { return !_frames.empty(); }
    std::string pop();

    Error error() const { return _error; }
    // True while a frame header or body is partially received (used for stall timeouts).
    bool midFrame() const { return _haveLen || _hdrFill > 0; }
    std::size_t pending() const { return _frames.size(); }

private:
    std::uint32_t _max;
    unsigned char _hdr[4] = { 0, 0, 0, 0 };
    std::size_t _hdrFill = 0;
    bool _haveLen = false;
    std::uint32_t _need = 0;
    std::string _body;
    std::deque<std::string> _frames;
    Error _error = Error::None;
};

} // namespace gamebridge
