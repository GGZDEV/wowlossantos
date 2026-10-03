#include "gamebridge/protocol.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <vector>

namespace gamebridge {

namespace {

struct TypeInfo
{
    MsgType type;
    char const* name;
    Direction dir;
};

constexpr TypeInfo kTypes[] = {
    { MsgType::Hello, "HELLO", Direction::HostToCore },
    { MsgType::Welcome, "WELCOME", Direction::CoreToHost },
    { MsgType::EnterTest, "ENTER_TEST", Direction::HostToCore },
    { MsgType::EntityBind, "ENTITY_BIND", Direction::CoreToHost },
    { MsgType::HostBindAck, "HOST_BIND_ACK", Direction::HostToCore },
    { MsgType::Position, "POSITION", Direction::HostToCore },
    { MsgType::CastRequest, "CAST_REQUEST", Direction::HostToCore },
    { MsgType::CastStatus, "CAST_STATUS", Direction::CoreToHost },
    { MsgType::StateSnapshot, "STATE_SNAPSHOT", Direction::CoreToHost },
    { MsgType::CombatEvent, "COMBAT_EVENT", Direction::CoreToHost },
    { MsgType::HostEntityLost, "HOST_ENTITY_LOST", Direction::HostToCore },
    { MsgType::EntityDespawn, "ENTITY_DESPAWN", Direction::CoreToHost },
    { MsgType::SetPaused, "SET_PAUSED", Direction::HostToCore },
    { MsgType::PauseAck, "PAUSE_ACK", Direction::CoreToHost },
    { MsgType::Resync, "RESYNC", Direction::HostToCore },
    { MsgType::Reset, "RESET", Direction::Either },
    { MsgType::Ping, "PING", Direction::Either },
    { MsgType::Pong, "PONG", Direction::Either },
    { MsgType::Error, "ERROR", Direction::CoreToHost },
};

static_assert(sizeof(kTypes) / sizeof(kTypes[0]) == static_cast<std::size_t>(MsgType::Count), "type table");

// ---- field schema ---------------------------------------------------------

enum class Kind
{
    String,        // non-empty unless minLen==0
    StringOrNull,
    SafeUInt,      // integer 0..2^53-1
    UInt32,        // integer 0..2^32-1
    Int32,
    Bool,
    Finite,        // any finite number
    Enum,
    Position,      // {x,y,z} finite
    StringArray,
    EntityStates,  // STATE_SNAPSHOT entities
};

struct Field
{
    char const* name;
    Kind kind;
    bool required;
    std::size_t maxLen = 128;
    std::vector<char const*> enumValues = {};
};

bool checkField(Field const& f, Json const& v, std::string& detail);

bool checkObject(Json const& obj, std::vector<Field> const& fields, std::string& detail)
{
    if (!obj.isObject())
    {
        detail = "payload_not_object";
        return false;
    }
    for (Json::Member const& m : obj.members())
    {
        bool known = std::any_of(fields.begin(), fields.end(), [&](Field const& f) { return m.first == f.name; });
        if (!known)
        {
            detail = "unknown_field:" + m.first.substr(0, 32);
            return false;
        }
    }
    for (Field const& f : fields)
    {
        Json const* v = obj.find(f.name);
        if (!v)
        {
            if (f.required)
            {
                detail = std::string("missing_field:") + f.name;
                return false;
            }
            continue;
        }
        if (!checkField(f, *v, detail))
            return false;
    }
    return true;
}

bool checkPosition(Json const& v)
{
    if (!v.isObject() || v.members().size() != 3)
        return false;
    for (char const* k : { "x", "y", "z" })
    {
        Json const* c = v.find(k);
        if (!c || !c->isNumber() || !std::isfinite(c->asDouble()) || std::fabs(c->asDouble()) > 1.0e6)
            return false;
    }
    return true;
}

std::vector<Field> const& entityStateFields()
{
    static std::vector<Field> const f = {
        { "entity_id", Kind::String, true, 64 },
        { "generation", Kind::SafeUInt, true },
        { "revision", Kind::SafeUInt, true },
        { "hp", Kind::UInt32, true },
        { "max_hp", Kind::UInt32, true },
        { "alive", Kind::Bool, true },
        { "power_type", Kind::Enum, false, 16, { "mana", "rage", "focus", "energy", "happiness", "runes", "runic_power", "none" } },
        { "power", Kind::UInt32, false },
        { "max_power", Kind::UInt32, false },
        { "casting_spell_id", Kind::UInt32, false },
        { "position", Kind::Position, false },
        { "orientation", Kind::Finite, false },
    };
    return f;
}

bool checkField(Field const& f, Json const& v, std::string& detail)
{
    bool ok = false;
    switch (f.kind)
    {
        case Kind::String:
            ok = v.isString() && !v.asString().empty() && v.asString().size() <= f.maxLen;
            break;
        case Kind::StringOrNull:
            ok = v.isNull() || (v.isString() && v.asString().size() <= f.maxLen);
            break;
        case Kind::SafeUInt:
            ok = v.isInteger() && v.asInt() >= 0 && v.asInt() <= kMaxSafeInteger;
            break;
        case Kind::UInt32:
            ok = v.isInteger() && v.asInt() >= 0 && v.asInt() <= 0xFFFFFFFFLL;
            break;
        case Kind::Int32:
            ok = v.isInteger() && v.asInt() >= -2147483648LL && v.asInt() <= 2147483647LL;
            break;
        case Kind::Bool:
            ok = v.isBool();
            break;
        case Kind::Finite:
            ok = v.isNumber() && std::isfinite(v.asDouble());
            break;
        case Kind::Enum:
            ok = v.isString() && std::any_of(f.enumValues.begin(), f.enumValues.end(),
                                             [&](char const* e) { return v.asString() == e; });
            break;
        case Kind::Position:
            ok = checkPosition(v);
            break;
        case Kind::StringArray:
            ok = v.isArray() && v.items().size() <= 32 &&
                 std::all_of(v.items().begin(), v.items().end(), [&](Json const& s) {
                     return s.isString() && !s.asString().empty() && s.asString().size() <= f.maxLen;
                 });
            break;
        case Kind::EntityStates:
        {
            if (!v.isArray() || v.items().size() > 64)
                break;
            ok = true;
            for (Json const& e : v.items())
            {
                std::string d;
                if (!checkObject(e, entityStateFields(), d))
                {
                    detail = std::string("invalid_entity_state:") + d;
                    return false;
                }
            }
            break;
        }
    }
    if (!ok)
        detail = std::string("invalid_field:") + f.name;
    return ok;
}

std::vector<Field> const& payloadFields(MsgType t)
{
    static std::vector<Field> const hello = {
        { "adapter", Kind::String, true, 64 },
        { "version", Kind::SafeUInt, true },
        { "token", Kind::String, true, 256 },
    };
    static std::vector<Field> const welcome = {
        { "core_sha", Kind::String, true, 64 },
        { "capabilities", Kind::StringArray, true, 64 },
        { "character_id", Kind::String, true, 64 },
        { "player_entity_id", Kind::String, true, 64 },
        { "data_fixture", Kind::String, true, 128 },
        { "spell_ids", Kind::StringArray, false, 16 },
    };
    static std::vector<Field> const enterTest = {
        { "test_fixture_id", Kind::String, true, 64 },
        { "observed_player_position", Kind::Position, true },
    };
    static std::vector<Field> const entityBind = {
        { "entity_id", Kind::String, true, 64 },
        { "kind", Kind::Enum, true, 16, { "player", "creature" } },
        { "core_guid", Kind::String, true, 64 },
        { "template_tag", Kind::String, true, 64 },
        { "generation", Kind::SafeUInt, true },
        { "position", Kind::Position, false },
        { "host_position", Kind::Position, false },
    };
    static std::vector<Field> const hostBindAck = {
        { "entity_id", Kind::String, true, 64 },
        { "host_generation", Kind::SafeUInt, true },
    };
    static std::vector<Field> const position = {
        { "entity_id", Kind::String, true, 64 },
        { "generation", Kind::SafeUInt, true },
        { "sample_id", Kind::SafeUInt, true },
        { "position", Kind::Position, true },
        { "orientation", Kind::Finite, true },
    };
    static std::vector<Field> const castRequest = {
        { "caster_id", Kind::String, true, 64 },
        { "target_id", Kind::String, true, 64 },
        { "spell_id", Kind::UInt32, true },
        { "host_generation", Kind::SafeUInt, true },
    };
    static std::vector<Field> const castStatus = {
        { "status", Kind::Enum, true, 16, { "accepted", "rejected", "interrupted", "completed" } },
        { "reason", Kind::StringOrNull, true, 64 },
        { "spell_id", Kind::UInt32, true },
        { "native_result", Kind::Int32, false },
    };
    static std::vector<Field> const stateSnapshot = {
        { "full", Kind::Bool, true },
        { "revision", Kind::SafeUInt, true },
        { "entities", Kind::EntityStates, true },
    };
    static std::vector<Field> const combatEvent = {
        { "event_id", Kind::String, true, 64 },
        { "kind", Kind::Enum, true, 16, { "damage", "heal", "death", "cast_start", "cast_go", "cast_failed" } },
        { "source_id", Kind::StringOrNull, true, 64 },
        { "target_id", Kind::StringOrNull, true, 64 },
        { "amount", Kind::UInt32, true },
        { "school", Kind::UInt32, true },
        { "critical", Kind::Bool, true },
        { "spell_id", Kind::UInt32, false },
    };
    static std::vector<Field> const hostEntityLost = {
        { "entity_id", Kind::String, true, 64 },
        { "generation", Kind::SafeUInt, true },
        { "reason", Kind::String, true, 64 },
    };
    static std::vector<Field> const entityDespawn = {
        { "entity_id", Kind::String, true, 64 },
        { "final_revision", Kind::SafeUInt, true },
        { "reason", Kind::String, true, 64 },
    };
    static std::vector<Field> const setPaused = {
        { "paused", Kind::Bool, true },
        { "reason", Kind::String, true, 64 },
    };
    static std::vector<Field> const pauseAck = {
        { "paused", Kind::Bool, true },
        { "final_revision", Kind::SafeUInt, true },
    };
    static std::vector<Field> const resync = {
        { "last_revision", Kind::SafeUInt, true },
        { "reason", Kind::String, true, 64 },
    };
    static std::vector<Field> const reset = {
        { "reason", Kind::String, true, 64 },
    };
    static std::vector<Field> const ping = {
        { "nonce", Kind::String, true, 64 },
    };
    static std::vector<Field> const error = {
        { "code", Kind::String, true, 64 },
        { "related_request_id", Kind::StringOrNull, true, 128 },
        { "description", Kind::String, true, 256 },
    };

    switch (t)
    {
        case MsgType::Hello: return hello;
        case MsgType::Welcome: return welcome;
        case MsgType::EnterTest: return enterTest;
        case MsgType::EntityBind: return entityBind;
        case MsgType::HostBindAck: return hostBindAck;
        case MsgType::Position: return position;
        case MsgType::CastRequest: return castRequest;
        case MsgType::CastStatus: return castStatus;
        case MsgType::StateSnapshot: return stateSnapshot;
        case MsgType::CombatEvent: return combatEvent;
        case MsgType::HostEntityLost: return hostEntityLost;
        case MsgType::EntityDespawn: return entityDespawn;
        case MsgType::SetPaused: return setPaused;
        case MsgType::PauseAck: return pauseAck;
        case MsgType::Resync: return resync;
        case MsgType::Reset: return reset;
        case MsgType::Ping:
        case MsgType::Pong: return ping;
        case MsgType::Error:
        case MsgType::Count: break;
    }
    return error;
}

void canonicalDump(Json const& v, std::string& out)
{
    if (v.isObject())
    {
        std::vector<Json::Member const*> ms;
        for (Json::Member const& m : v.members())
            ms.push_back(&m);
        std::sort(ms.begin(), ms.end(), [](Json::Member const* a, Json::Member const* b) { return a->first < b->first; });
        out.push_back('{');
        for (std::size_t i = 0; i < ms.size(); ++i)
        {
            if (i)
                out.push_back(',');
            out += Json::string(ms[i]->first).dump();
            out.push_back(':');
            canonicalDump(ms[i]->second, out);
        }
        out.push_back('}');
    }
    else if (v.isArray())
    {
        out.push_back('[');
        for (std::size_t i = 0; i < v.items().size(); ++i)
        {
            if (i)
                out.push_back(',');
            canonicalDump(v.items()[i], out);
        }
        out.push_back(']');
    }
    else
        out += v.dump();
}

} // namespace

char const* msgTypeName(MsgType t)
{
    for (TypeInfo const& i : kTypes)
        if (i.type == t)
            return i.name;
    return "?";
}

bool msgTypeFromName(std::string const& name, MsgType& out)
{
    for (TypeInfo const& i : kTypes)
        if (name == i.name)
        {
            out = i.type;
            return true;
        }
    return false;
}

Direction msgDirection(MsgType t)
{
    for (TypeInfo const& i : kTypes)
        if (i.type == t)
            return i.dir;
    return Direction::Either;
}

Json Envelope::toJson() const
{
    Json j = Json::object();
    j.set("version", Json::integer(version));
    j.set("type", Json::string(msgTypeName(type)));
    j.set("epoch", Json::string(epoch));
    j.set("seq", Json::integer(seq));
    j.set("request_id", hasRequestId ? Json::string(requestId) : Json::null());
    j.set("payload", payload);
    return j;
}

bool validatePayload(MsgType type, Json const& payload, std::string& detail)
{
    return checkObject(payload, payloadFields(type), detail);
}

DecodeResult decodeEnvelope(std::string const& frameBody)
{
    DecodeResult r;
    JsonParseResult pr = parseJson(frameBody);
    if (!pr.ok)
    {
        r.code = "invalid_json";
        r.detail = pr.error;
        return r;
    }
    Json const& j = pr.value;
    if (!j.isObject())
    {
        r.code = "invalid_envelope";
        r.detail = "not_object";
        return r;
    }

    // Version first: reject unsupported peers before interpreting anything else.
    Json const* version = j.find("version");
    if (!version || !version->isInteger())
    {
        r.code = "invalid_envelope";
        r.detail = "missing_version";
        return r;
    }
    if (version->asInt() != kProtocolVersion)
    {
        r.code = "unsupported_version";
        r.detail = "version";
        return r;
    }

    static char const* const allowed[] = { "version", "type", "epoch", "seq", "request_id", "payload" };
    for (Json::Member const& m : j.members())
    {
        if (std::none_of(std::begin(allowed), std::end(allowed), [&](char const* a) { return m.first == a; }))
        {
            r.code = "invalid_envelope";
            r.detail = "unknown_field";
            return r;
        }
    }
    Json const* type = j.find("type");
    Json const* epoch = j.find("epoch");
    Json const* seq = j.find("seq");
    Json const* reqId = j.find("request_id");
    Json const* payload = j.find("payload");
    if (!type || !epoch || !seq || !reqId || !payload)
    {
        r.code = "invalid_envelope";
        r.detail = "missing_field";
        return r;
    }
    if (!type->isString())
    {
        r.code = "invalid_envelope";
        r.detail = "type";
        return r;
    }
    if (!msgTypeFromName(type->asString(), r.env.type))
    {
        r.code = "unknown_type";
        r.detail = "type";
        return r;
    }
    if (!epoch->isString() || epoch->asString().size() > kMaxEpochLength)
    {
        r.code = "invalid_envelope";
        r.detail = "epoch";
        return r;
    }
    if (!seq->isInteger() || seq->asInt() < 0 || seq->asInt() > kMaxSafeInteger)
    {
        r.code = "invalid_envelope";
        r.detail = "seq";
        return r;
    }
    if (!(reqId->isNull() || (reqId->isString() && !reqId->asString().empty() && reqId->asString().size() <= kMaxRequestIdLength)))
    {
        r.code = "invalid_envelope";
        r.detail = "request_id";
        return r;
    }
    if (!payload->isObject())
    {
        r.code = "invalid_envelope";
        r.detail = "payload";
        return r;
    }
    std::string d;
    if (!validatePayload(r.env.type, *payload, d))
    {
        r.code = "invalid_payload";
        r.detail = d;
        return r;
    }
    r.env.version = version->asInt();
    r.env.epoch = epoch->asString();
    r.env.seq = seq->asInt();
    r.env.hasRequestId = reqId->isString();
    if (r.env.hasRequestId)
        r.env.requestId = reqId->asString();
    r.env.payload = *payload;
    r.ok = true;
    return r;
}

bool tokenEquals(std::string const& a, std::string const& b)
{
    // Length leak is acceptable (local secret, fixed length per install); content is compared in constant time.
    std::size_t n = std::max(a.size(), b.size());
    unsigned char diff = static_cast<unsigned char>(a.size() != b.size());
    for (std::size_t i = 0; i < n; ++i)
    {
        unsigned char ca = i < a.size() ? static_cast<unsigned char>(a[i]) : 0;
        unsigned char cb = i < b.size() ? static_cast<unsigned char>(b[i]) : 0;
        diff |= static_cast<unsigned char>(ca ^ cb);
    }
    return diff == 0;
}

std::uint64_t payloadHash(Json const& payload)
{
    std::string s;
    canonicalDump(payload, s);
    std::uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s)
    {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

} // namespace gamebridge
