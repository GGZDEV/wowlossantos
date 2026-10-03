#include "gamebridge/framing.h"

namespace gamebridge {

std::string encodeFrame(std::string const& body, std::uint32_t maxFrame)
{
    if (body.empty() || body.size() > maxFrame)
        return std::string();
    std::uint32_t n = static_cast<std::uint32_t>(body.size());
    std::string out;
    out.reserve(4 + body.size());
    out.push_back(static_cast<char>((n >> 24) & 0xFF));
    out.push_back(static_cast<char>((n >> 16) & 0xFF));
    out.push_back(static_cast<char>((n >> 8) & 0xFF));
    out.push_back(static_cast<char>(n & 0xFF));
    out += body;
    return out;
}

bool FrameDecoder::feed(char const* data, std::size_t len)
{
    if (_error != Error::None)
        return false;
    std::size_t i = 0;
    while (i < len)
    {
        if (!_haveLen)
        {
            _hdr[_hdrFill++] = static_cast<unsigned char>(data[i++]);
            if (_hdrFill < 4)
                continue;
            _need = (std::uint32_t(_hdr[0]) << 24) | (std::uint32_t(_hdr[1]) << 16) |
                    (std::uint32_t(_hdr[2]) << 8) | std::uint32_t(_hdr[3]);
            _hdrFill = 0;
            if (_need == 0)
            {
                _error = Error::ZeroLength;
                return false;
            }
            if (_need > _max)
            {
                _error = Error::Oversize;
                return false;
            }
            _haveLen = true;
            _body.clear();
            _body.reserve(_need);
            continue;
        }
        std::size_t take = len - i;
        std::size_t want = _need - _body.size();
        if (take > want)
            take = want;
        _body.append(data + i, take);
        i += take;
        if (_body.size() == _need)
        {
            _frames.push_back(std::move(_body));
            _body = std::string();
            _haveLen = false;
        }
    }
    return true;
}

std::string FrameDecoder::pop()
{
    std::string f = std::move(_frames.front());
    _frames.pop_front();
    return f;
}

} // namespace gamebridge
