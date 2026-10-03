/*
 * mod-gamebridge - see GameBridge.h for the threading contract.
 *
 * Native paths used (all verified against the pinned AzerothCore revision):
 *  - headless WorldSession: WorldSession ctor with a null socket (upstream "headless
 *    session" support, commit 92fed92e), owned by this module and NOT registered in
 *    WorldSessionMgr (which deletes socket-less sessions in UpdateSessions).
 *  - character creation: WorldSession::HandleCharCreateOpcode (normal create path/validation)
 *  - login: LoginQueryHolder + WorldSession::HandlePlayerLoginFromDB (normal login path)
 *  - casting: WorldSession::HandleCastSpellOpcode with a CMSG_CAST_SPELL payload, i.e. the
 *    exact player cast path (spellbook check, spell queue, Spell::prepare/CheckCast:
 *    power, cooldown, GCD, range, LOS, facing, target validity). Nothing is triggered.
 *  - logout/save: WorldSession::LogoutPlayer(true)
 */
#include "GameBridge.h"

#include "AccountMgr.h"
#include "CharacterCache.h"
#include "Config.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "GitRevision.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "Player.h"
#include "QueryHolder.h"
#include "SmartEnum.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "TemporarySummon.h"
#include "Timer.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <random>
#include <sstream>
#include <thread>

using gamebridge::Json;
using gamebridge::MsgType;

namespace GameBridge
{

namespace
{

constexpr char const* kPlayerEntityId = "player-1";
constexpr char const* kTargetEntityId = "target-1";
constexpr size_t kMaxHookEvents = 1024;

size_t ThreadTag()
{
    return std::hash<std::thread::id>{}(std::this_thread::get_id()) % 100000;
}

std::string GuidString(ObjectGuid guid)
{
    char buf[24];
    snprintf(buf, sizeof(buf), "0x%016llX", static_cast<unsigned long long>(guid.GetRawValue()));
    return buf;
}

char const* PowerName(uint8 p)
{
    switch (p)
    {
        case POWER_MANA: return "mana";
        case POWER_RAGE: return "rage";
        case POWER_FOCUS: return "focus";
        case POWER_ENERGY: return "energy";
        case POWER_HAPPINESS: return "happiness";
        case POWER_RUNE: return "runes";
        case POWER_RUNIC_POWER: return "runic_power";
        default: return "none";
    }
}

Json Vec(float x, float y, float z)
{
    Json p = Json::object();
    p.set("x", Json::number(x));
    p.set("y", Json::number(y));
    p.set("z", Json::number(z));
    return p;
}

bool ParseFloats(std::string const& s, float* out, size_t n)
{
    std::istringstream in(s);
    for (size_t i = 0; i < n; ++i)
        if (!(in >> out[i]) || !std::isfinite(out[i]))
            return false;
    return true;
}

std::string RandomPassword()
{
    static char const alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    std::random_device rd;
    std::string p;
    for (int i = 0; i < 16; ++i)
        p.push_back(alphabet[rd() % (sizeof(alphabet) - 1)]);
    return p;
}

} // namespace

Bridge& Bridge::Instance()
{
    static Bridge instance;
    return instance;
}

// ---------------------------------------------------------------------------
// configuration
// ---------------------------------------------------------------------------

void Bridge::LoadConfig()
{
    Config c;
    c.enabled = sConfigMgr->GetOption<bool>("GameBridge.Enable", false);
    c.port = static_cast<uint16>(sConfigMgr->GetOption<uint32>("GameBridge.Port", gamebridge::kDefaultPort));
    c.token = sConfigMgr->GetOption<std::string>("GameBridge.Token", "");
    std::string tokenEnv = sConfigMgr->GetOption<std::string>("GameBridge.TokenEnv", "ATA_BRIDGE_TOKEN");
    if (!tokenEnv.empty())
        if (char const* t = std::getenv(tokenEnv.c_str()))
            c.token = t;
    c.accountName = sConfigMgr->GetOption<std::string>("GameBridge.Account", "GTABRIDGE");
    std::transform(c.accountName.begin(), c.accountName.end(), c.accountName.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    c.characterName = sConfigMgr->GetOption<std::string>("GameBridge.Character", "Gtabridge");
    c.createIfMissing = sConfigMgr->GetOption<bool>("GameBridge.Fixture.CreateIfMissing", true);
    c.race = static_cast<uint8>(sConfigMgr->GetOption<uint32>("GameBridge.Fixture.Race", 1));
    c.playerClass = static_cast<uint8>(sConfigMgr->GetOption<uint32>("GameBridge.Fixture.Class", 8));
    c.gender = static_cast<uint8>(sConfigMgr->GetOption<uint32>("GameBridge.Fixture.Gender", 0));
    c.fixtureId = sConfigMgr->GetOption<std::string>("GameBridge.Fixture.Id", "m1-arena");
    c.creatureEntry = sConfigMgr->GetOption<uint32>("GameBridge.Fixture.CreatureEntry", 0);
    c.targetDistance = sConfigMgr->GetOption<float>("GameBridge.Fixture.TargetDistance", 15.0f);
    c.isolateEntities = sConfigMgr->GetOption<bool>("GameBridge.Fixture.IsolateEntities", true);
    {
        std::istringstream in(sConfigMgr->GetOption<std::string>("GameBridge.SpellIds", ""));
        uint32 id;
        while (in >> id)
            c.spellIds.push_back(id);
    }
    c.mapId = sConfigMgr->GetOption<uint32>("GameBridge.Arena.Map", 1);
    float core[4] = { 0, 0, 0, 0 };
    if (!ParseFloats(sConfigMgr->GetOption<std::string>("GameBridge.Arena.CoreOrigin", ""), core, 4))
        LOG_ERROR("module.gamebridge", "GameBridge.Arena.CoreOrigin must be \"x y z o\"");
    c.coreOrigin.Relocate(core[0], core[1], core[2], core[3]);
    if (!ParseFloats(sConfigMgr->GetOption<std::string>("GameBridge.Projection.GtaOrigin", "0 0 0"), c.gtaOrigin, 3))
        LOG_ERROR("module.gamebridge", "GameBridge.Projection.GtaOrigin must be \"x y z\"");
    c.scale = sConfigMgr->GetOption<float>("GameBridge.Projection.Scale", 1.0936133f);
    c.yawRad = sConfigMgr->GetOption<float>("GameBridge.Projection.YawDegrees", -90.0f) * float(M_PI) / 180.0f;
    c.arenaRadius = sConfigMgr->GetOption<float>("GameBridge.Arena.Radius", 40.0f);
    c.maxSpeed = sConfigMgr->GetOption<float>("GameBridge.Arena.MaxSpeed", 20.0f);
    c.coreSha = GitRevision::GetHash();
    _cfg = c;
}

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------

void Bridge::StartEndpoint()
{
    gamebridge::CoreEndpoint::Config ec;
    ec.port = _cfg.port;
    ec.token = _cfg.token;
    _endpoint = std::make_unique<gamebridge::CoreEndpoint>(ec, [](std::string const& s) {
        LOG_INFO("module.gamebridge.io", "{}", s);  // IO thread: logging only
    });
    if (!_endpoint->start())
    {
        LOG_ERROR("module.gamebridge", "endpoint failed to start (token configured? port {} free?)", _cfg.port);
        _endpoint.reset();
    }
}

void Bridge::Update(uint32 diff)
{
    if (!_worldThread)
        _worldThread = ThreadTag();
    if (_phase == Phase::Disabled)
    {
        if (!_cfg.enabled)
            return;
        if (!_cfg.creatureEntry || _cfg.spellIds.empty())
        {
            LOG_ERROR("module.gamebridge", "GameBridge.Fixture.CreatureEntry and GameBridge.SpellIds are required");
            _phase = Phase::Failed;
            return;
        }
        StartEndpoint();
        if (!_endpoint)
        {
            _phase = Phase::Failed;
            return;
        }
        _phase = Phase::Provisioning;
        _phaseTimer = 0;
        Provision();
        return;
    }

    if (_phase == Phase::Provisioning)
    {
        _phaseTimer += diff;
        Provision();
        return;
    }

    if (_phase == Phase::CreatingCharacter || _phase == Phase::LoggingIn)
    {
        // The session is not in WorldSessionMgr and its player is not on a map yet,
        // so nobody else processes its async DB callbacks: do it here (world thread).
        _session->ProcessQueryCallbacks();
        DrainHookEvents();
        if (_phase == Phase::LoggingIn)
        {
            Player* p = _session->GetPlayer();
            if (p && p->IsInWorld())
            {
                _phase = Phase::InWorld;
                {
                    std::lock_guard<std::mutex> g(_hookMx);
                    _hookPlayer = p->GetGUID();
                }
                LOG_INFO("module.gamebridge", "headless player {} ({}) in world: map {} ({:.2f}, {:.2f}, {:.2f}) level {} hp {}/{} power {}/{}",
                         p->GetName(), GuidString(p->GetGUID()), p->GetMapId(), p->GetPositionX(), p->GetPositionY(),
                         p->GetPositionZ(), p->GetLevel(), p->GetHealth(), p->GetMaxHealth(),
                         p->GetPower(p->getPowerType()), p->GetMaxPower(p->getPowerType()));
                LOG_INFO("module.gamebridge", "loaded state: xp {} auras {} (frost armor 168: {})", p->GetUInt32Value(PLAYER_XP),
                         p->GetAppliedAuras().size(), p->HasAura(168) ? "yes" : "no");
                for (uint32 spellId : _cfg.spellIds)
                    LOG_INFO("module.gamebridge", "fixture spell {} learned/active: {}", spellId, p->HasActiveSpell(spellId) ? "yes" : "NO");
            }
            else if (_loginDispatched && !_session->PlayerLoading() && !p)
            {
                // HandlePlayerLoginFromDB ran and left no player (LoadFromDB failure/kick).
                LOG_ERROR("module.gamebridge", "headless login failed (see entities.player / network logs)");
                _phase = Phase::Failed;
            }
            else if ((_phaseTimer += diff) > 60000)
            {
                LOG_ERROR("module.gamebridge", "headless login timed out");
                _phase = Phase::Failed;
            }
        }
        return;
    }

    if (_phase != Phase::InWorld || !_endpoint)
        return;

    Player* player = GetPlayer();
    if (!player || !player->IsInWorld())
    {
        LOG_ERROR("module.gamebridge", "headless player left the world unexpectedly");
        ExitTestMode("player_left_world", true);
        _phase = Phase::Failed;
        return;
    }

    gamebridge::CoreEndpoint::Inbound in;
    // Bounded per tick; the endpoint queue itself is bounded.
    for (int i = 0; i < 64 && _endpoint->pollInbound(in); ++i)
        HandleInbound(in);

    if (_welcomePending && !_epoch.empty())
        SendWelcome();

    DrainHookEvents();
    PublishState(diff, false);
}

void Bridge::Provision()
{
    // Re-entrant: called every world tick while Phase::Provisioning.
    _accountId = AccountMgr::GetId(_cfg.accountName);
    if (!_accountId)
    {
        if (!_cfg.createIfMissing)
        {
            LOG_ERROR("module.gamebridge", "bridge account {} missing and CreateIfMissing = 0", _cfg.accountName);
            _phase = Phase::Failed;
            return;
        }
        if (!_accountCreateIssued)
        {
            AccountOpResult r = sAccountMgr->CreateAccount(_cfg.accountName, RandomPassword());
            if (r != AOR_OK)
            {
                LOG_ERROR("module.gamebridge", "could not create bridge account {} (result {})", _cfg.accountName, uint32(r));
                _phase = Phase::Failed;
                return;
            }
            _accountCreateIssued = true;
            LOG_INFO("module.gamebridge", "bridge account {} creation queued (random unused password)", _cfg.accountName);
        }
        // AccountMgr::CreateAccount inserts through the async login DB queue: wait for it.
        if (_phaseTimer > 15000)
        {
            LOG_ERROR("module.gamebridge", "bridge account {} still missing after creation", _cfg.accountName);
            _phase = Phase::Failed;
        }
        return;
    }
    LOG_INFO("module.gamebridge", "bridge account {} id {}", _cfg.accountName, _accountId);

    // Headless session: null socket. Owned by this module, never added to WorldSessionMgr.
    _session = new WorldSession(_accountId, std::string(_cfg.accountName), 0, nullptr, SEC_PLAYER,
                                uint8(sWorld->getIntConfig(CONFIG_EXPANSION)), 0, LOCALE_enUS, 0, false, true, 0);
    {
        std::lock_guard<std::mutex> g(_hookMx);
        _hookSession = _session;   // only the pointer value is compared in hooks
    }

    ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(_cfg.characterName);
    if (guid)
    {
        if (sCharacterCache->GetCharacterAccountIdByGuid(guid) != _accountId)
        {
            LOG_ERROR("module.gamebridge", "character {} does not belong to account {}", _cfg.characterName, _cfg.accountName);
            _phase = Phase::Failed;
            return;
        }
        _playerGuid = guid;
        BeginLogin();
        return;
    }
    if (!_cfg.createIfMissing)
    {
        LOG_ERROR("module.gamebridge", "character {} missing and CreateIfMissing = 0", _cfg.characterName);
        _phase = Phase::Failed;
        return;
    }
    CreateCharacter();
}

void Bridge::CreateCharacter()
{
    // Exactly the client's CMSG_CHAR_CREATE payload, handled by the native handler
    // (name rules, race/class legality, realm limits, persistence).
    WorldPacket pkt(CMSG_CHAR_CREATE, _cfg.characterName.size() + 1 + 10);
    pkt << _cfg.characterName;
    pkt << uint8(_cfg.race) << uint8(_cfg.playerClass) << uint8(_cfg.gender);
    pkt << uint8(0) << uint8(0) << uint8(0) << uint8(0) << uint8(0);   // skin face hairStyle hairColor facialHair
    pkt << uint8(0);                                                    // outfitId
    _phase = Phase::CreatingCharacter;
    LOG_INFO("module.gamebridge", "creating fixture character {} race {} class {} via native CMSG_CHAR_CREATE handler",
             _cfg.characterName, _cfg.race, _cfg.playerClass);
    _session->HandleCharCreateOpcode(pkt);
}

void Bridge::BeginLogin()
{
    _phase = Phase::LoggingIn;
    _phaseTimer = 0;
    _loginDispatched = false;
    // Fixture placement: put the character at the arena anchor before loading it
    // (same statement the core uses for offline teleports).
    uint32 zone = sMapMgr->GetZoneId(PHASEMASK_NORMAL, _cfg.mapId, _cfg.coreOrigin);
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    Player::SavePositionInDB(WorldLocation(_cfg.mapId, _cfg.coreOrigin), uint16(zone), _playerGuid, trans);
    ObjectGuid guid = _playerGuid;
    uint32 accountId = _accountId;
    _session->AddTransactionCallback(CharacterDatabase.AsyncCommitTransaction(trans)).AfterComplete([this, guid, accountId](bool ok)
    {
        if (!ok)
        {
            LOG_ERROR("module.gamebridge", "fixture position update failed");
            _phase = Phase::Failed;
            return;
        }
        std::shared_ptr<LoginQueryHolder> holder = std::make_shared<LoginQueryHolder>(accountId, guid);
        if (!holder->Initialize())
        {
            LOG_ERROR("module.gamebridge", "LoginQueryHolder initialisation failed");
            _phase = Phase::Failed;
            return;
        }
        LOG_INFO("module.gamebridge", "loading character {} through native login path (no client)", GuidString(guid));
        _session->AddQueryHolderCallback(CharacterDatabase.DelayQueryHolder(holder)).AfterComplete([this](SQLQueryHolderBase const& h)
        {
            _loginDispatched = true;
            _session->HandlePlayerLoginFromDB(static_cast<LoginQueryHolder const&>(h));
        });
    });
}

Player* Bridge::GetPlayer() const
{
    return _session ? _session->GetPlayer() : nullptr;
}

void Bridge::LogoutHeadless(char const* reason)
{
    if (!_session)
        return;
    {
        std::lock_guard<std::mutex> g(_hookMx);
        _hookSession = nullptr;
        _hookPlayer.Clear();
        _hookBound.clear();
    }
    if (Player* p = _session->GetPlayer())
        LOG_INFO("module.gamebridge", "logging out headless player {} ({}): hp {}/{}, saving through native logout",
                 p->GetName(), reason, p->GetHealth(), p->GetMaxHealth());
    if (_session->GetPlayer())
        _session->LogoutPlayer(true);
    delete _session;
    _session = nullptr;
}

void Bridge::Shutdown()
{
    if (_phase == Phase::Disabled)
        return;
    if (_endpoint)
        _endpoint->stop();   // join IO thread first: nothing can enqueue afterwards
    if (_phase == Phase::InWorld)
        DespawnTarget("core_shutdown", false);
    LogoutHeadless("core_shutdown");
    _endpoint.reset();
    _phase = Phase::Disabled;
}

// ---------------------------------------------------------------------------
// protocol (world thread)
// ---------------------------------------------------------------------------

void Bridge::Send(MsgType type, Json payload, std::string const& requestId, std::string const& coalesceKey)
{
    if (!_endpoint || _epoch.empty())
        return;
    if (!_endpoint->send(_epoch, type, std::move(payload), requestId, coalesceKey))
    {
        // The endpoint resets the connection; host reconnects with a new epoch + full snapshot.
        LOG_ERROR("module.gamebridge", "outbound queue overflow: bridge frozen until resync");
    }
}

void Bridge::SendError(std::string const& code, std::string const& requestId, std::string const& description)
{
    Json p = Json::object();
    p.set("code", Json::string(code));
    p.set("related_request_id", requestId.empty() ? Json::null() : Json::string(requestId));
    p.set("description", Json::string(description));
    Send(MsgType::Error, p, requestId);
}

void Bridge::SendWelcome()
{
    Player* player = GetPlayer();
    if (!player)
        return;
    Json p = Json::object();
    p.set("core_sha", Json::string(_cfg.coreSha.substr(0, 64)));
    Json caps = Json::array();
    for (char const* c : { "native_cast", "state_snapshot", "combat_events", "headless_session", "pause_as_reset" })
        caps.push(Json::string(c));
    p.set("capabilities", caps);
    p.set("character_id", Json::string(std::to_string(player->GetGUID().GetCounter())));
    p.set("player_entity_id", Json::string(kPlayerEntityId));
    p.set("data_fixture", Json::string(_cfg.fixtureId));
    Json spells = Json::array();
    for (uint32 s : _cfg.spellIds)
        spells.push(Json::string(std::to_string(s)));
    p.set("spell_ids", spells);
    Send(MsgType::Welcome, p);
    _welcomePending = false;
    LOG_INFO("module.gamebridge", "WELCOME sent, epoch {}", _epoch);
}

void Bridge::HandleInbound(gamebridge::CoreEndpoint::Inbound& in)
{
    using Kind = gamebridge::CoreEndpoint::Inbound::Kind;
    if (in.kind == Kind::Connected)
    {
        if (_testMode)
            ExitTestMode("new_epoch", false);
        _epoch = in.epoch;
        _revision = 0;
        _eventCounter = 0;
        _pendingCasts.clear();
        _welcomePending = true;
        return;
    }
    if (in.kind == Kind::Disconnected)
    {
        if (in.epoch != _epoch)
            return;
        LOG_INFO("module.gamebridge", "host disconnected ({}): freezing fixture, pending requests invalidated", in.reason);
        ExitTestMode("host_disconnected", false);
        _pendingCasts.clear();
        _epoch.clear();
        _welcomePending = false;
        return;
    }
    if (in.epoch != _epoch)
        return;  // stale

    gamebridge::Envelope const& env = in.env;
    switch (env.type)
    {
        case MsgType::EnterTest: HandleEnterTest(env); break;
        case MsgType::HostBindAck: HandleHostBindAck(env); break;
        case MsgType::Position: HandlePosition(env); break;
        case MsgType::CastRequest: HandleCastRequest(env); break;
        case MsgType::SetPaused:
        {
            bool paused = env.payload.find("paused")->asBool();
            if (paused)
                ExitTestMode("paused", true);   // M1: pause = reset/despawn, fresh ENTER_TEST on resume
            Json p = Json::object();
            p.set("paused", Json::boolean(paused));
            p.set("final_revision", Json::integer(int64(_revision)));
            Send(MsgType::PauseAck, p, env.hasRequestId ? env.requestId : std::string());
            break;
        }
        case MsgType::Resync:
            for (auto const& [id, e] : _entities)
            {
                (void)id;
                Json b = Json::object();
                b.set("entity_id", Json::string(e.id));
                b.set("kind", Json::string(e.isPlayer ? "player" : "creature"));
                b.set("core_guid", Json::string(GuidString(e.guid)));
                b.set("template_tag", Json::string(e.isPlayer ? "fixture_player" : "fixture_target"));
                b.set("generation", Json::integer(int64(e.generation)));
                Send(MsgType::EntityBind, b);
            }
            PublishState(0, true);
            break;
        case MsgType::HostEntityLost:
        {
            std::string id = env.payload.find("entity_id")->asString();
            if (id == kTargetEntityId)
                DespawnTarget("host_entity_lost", true);
            else if (id == kPlayerEntityId)
                ExitTestMode("host_player_lost", true);
            break;
        }
        case MsgType::Reset:
            ExitTestMode("host_reset", false);
            break;
        default:
            SendError("unsupported_message", env.hasRequestId ? env.requestId : std::string(), gamebridge::msgTypeName(env.type));
            break;
    }
}

void Bridge::HostToCore(float hx, float hy, float hz, float& cx, float& cy, float& cz) const
{
    float dx = (hx - _cfg.gtaOrigin[0]) * _cfg.scale;
    float dy = (hy - _cfg.gtaOrigin[1]) * _cfg.scale;
    float c = std::cos(_cfg.yawRad), s = std::sin(_cfg.yawRad);
    cx = _cfg.coreOrigin.GetPositionX() + c * dx - s * dy;
    cy = _cfg.coreOrigin.GetPositionY() + s * dx + c * dy;
    cz = _cfg.coreOrigin.GetPositionZ() + (hz - _cfg.gtaOrigin[2]) * _cfg.scale;
}

void Bridge::CoreToHost(float cx, float cy, float cz, float& hx, float& hy, float& hz) const
{
    float dx = cx - _cfg.coreOrigin.GetPositionX();
    float dy = cy - _cfg.coreOrigin.GetPositionY();
    float c = std::cos(-_cfg.yawRad), s = std::sin(-_cfg.yawRad);
    hx = _cfg.gtaOrigin[0] + (c * dx - s * dy) / _cfg.scale;
    hy = _cfg.gtaOrigin[1] + (s * dx + c * dy) / _cfg.scale;
    hz = _cfg.gtaOrigin[2] + (cz - _cfg.coreOrigin.GetPositionZ()) / _cfg.scale;
}

Bridge::BoundEntity* Bridge::FindEntity(std::string const& id)
{
    auto it = _entities.find(id);
    return it == _entities.end() ? nullptr : &it->second;
}

void Bridge::HandleEnterTest(gamebridge::Envelope const& env)
{
    std::string const reqId = env.hasRequestId ? env.requestId : std::string();
    Player* player = GetPlayer();
    if (env.payload.find("test_fixture_id")->asString() != _cfg.fixtureId)
    {
        SendError("unknown_fixture", reqId, "fixture not configured on core");
        return;
    }
    if (!player->IsAlive())
    {
        SendError("player_dead", reqId, "fixture player is dead");
        return;
    }
    if (_testMode)
        ExitTestMode("re_enter", true);

    Json const& obs = *env.payload.find("observed_player_position");
    float cx, cy, cz;
    HostToCore(float(obs.find("x")->asDouble()), float(obs.find("y")->asDouble()), float(obs.find("z")->asDouble()), cx, cy, cz);
    if (std::hypot(cx - _cfg.coreOrigin.GetPositionX(), cy - _cfg.coreOrigin.GetPositionY()) > _cfg.arenaRadius)
    {
        SendError("outside_arena", reqId, "observed player position projects outside the arena");
        return;
    }
    Map* map = player->GetMap();
    float groundZ = map->GetHeight(player->GetPhaseMask(), cx, cy, player->GetPositionZ() + 5.0f);
    if (groundZ <= INVALID_HEIGHT)
    {
        SendError("no_ground", reqId, "no core terrain at projected position");
        return;
    }
    // Movement observation inside the arena; same relocation primitive the movement handler uses.
    map->PlayerRelocation(player, cx, cy, groundZ, _cfg.coreOrigin.GetOrientation());
    _lastSampleId = -1;
    _lastSampleMs = getMSTime();

    BoundEntity pe;
    pe.id = kPlayerEntityId;
    pe.isPlayer = true;
    pe.guid = player->GetGUID();
    pe.generation = ++_generationCounter;
    _entities[pe.id] = pe;

    if (!SpawnTarget(player))
    {
        _entities.clear();
        SendError("target_spawn_failed", reqId, "could not summon fixture creature");
        return;
    }
    {
        std::lock_guard<std::mutex> g(_hookMx);
        _hookBound.clear();
        for (auto const& [id, e] : _entities)
        {
            (void)id;
            _hookBound.insert(e.guid.GetRawValue());
        }
    }
    _testMode = true;

    for (auto const& [id, e] : _entities)
    {
        (void)id;
        Unit* u = e.isPlayer ? static_cast<Unit*>(player) : map->GetCreature(e.guid);
        float hx, hy, hz;
        CoreToHost(u->GetPositionX(), u->GetPositionY(), u->GetPositionZ(), hx, hy, hz);
        Json b = Json::object();
        b.set("entity_id", Json::string(e.id));
        b.set("kind", Json::string(e.isPlayer ? "player" : "creature"));
        b.set("core_guid", Json::string(GuidString(e.guid)));
        b.set("template_tag", Json::string(e.isPlayer ? "fixture_player" : "fixture_target"));
        b.set("generation", Json::integer(int64(e.generation)));
        b.set("position", Vec(u->GetPositionX(), u->GetPositionY(), u->GetPositionZ()));
        b.set("host_position", Vec(hx, hy, hz));
        Send(MsgType::EntityBind, b);
        LOG_INFO("module.gamebridge", "bound {} -> {} gen {} core ({:.2f}, {:.2f}, {:.2f}) host ({:.2f}, {:.2f}, {:.2f})",
                 e.id, GuidString(e.guid), e.generation, u->GetPositionX(), u->GetPositionY(), u->GetPositionZ(), hx, hy, hz);
    }
    PublishState(0, true);
}

bool Bridge::SpawnTarget(Player* player)
{
    Map* map = player->GetMap();
    float o = player->GetOrientation();
    float x = player->GetPositionX() + _cfg.targetDistance * std::cos(o);
    float y = player->GetPositionY() + _cfg.targetDistance * std::sin(o);
    float z = map->GetHeight(player->GetPhaseMask(), x, y, player->GetPositionZ() + 5.0f);
    if (z <= INVALID_HEIGHT)
        z = player->GetPositionZ();
    Position pos(x, y, z, Position::NormalizeOrientation(o + float(M_PI)));
    // Map-owned summon (no summoner, manual despawn): lives on the player's map, never respawns.
    TempSummon* c = map->SummonCreature(_cfg.creatureEntry, pos);
    if (!c)
        return false;
    // M1 fixture: stationary, non-attacking target. AI suppression is scoped to this summon.
    c->SetReactState(REACT_PASSIVE);
    c->GetMotionMaster()->MoveIdle();
    BoundEntity te;
    te.id = kTargetEntityId;
    te.guid = c->GetGUID();
    te.generation = ++_generationCounter;
    _entities[te.id] = te;
    LOG_INFO("module.gamebridge", "fixture creature {} entry {} '{}' level {} hp {}/{} at ({:.2f}, {:.2f}, {:.2f}), {:.1f} yd from player",
             GuidString(c->GetGUID()), c->GetEntry(), c->GetName(), c->GetLevel(), c->GetHealth(), c->GetMaxHealth(),
             c->GetPositionX(), c->GetPositionY(), c->GetPositionZ(), player->GetDistance(c));
    return true;
}

void Bridge::DespawnTarget(char const* reason, bool notifyHost)
{
    auto it = _entities.find(kTargetEntityId);
    if (it == _entities.end())
        return;
    Player* player = GetPlayer();
    if (player && player->IsInWorld())
        if (Creature* c = player->GetMap()->GetCreature(it->second.guid))
        {
            if (TempSummon* s = c->ToTempSummon())
                s->UnSummon();
            else
                c->AddObjectToRemoveList();
        }
    if (notifyHost)
    {
        Json p = Json::object();
        p.set("entity_id", Json::string(kTargetEntityId));
        p.set("final_revision", Json::integer(int64(_revision)));
        p.set("reason", Json::string(reason));
        Send(MsgType::EntityDespawn, p);
    }
    {
        std::lock_guard<std::mutex> g(_hookMx);
        _hookBound.erase(it->second.guid.GetRawValue());
    }
    LOG_INFO("module.gamebridge", "fixture target despawned ({})", reason);
    _entities.erase(it);
}

void Bridge::ExitTestMode(char const* reason, bool notifyHost)
{
    Player* player = GetPlayer();
    if (player && player->IsInWorld())
    {
        // Clear pending effects: nothing queued may age invisibly and land later.
        player->InterruptNonMeleeSpells(true);
        player->SpellQueue.clear();
        player->CombatStop(true);
    }
    for (auto const& [cc, pc] : _pendingCasts)
    {
        (void)cc;
        if (notifyHost && pc.epoch == _epoch)
            SendCastStatus(pc, "interrupted", reason, 0);
    }
    _pendingCasts.clear();
    DespawnTarget(reason, notifyHost);
    if (notifyHost && _entities.count(kPlayerEntityId))
    {
        Json p = Json::object();
        p.set("entity_id", Json::string(kPlayerEntityId));
        p.set("final_revision", Json::integer(int64(_revision)));
        p.set("reason", Json::string(reason));
        Send(MsgType::EntityDespawn, p);
    }
    _entities.clear();
    {
        std::lock_guard<std::mutex> g(_hookMx);
        _hookBound.clear();
    }
    if (_testMode)
        LOG_INFO("module.gamebridge", "test mode exited ({})", reason);
    _testMode = false;
}

void Bridge::HandleHostBindAck(gamebridge::Envelope const& env)
{
    BoundEntity* e = FindEntity(env.payload.find("entity_id")->asString());
    if (!e)
    {
        SendError("unknown_entity", std::string(), "HOST_BIND_ACK for unbound entity");
        return;
    }
    e->hostGeneration = uint64(env.payload.find("host_generation")->asInt());
}

void Bridge::HandlePosition(gamebridge::Envelope const& env)
{
    BoundEntity* e = FindEntity(env.payload.find("entity_id")->asString());
    if (!_testMode || !e || !e->isPlayer || uint64(env.payload.find("generation")->asInt()) != e->generation)
        return;  // stale/unknown telemetry is dropped (coalescable)
    int64 sample = env.payload.find("sample_id")->asInt();
    if (sample <= _lastSampleId)
        return;
    Player* player = GetPlayer();
    Json const& pos = *env.payload.find("position");
    float cx, cy, cz;
    HostToCore(float(pos.find("x")->asDouble()), float(pos.find("y")->asDouble()), float(pos.find("z")->asDouble()), cx, cy, cz);
    if (std::hypot(cx - _cfg.coreOrigin.GetPositionX(), cy - _cfg.coreOrigin.GetPositionY()) > _cfg.arenaRadius)
    {
        ExitTestMode("left_arena", true);
        return;
    }
    uint32 now = getMSTime();
    float dt = std::max(0.05f, getMSTimeDiff(_lastSampleMs, now) / 1000.0f);
    float dist = std::hypot(cx - player->GetPositionX(), cy - player->GetPositionY());
    if (dist > _cfg.maxSpeed * dt + 2.0f)
    {
        // Teleport/loading invalidates the binding.
        ExitTestMode("position_jump", true);
        return;
    }
    Map* map = player->GetMap();
    float groundZ = map->GetHeight(player->GetPhaseMask(), cx, cy, player->GetPositionZ() + 3.0f);
    if (groundZ <= INVALID_HEIGHT)
        return;
    // GTA heading 0 = north, counter-clockwise; core orientation = heading + pi/2 + yaw.
    float o = Position::NormalizeOrientation(float(env.payload.find("orientation")->asDouble()) + float(M_PI) / 2.0f + _cfg.yawRad);
    map->PlayerRelocation(player, cx, cy, groundZ, o);
    _lastSampleId = sample;
    _lastSampleMs = now;
}

void Bridge::SendCastStatus(PendingCast const& pc, char const* status, std::string const& reason, int32 nativeResult)
{
    Json p = Json::object();
    p.set("status", Json::string(status));
    p.set("reason", reason.empty() ? Json::null() : Json::string(reason.substr(0, 64)));
    p.set("spell_id", Json::integer(pc.spellId));
    if (nativeResult)
        p.set("native_result", Json::integer(nativeResult));
    Send(MsgType::CastStatus, p, pc.requestId);
}

void Bridge::HandleCastRequest(gamebridge::Envelope const& env)
{
    PendingCast pc;
    pc.requestId = env.requestId;
    pc.epoch = _epoch;
    pc.spellId = uint32(env.payload.find("spell_id")->asInt());

    auto reject = [&](char const* reason) {
        LOG_INFO("module.gamebridge", "CAST_REQUEST {} spell {} rejected by bridge validation: {}", pc.requestId, pc.spellId, reason);
        SendCastStatus(pc, "rejected", reason, 0);
    };

    if (!_testMode)
        return reject("not_in_test_mode");
    if (env.payload.find("caster_id")->asString() != kPlayerEntityId)
        return reject("caster_not_controlled");
    BoundEntity* target = FindEntity(env.payload.find("target_id")->asString());
    if (!target || target->isPlayer)
        return reject("unknown_target");
    if (!target->hostGeneration || uint64(env.payload.find("host_generation")->asInt()) != target->hostGeneration)
        return reject("stale_target_generation");
    if (std::find(_cfg.spellIds.begin(), _cfg.spellIds.end(), pc.spellId) == _cfg.spellIds.end())
        return reject("spell_not_permitted");

    Player* player = GetPlayer();
    Creature* creature = player->GetMap()->GetCreature(target->guid);
    if (!creature)
        return reject("target_missing");

    // Allocate a native cast_count (1..255) that is not in flight.
    uint8 cc = 0;
    for (int i = 0; i < 255; ++i)
    {
        _castCounter = uint8(_castCounter % 255 + 1);
        if (!_pendingCasts.count(_castCounter))
        {
            cc = _castCounter;
            break;
        }
    }
    if (!cc)
        return reject("too_many_pending_casts");
    _pendingCasts[cc] = pc;

    uint32 hpBefore = creature->GetHealth();
    uint32 powerBefore = player->GetPower(player->getPowerType());

    // CMSG_CAST_SPELL: uint8 castCount, uint32 spellId, uint8 castFlags, SpellCastTargets.
    WorldPacket pkt(CMSG_CAST_SPELL, 4 + 1 + 1 + 4 + 9);
    pkt << uint8(cc) << uint32(pc.spellId) << uint8(0);
    SpellCastTargets targets;
    targets.SetUnitTarget(creature);
    targets.Write(pkt);
    LOG_INFO("module.gamebridge", "CAST_REQUEST {} -> native HandleCastSpellOpcode(castCount {}, spell {}, target {}) "
             "target hp {}/{} caster power {}",
             pc.requestId, uint32(cc), pc.spellId, GuidString(creature->GetGUID()), hpBefore, creature->GetMaxHealth(), powerBefore);
    _session->HandleCastSpellOpcode(pkt);

    DrainHookEvents();
    auto it = _pendingCasts.find(cc);
    if (it == _pendingCasts.end() || it->second.accepted)
        return;  // native outcome already reported
    bool queued = std::any_of(player->SpellQueue.begin(), player->SpellQueue.end(),
                              [&](PendingSpellCastRequest const& r) { return r.spellId == pc.spellId; });
    if (queued)
    {
        LOG_INFO("module.gamebridge", "CAST_REQUEST {} held in native spell queue", pc.requestId);
        return;  // the native queue will execute it (or it is cleared on reset)
    }
    // The native handler returned without starting or failing the spell (e.g. spell not in spellbook).
    LOG_INFO("module.gamebridge", "CAST_REQUEST {} ignored by native handler (not in spellbook/passive?)", pc.requestId);
    SendCastStatus(it->second, "rejected", "native_handler_ignored", 0);
    _pendingCasts.erase(it);
}

// ---------------------------------------------------------------------------
// hooks (any thread)
// ---------------------------------------------------------------------------

void Bridge::HookPacketSent(WorldSession* session, WorldPacket const& packet)
{
    if (!session || !session->IsHeadless())
        return;
    {
        std::lock_guard<std::mutex> g(_hookMx);
        if (session != _hookSession)
            return;
    }
    uint16 op = packet.GetOpcode();
    if (op != SMSG_CAST_FAILED && op != SMSG_SPELLNONMELEEDAMAGELOG && op != SMSG_CHAR_CREATE)
        return;
    HookEvent ev;
    ev.thread = ThreadTag();
    try
    {
        WorldPacket p(packet);
        p.rpos(0);
        if (op == SMSG_CAST_FAILED)
        {
            uint8 castCount, result;
            uint32 spellId;
            p >> castCount >> spellId >> result;
            ev.kind = HookEvent::Kind::CastFailed;
            ev.castCount = castCount;
            ev.spellId = spellId;
            ev.result = result;
        }
        else if (op == SMSG_CHAR_CREATE)
        {
            uint8 code;
            p >> code;
            ev.kind = HookEvent::Kind::CharCreateResult;
            ev.result = code;
        }
        else
        {
            // Unit::SendSpellNonMeleeDamageLog layout
            uint64 target, attacker;
            uint32 spellId, damage, overkill, absorb, resist, blocked, hitInfo;
            uint8 school, physical, unused;
            p.readPackGUID(target);
            p.readPackGUID(attacker);
            p >> spellId >> damage >> overkill >> school >> absorb >> resist >> physical >> unused >> blocked >> hitInfo;
            ev.kind = HookEvent::Kind::SpellDamage;
            ev.target = ObjectGuid(target);
            ev.source = ObjectGuid(attacker);
            ev.spellId = spellId;
            ev.amount = damage;
            ev.school = school;
            ev.critical = (hitInfo & SPELL_HIT_TYPE_CRIT) != 0;
            std::lock_guard<std::mutex> g(_hookMx);
            if (!_hookBound.count(target) && !_hookBound.count(attacker))
                return;
        }
    }
    catch (ByteBufferException const&)
    {
        return;
    }
    std::lock_guard<std::mutex> g(_hookMx);
    if (_hookEvents.size() >= kMaxHookEvents)
    {
        _hookOverflow = true;
        return;
    }
    _hookEvents.push_back(ev);
}

void Bridge::HookSpellPrepare(Spell* spell, Unit* caster)
{
    if (!caster || !spell->m_cast_count)
        return;
    std::lock_guard<std::mutex> g(_hookMx);
    if (caster->GetGUID() != _hookPlayer || _hookEvents.size() >= kMaxHookEvents)
        return;
    HookEvent ev;
    ev.thread = ThreadTag();
    ev.kind = HookEvent::Kind::SpellPrepared;
    ev.castCount = spell->m_cast_count;
    ev.spellId = spell->m_spellInfo->Id;
    _hookEvents.push_back(ev);
}

void Bridge::HookSpellCast(Spell* spell, Unit* caster)
{
    if (!caster || !spell->m_cast_count)
        return;
    std::lock_guard<std::mutex> g(_hookMx);
    if (caster->GetGUID() != _hookPlayer || _hookEvents.size() >= kMaxHookEvents)
        return;
    HookEvent ev;
    ev.thread = ThreadTag();
    ev.kind = HookEvent::Kind::SpellCast;
    ev.castCount = spell->m_cast_count;
    ev.spellId = spell->m_spellInfo->Id;
    _hookEvents.push_back(ev);
}

void Bridge::HookSpellCancel(Spell* spell, Unit* caster)
{
    if (!caster || !spell->m_cast_count)
        return;
    std::lock_guard<std::mutex> g(_hookMx);
    if (caster->GetGUID() != _hookPlayer || _hookEvents.size() >= kMaxHookEvents)
        return;
    HookEvent ev;
    ev.thread = ThreadTag();
    ev.kind = HookEvent::Kind::SpellCancel;
    ev.castCount = spell->m_cast_count;
    ev.spellId = spell->m_spellInfo->Id;
    _hookEvents.push_back(ev);
}

uint32 Bridge::HookDealDamage(Unit* attacker, Unit* victim, uint32 damage)
{
    if (!victim || !damage || !_cfg.isolateEntities)
        return damage;
    std::lock_guard<std::mutex> g(_hookMx);
    if (!_hookBound.count(victim->GetGUID().GetRawValue()))
        return damage;
    if (attacker && attacker->GetGUID() == _hookPlayer)
        return damage;   // bridge-initiated native combat
    if (attacker && attacker->GetGUID() == victim->GetGUID() && victim->GetGUID() == _hookPlayer)
        return damage;
    if (_hookEvents.size() < kMaxHookEvents)
    {
        HookEvent ev;
    ev.thread = ThreadTag();
        ev.kind = HookEvent::Kind::DamageBlocked;
        ev.target = victim->GetGUID();
        ev.source = attacker ? attacker->GetGUID() : ObjectGuid::Empty;
        ev.amount = damage;
        _hookEvents.push_back(ev);
    }
    return 0;   // M1 arena isolation: only bridge-initiated combat changes bridge entities
}

// ---------------------------------------------------------------------------
// results and state (world thread)
// ---------------------------------------------------------------------------

void Bridge::DrainHookEvents()
{
    std::deque<HookEvent> events;
    bool overflow;
    {
        std::lock_guard<std::mutex> g(_hookMx);
        events.swap(_hookEvents);
        overflow = _hookOverflow;
        _hookOverflow = false;
    }
    if (overflow && _testMode)
    {
        LOG_ERROR("module.gamebridge", "hook event overflow: resetting fixture");
        ExitTestMode("core_event_overflow", true);
    }

    auto entityIdOf = [&](ObjectGuid guid) -> Json {
        for (auto const& [id, e] : _entities)
            if (e.guid == guid)
                return Json::string(id);
        return Json::null();
    };

    for (HookEvent const& ev : events)
    {
        switch (ev.kind)
        {
            case HookEvent::Kind::CharCreateResult:
                if (_phase != Phase::CreatingCharacter)
                    break;
                if (ev.result == CHAR_CREATE_SUCCESS)
                {
                    // The cache entry is added in the same callback, before SendCharCreate.
                    _playerGuid = sCharacterCache->GetCharacterGuidByName(_cfg.characterName);
                    LOG_INFO("module.gamebridge", "native character creation succeeded: {}", GuidString(_playerGuid));
                    BeginLogin();
                }
                else
                {
                    LOG_ERROR("module.gamebridge", "native character creation failed with response code {}", ev.result);
                    _phase = Phase::Failed;
                }
                break;
            case HookEvent::Kind::CastFailed:
            {
                auto it = _pendingCasts.find(ev.castCount);
                if (it == _pendingCasts.end() || it->second.spellId != ev.spellId)
                    break;
                char const* name = EnumUtils::ToString(SpellCastResult(ev.result)).Constant;
                bool interrupted = it->second.accepted;
                LOG_INFO("module.gamebridge", "native cast result for {}: {} ({})", it->second.requestId, name,
                         interrupted ? "interrupted" : "rejected");
                SendCastStatus(it->second, interrupted ? "interrupted" : "rejected", name, ev.result);
                _pendingCasts.erase(it);
                break;
            }
            case HookEvent::Kind::SpellPrepared:
            {
                auto it = _pendingCasts.find(ev.castCount);
                if (it == _pendingCasts.end() || it->second.accepted)
                    break;
                it->second.accepted = true;
                LOG_INFO("module.gamebridge", "native Spell::prepare accepted {} (spell {}) [hook thread {}, world thread {}]",
                         it->second.requestId, ev.spellId, ev.thread, _worldThread);
                SendCastStatus(it->second, "accepted", std::string(), 0);
                break;
            }
            case HookEvent::Kind::SpellCast:
            {
                auto it = _pendingCasts.find(ev.castCount);
                if (it == _pendingCasts.end())
                    break;
                if (!it->second.accepted)
                {
                    it->second.accepted = true;
                    SendCastStatus(it->second, "accepted", std::string(), 0);
                }
                Player* player = GetPlayer();
                LOG_INFO("module.gamebridge", "native Spell::cast executed {} (spell {}); caster power now {} [hook thread {}, world thread {}]",
                         it->second.requestId, ev.spellId, player ? player->GetPower(player->getPowerType()) : 0, ev.thread, _worldThread);
                SendCastStatus(it->second, "completed", std::string(), 0);
                _pendingCasts.erase(it);
                break;
            }
            case HookEvent::Kind::SpellCancel:
            {
                auto it = _pendingCasts.find(ev.castCount);
                if (it == _pendingCasts.end())
                    break;
                SendCastStatus(it->second, "interrupted", "SPELL_CANCELLED", 0);
                _pendingCasts.erase(it);
                break;
            }
            case HookEvent::Kind::SpellDamage:
            {
                Json p = Json::object();
                p.set("event_id", Json::string("ev-" + std::to_string(++_eventCounter)));
                p.set("kind", Json::string("damage"));
                p.set("source_id", entityIdOf(ev.source));
                p.set("target_id", entityIdOf(ev.target));
                p.set("amount", Json::integer(ev.amount));
                p.set("school", Json::integer(ev.school));
                p.set("critical", Json::boolean(ev.critical));
                p.set("spell_id", Json::integer(ev.spellId));
                LOG_INFO("module.gamebridge", "native damage log: spell {} {} -> {} amount {} school {} crit {} [hook thread {}]",
                         ev.spellId, GuidString(ev.source), GuidString(ev.target), ev.amount, ev.school, ev.critical, ev.thread);
                Send(MsgType::CombatEvent, p);   // presentation only; never applied as HP
                break;
            }
            case HookEvent::Kind::DamageBlocked:
                LOG_INFO("module.gamebridge", "arena isolation blocked {} damage from {} to bridge entity {}",
                         ev.amount, GuidString(ev.source), GuidString(ev.target));
                break;
        }
    }
}

Json Bridge::EntityStateJson(BoundEntity const& e) const
{
    BoundEntity::State const& st = e.published;
    Json s = Json::object();
    s.set("entity_id", Json::string(e.id));
    s.set("generation", Json::integer(int64(e.generation)));
    s.set("revision", Json::integer(int64(e.revision)));
    s.set("hp", Json::integer(st.hp));
    s.set("max_hp", Json::integer(st.maxHp));
    s.set("alive", Json::boolean(st.alive));
    s.set("power_type", Json::string(PowerName(st.powerType)));
    s.set("power", Json::integer(st.power));
    s.set("max_power", Json::integer(st.maxPower));
    s.set("casting_spell_id", Json::integer(st.casting));
    return s;
}

void Bridge::PublishState(uint32 diff, bool forceFull)
{
    if (!_testMode || _epoch.empty())
        return;
    _stateTimer += diff;
    _fullTimer += diff;
    Player* player = GetPlayer();
    Map* map = player->GetMap();

    std::vector<std::string> gone;
    std::vector<std::pair<BoundEntity*, BoundEntity::State>> dirty;
    bool urgent = false;
    for (auto& [id, e] : _entities)
    {
        Unit* u = e.isPlayer ? static_cast<Unit*>(player) : static_cast<Unit*>(map->GetCreature(e.guid));
        if (!u || !u->IsInWorld())
        {
            gone.push_back(id);
            continue;
        }
        BoundEntity::State cur;
        cur.powerType = uint8(u->getPowerType());
        cur.hp = u->GetHealth();
        cur.maxHp = u->GetMaxHealth();
        cur.power = u->GetPower(Powers(cur.powerType));
        cur.maxPower = u->GetMaxPower(Powers(cur.powerType));
        Spell* spell = u->GetCurrentSpell(CURRENT_GENERIC_SPELL);
        cur.casting = spell && spell->getState() == SPELL_STATE_PREPARING ? spell->m_spellInfo->Id : 0;
        cur.alive = u->IsAlive();
        if (!e.havePublished || !(cur == e.published))
        {
            if (!e.havePublished || cur.alive != e.published.alive)
                urgent = true;   // first state and deaths are published immediately
            dirty.emplace_back(&e, cur);
        }
        if (!cur.alive && !e.deathAnnounced)
        {
            e.deathAnnounced = true;
            Json p = Json::object();
            p.set("event_id", Json::string("ev-" + std::to_string(++_eventCounter)));
            p.set("kind", Json::string("death"));
            p.set("source_id", Json::null());
            p.set("target_id", Json::string(e.id));
            p.set("amount", Json::integer(0));
            p.set("school", Json::integer(0));
            p.set("critical", Json::boolean(false));
            LOG_INFO("module.gamebridge", "native death of {} ({})", e.id, GuidString(e.guid));
            Send(MsgType::CombatEvent, p);   // presentation; the snapshot below carries alive=false
        }
    }

    bool const sendFull = forceFull || _fullTimer >= _cfg.fullSnapshotIntervalMs;
    bool const sendDelta = !dirty.empty() && (urgent || _stateTimer >= _cfg.stateMinIntervalMs);
    if (sendFull || sendDelta)
    {
        if (!dirty.empty())
        {
            ++_revision;
            for (auto& [e, cur] : dirty)
            {
                e->published = cur;
                e->havePublished = true;
                e->revision = _revision;
                LOG_INFO("module.gamebridge", "state rev {}: {} hp {}/{} alive {} {} {}/{} casting {}", e->revision, e->id,
                         cur.hp, cur.maxHp, cur.alive ? 1 : 0, PowerName(cur.powerType), cur.power, cur.maxPower, cur.casting);
            }
        }
        Json ents = Json::array();
        for (auto const& [id, e] : _entities)
        {
            if (!e.havePublished || std::find(gone.begin(), gone.end(), id) != gone.end())
                continue;
            bool isDirty = std::any_of(dirty.begin(), dirty.end(), [&](auto const& d) { return d.first == &e; });
            if (sendFull || isDirty)
                ents.push(EntityStateJson(e));
        }
        Json p = Json::object();
        p.set("full", Json::boolean(sendFull));
        p.set("revision", Json::integer(int64(_revision)));
        p.set("entities", ents);
        // Snapshots are never coalesced: each entity state carries its revision.
        Send(MsgType::StateSnapshot, p);
        _stateTimer = 0;
        if (sendFull)
            _fullTimer = 0;
    }

    for (std::string const& id : gone)
    {
        Json p = Json::object();
        p.set("entity_id", Json::string(id));
        p.set("final_revision", Json::integer(int64(_revision)));
        p.set("reason", Json::string("core_removed"));
        Send(MsgType::EntityDespawn, p);
        {
            std::lock_guard<std::mutex> g(_hookMx);
            _hookBound.erase(_entities[id].guid.GetRawValue());
        }
        LOG_INFO("module.gamebridge", "bound entity {} no longer in world", id);
        _entities.erase(id);
        if (id == kPlayerEntityId)
            ExitTestMode("player_removed", true);
    }
}

} // namespace GameBridge
