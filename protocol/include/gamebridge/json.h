// GameBridge protocol v1 - strict, dependency-free JSON value/parser/serializer.
//
// The parser is deliberately stricter than RFC 8259 requires:
//  - input must be valid UTF-8 (overlongs, surrogates and > U+10FFFF rejected)
//  - duplicate object keys are rejected
//  - nesting depth is bounded (default 16)
//  - numbers that overflow to +/-inf are rejected (non-finite numbers)
//  - only one top-level value; trailing non-whitespace is rejected
// No pointers or platform-sized integers are part of the serialized form.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace gamebridge {

class Json
{
public:
    enum class Type : std::uint8_t { Null, Bool, Number, String, Array, Object };
    using Member = std::pair<std::string, Json>;

    Json() = default;
    static Json null() { return Json(); }
    static Json boolean(bool b);
    static Json number(double d);
    static Json integer(std::int64_t i);
    static Json string(std::string s);
    static Json array();
    static Json object();

    Type type() const { return _type; }
    bool isNull() const { return _type == Type::Null; }
    bool isBool() const { return _type == Type::Bool; }
    bool isNumber() const { return _type == Type::Number; }
    bool isString() const { return _type == Type::String; }
    bool isArray() const { return _type == Type::Array; }
    bool isObject() const { return _type == Type::Object; }

    // True when the number was written without fraction/exponent and fits int64.
    bool isInteger() const { return _type == Type::Number && _isInt; }

    bool asBool() const { return _bool; }
    double asDouble() const { return _num; }
    std::int64_t asInt() const { return _int; }
    std::string const& asString() const { return _str; }
    std::vector<Json> const& items() const { return _arr; }
    std::vector<Member> const& members() const { return _obj; }

    // Object helpers. find() returns nullptr when absent or when this is not an object.
    Json const* find(std::string const& key) const;
    Json& set(std::string key, Json value);   // replaces existing key
    Json& push(Json value);                    // array append

    std::string dump() const;

    bool operator==(Json const& other) const;
    bool operator!=(Json const& other) const { return !(*this == other); }

private:
    Type _type = Type::Null;
    bool _bool = false;
    bool _isInt = false;
    double _num = 0.0;
    std::int64_t _int = 0;
    std::string _str;
    std::vector<Json> _arr;
    std::vector<Member> _obj;

    void dumpTo(std::string& out) const;
    friend class JsonParser;
};

struct JsonParseResult
{
    bool ok = false;
    Json value;
    std::string error;   // safe, short description (never echoes input)
};

constexpr int kDefaultMaxJsonDepth = 16;

JsonParseResult parseJson(std::string const& text, int maxDepth = kDefaultMaxJsonDepth);
bool isValidUtf8(std::string const& s);

} // namespace gamebridge
