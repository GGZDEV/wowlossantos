// GameBridge protocol v1 - envelope and typed payload validation.
//
// This is the application protocol between the GTA SA adapter (host) and
// mod-gamebridge (core). It is NOT the WoW wire protocol. See docs/protocol.md.
#pragma once

#include "gamebridge/json.h"

#include <cstdint>
#include <string>

namespace gamebridge {

constexpr std::int64_t kProtocolVersion = 1;
constexpr std::int64_t kMaxSafeInteger = 9007199254740991LL; // 2^53-1
constexpr std::size_t kMaxEpochLength = 128;
constexpr std::size_t kMaxRequestIdLength = 128;
constexpr std::uint16_t kDefaultPort = 17635;

enum class MsgType : std::uint8_t
{
    Hello, Welcome, EnterTest, EntityBind, HostBindAck, Position, CastRequest, CastStatus,
    StateSnapshot, CombatEvent, HostEntityLost, EntityDespawn, SetPaused, PauseAck, Resync,
    Reset, Ping, Pong, Error,
    Count
};

enum class Direction : std::uint8_t { HostToCore, CoreToHost, Either };

char const* msgTypeName(MsgType t);
bool msgTypeFromName(std::string const& name, MsgType& out);
Direction msgDirection(MsgType t);

struct Envelope
{
    std::int64_t version = kProtocolVersion;
    MsgType type = MsgType::Ping;
    std::string epoch;
    std::int64_t seq = 0;
    bool hasRequestId = false;
    std::string requestId;
    Json payload = Json::object();

    Json toJson() const;
    std::string serialize() const { return toJson().dump(); }
};

struct DecodeResult
{
    bool ok = false;
    Envelope env;
    // Machine-readable code: invalid_json, invalid_envelope, unsupported_version,
    // unknown_type, invalid_payload. Version is checked first so that an
    // unsupported peer is rejected before anything else is interpreted.
    std::string code;
    std::string detail;  // safe description; never contains the token or raw input
};

// Parse + validate one frame body (envelope and typed payload).
DecodeResult decodeEnvelope(std::string const& frameBody);

// Validate a payload against the v1 schema for its type (exact field set,
// types, enums, finite numbers, safe-integer ranges, string limits).
bool validatePayload(MsgType type, Json const& payload, std::string& detail);

// Constant-time comparison for the local bridge token.
bool tokenEquals(std::string const& a, std::string const& b);

// Stable 64-bit FNV-1a hash of the canonical payload (used for request dedupe).
std::uint64_t payloadHash(Json const& payload);

} // namespace gamebridge
