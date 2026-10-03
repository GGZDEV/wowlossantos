#include "gamebridge/json.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace gamebridge {

Json Json::boolean(bool b)
{
    Json j;
    j._type = Type::Bool;
    j._bool = b;
    return j;
}

Json Json::number(double d)
{
    Json j;
    j._type = Type::Number;
    j._num = d;
    // Keep integral doubles printable as integers when they are exact.
    if (std::isfinite(d) && d == std::floor(d) && std::fabs(d) <= 9007199254740991.0)
    {
        j._isInt = true;
        j._int = static_cast<std::int64_t>(d);
    }
    return j;
}

Json Json::integer(std::int64_t i)
{
    Json j;
    j._type = Type::Number;
    j._isInt = true;
    j._int = i;
    j._num = static_cast<double>(i);
    return j;
}

Json Json::string(std::string s)
{
    Json j;
    j._type = Type::String;
    j._str = std::move(s);
    return j;
}

Json Json::array()
{
    Json j;
    j._type = Type::Array;
    return j;
}

Json Json::object()
{
    Json j;
    j._type = Type::Object;
    return j;
}

Json const* Json::find(std::string const& key) const
{
    if (_type != Type::Object)
        return nullptr;
    for (Member const& m : _obj)
        if (m.first == key)
            return &m.second;
    return nullptr;
}

Json& Json::set(std::string key, Json value)
{
    if (_type != Type::Object)
        *this = object();
    for (Member& m : _obj)
        if (m.first == key)
        {
            m.second = std::move(value);
            return m.second;
        }
    _obj.emplace_back(std::move(key), std::move(value));
    return _obj.back().second;
}

Json& Json::push(Json value)
{
    if (_type != Type::Array)
        *this = array();
    _arr.push_back(std::move(value));
    return _arr.back();
}

bool Json::operator==(Json const& o) const
{
    if (_type != o._type)
        return false;
    switch (_type)
    {
        case Type::Null: return true;
        case Type::Bool: return _bool == o._bool;
        case Type::Number:
            if (_isInt && o._isInt)
                return _int == o._int;
            return _num == o._num;
        case Type::String: return _str == o._str;
        case Type::Array: return _arr == o._arr;
        case Type::Object:
        {
            // Key order is not semantically relevant.
            if (_obj.size() != o._obj.size())
                return false;
            for (Member const& m : _obj)
            {
                Json const* other = o.find(m.first);
                if (!other || !(*other == m.second))
                    return false;
            }
            return true;
        }
    }
    return false;
}

static void dumpString(std::string const& s, std::string& out)
{
    out.push_back('"');
    for (unsigned char c : s)
    {
        switch (c)
        {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20)
                {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                }
                else
                    out.push_back(static_cast<char>(c));
        }
    }
    out.push_back('"');
}

void Json::dumpTo(std::string& out) const
{
    switch (_type)
    {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += _bool ? "true" : "false"; break;
        case Type::Number:
        {
            char buf[40];
            if (_isInt)
                std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(_int));
            else if (std::isfinite(_num))
                std::snprintf(buf, sizeof(buf), "%.17g", _num);
            else
                std::snprintf(buf, sizeof(buf), "null"); // never emit non-finite numbers
            out += buf;
            break;
        }
        case Type::String: dumpString(_str, out); break;
        case Type::Array:
        {
            out.push_back('[');
            bool first = true;
            for (Json const& v : _arr)
            {
                if (!first)
                    out.push_back(',');
                first = false;
                v.dumpTo(out);
            }
            out.push_back(']');
            break;
        }
        case Type::Object:
        {
            out.push_back('{');
            bool first = true;
            for (Member const& m : _obj)
            {
                if (!first)
                    out.push_back(',');
                first = false;
                dumpString(m.first, out);
                out.push_back(':');
                m.second.dumpTo(out);
            }
            out.push_back('}');
            break;
        }
    }
}

std::string Json::dump() const
{
    std::string out;
    dumpTo(out);
    return out;
}

bool isValidUtf8(std::string const& s)
{
    std::size_t i = 0;
    std::size_t const n = s.size();
    while (i < n)
    {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80)
        {
            ++i;
            continue;
        }
        std::size_t len;
        std::uint32_t cp;
        if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; }
        else return false;
        if (i + len > n)
            return false;
        for (std::size_t k = 1; k < len; ++k)
        {
            unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80)
                return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000))
            return false; // overlong
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            return false;
        i += len;
    }
    return true;
}

class JsonParser
{
public:
    JsonParser(std::string const& t, int maxDepth) : _t(t), _maxDepth(maxDepth) { }

    JsonParseResult run()
    {
        JsonParseResult r;
        if (!isValidUtf8(_t))
        {
            r.error = "invalid_utf8";
            return r;
        }
        skipWs();
        if (!parseValue(r.value, 0))
        {
            r.error = _err;
            return r;
        }
        skipWs();
        if (_p != _t.size())
        {
            r.error = "trailing_data";
            return r;
        }
        r.ok = true;
        return r;
    }

private:
    std::string const& _t;
    std::size_t _p = 0;
    int _maxDepth;
    std::string _err;

    bool fail(char const* e)
    {
        if (_err.empty())
            _err = e;
        return false;
    }

    void skipWs()
    {
        while (_p < _t.size() && (_t[_p] == ' ' || _t[_p] == '\t' || _t[_p] == '\n' || _t[_p] == '\r'))
            ++_p;
    }

    bool literal(char const* word)
    {
        std::size_t len = std::strlen(word);
        if (_t.compare(_p, len, word) != 0)
            return fail("invalid_literal");
        _p += len;
        return true;
    }

    bool parseValue(Json& out, int depth)
    {
        if (_p >= _t.size())
            return fail("unexpected_end");
        char c = _t[_p];
        switch (c)
        {
            case '{': return parseObject(out, depth + 1);
            case '[': return parseArray(out, depth + 1);
            case '"':
                out = Json::string("");
                return parseString(out._str);
            case 't': out = Json::boolean(true); return literal("true");
            case 'f': out = Json::boolean(false); return literal("false");
            case 'n': out = Json(); return literal("null");
            default:
                if (c == '-' || (c >= '0' && c <= '9'))
                    return parseNumber(out);
                return fail("unexpected_character");
        }
    }

    bool parseObject(Json& out, int depth)
    {
        if (depth > _maxDepth)
            return fail("nesting_too_deep");
        out = Json::object();
        ++_p; // '{'
        skipWs();
        if (_p < _t.size() && _t[_p] == '}')
        {
            ++_p;
            return true;
        }
        for (;;)
        {
            skipWs();
            if (_p >= _t.size() || _t[_p] != '"')
                return fail("expected_key");
            std::string key;
            if (!parseString(key))
                return false;
            if (out.find(key))
                return fail("duplicate_key");
            skipWs();
            if (_p >= _t.size() || _t[_p] != ':')
                return fail("expected_colon");
            ++_p;
            skipWs();
            Json v;
            if (!parseValue(v, depth))
                return false;
            out._obj.emplace_back(std::move(key), std::move(v));
            skipWs();
            if (_p >= _t.size())
                return fail("unexpected_end");
            if (_t[_p] == ',')
            {
                ++_p;
                continue;
            }
            if (_t[_p] == '}')
            {
                ++_p;
                return true;
            }
            return fail("expected_comma_or_brace");
        }
    }

    bool parseArray(Json& out, int depth)
    {
        if (depth > _maxDepth)
            return fail("nesting_too_deep");
        out = Json::array();
        ++_p; // '['
        skipWs();
        if (_p < _t.size() && _t[_p] == ']')
        {
            ++_p;
            return true;
        }
        for (;;)
        {
            skipWs();
            Json v;
            if (!parseValue(v, depth))
                return false;
            out._arr.push_back(std::move(v));
            skipWs();
            if (_p >= _t.size())
                return fail("unexpected_end");
            if (_t[_p] == ',')
            {
                ++_p;
                continue;
            }
            if (_t[_p] == ']')
            {
                ++_p;
                return true;
            }
            return fail("expected_comma_or_bracket");
        }
    }

    bool hex4(std::uint32_t& v)
    {
        if (_p + 4 > _t.size())
            return fail("bad_escape");
        v = 0;
        for (int i = 0; i < 4; ++i)
        {
            char c = _t[_p++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<std::uint32_t>(c - 'A' + 10);
            else return fail("bad_escape");
        }
        return true;
    }

    static void appendUtf8(std::string& s, std::uint32_t cp)
    {
        if (cp < 0x80)
            s.push_back(static_cast<char>(cp));
        else if (cp < 0x800)
        {
            s.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else if (cp < 0x10000)
        {
            s.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else
        {
            s.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool parseString(std::string& out)
    {
        ++_p; // opening quote
        for (;;)
        {
            if (_p >= _t.size())
                return fail("unterminated_string");
            unsigned char c = static_cast<unsigned char>(_t[_p++]);
            if (c == '"')
                return true;
            if (c < 0x20)
                return fail("control_char_in_string");
            if (c != '\\')
            {
                out.push_back(static_cast<char>(c));
                continue;
            }
            if (_p >= _t.size())
                return fail("bad_escape");
            char e = _t[_p++];
            switch (e)
            {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u':
                {
                    std::uint32_t cp;
                    if (!hex4(cp))
                        return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF)
                    {
                        if (_p + 2 > _t.size() || _t[_p] != '\\' || _t[_p + 1] != 'u')
                            return fail("lone_surrogate");
                        _p += 2;
                        std::uint32_t lo;
                        if (!hex4(lo))
                            return false;
                        if (lo < 0xDC00 || lo > 0xDFFF)
                            return fail("lone_surrogate");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    else if (cp >= 0xDC00 && cp <= 0xDFFF)
                        return fail("lone_surrogate");
                    appendUtf8(out, cp);
                    break;
                }
                default:
                    return fail("bad_escape");
            }
        }
    }

    bool parseNumber(Json& out)
    {
        std::size_t start = _p;
        bool integral = true;
        if (_t[_p] == '-')
            ++_p;
        if (_p >= _t.size())
            return fail("bad_number");
        if (_t[_p] == '0')
            ++_p;
        else if (_t[_p] >= '1' && _t[_p] <= '9')
            while (_p < _t.size() && _t[_p] >= '0' && _t[_p] <= '9')
                ++_p;
        else
            return fail("bad_number");
        if (_p < _t.size() && _t[_p] == '.')
        {
            integral = false;
            ++_p;
            if (_p >= _t.size() || !(_t[_p] >= '0' && _t[_p] <= '9'))
                return fail("bad_number");
            while (_p < _t.size() && _t[_p] >= '0' && _t[_p] <= '9')
                ++_p;
        }
        if (_p < _t.size() && (_t[_p] == 'e' || _t[_p] == 'E'))
        {
            integral = false;
            ++_p;
            if (_p < _t.size() && (_t[_p] == '+' || _t[_p] == '-'))
                ++_p;
            if (_p >= _t.size() || !(_t[_p] >= '0' && _t[_p] <= '9'))
                return fail("bad_number");
            while (_p < _t.size() && _t[_p] >= '0' && _t[_p] <= '9')
                ++_p;
        }
        std::string tok = _t.substr(start, _p - start);
        out = Json();
        out._type = Json::Type::Number;
        // strtod is locale dependent for the decimal point; the protocol is ASCII '.',
        // and none of our processes call setlocale(LC_NUMERIC), so "C" applies.
        out._num = std::strtod(tok.c_str(), nullptr);
        if (!std::isfinite(out._num))
            return fail("non_finite_number");
        if (integral)
        {
            errno = 0;
            long long v = std::strtoll(tok.c_str(), nullptr, 10);
            if (errno == 0)
            {
                out._isInt = true;
                out._int = static_cast<std::int64_t>(v);
            }
        }
        return true;
    }
};

JsonParseResult parseJson(std::string const& text, int maxDepth)
{
    return JsonParser(text, maxDepth).run();
}

} // namespace gamebridge
