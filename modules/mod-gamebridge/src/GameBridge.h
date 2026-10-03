/*
 * mod-gamebridge - Azeroth Theft Auto bridge endpoint inside worldserver.
 *
 * Threading contract (audited against the pinned core, see docs/evidence/m0-audit.md):
 *  - The protocol IO thread (gamebridge::CoreEndpoint) never touches game objects.
 *  - All Player/Creature/WorldSession work happens in Bridge::Update(), called from
 *    WorldScript::OnUpdate. World::Update invokes that hook after MapMgr::Update has
 *    waited for every map update thread, in the same phase as CLI commands, so no
 *    map is being updated concurrently.
 *  - Spell/unit/packet hooks may run on map threads. They only record compact
 *    events into a mutex-protected queue, which Update() drains.
 *  - No engine pointer is retained across ticks: entities are re-resolved by GUID.
 */
#ifndef MOD_GAMEBRIDGE_H
#define MOD_GAMEBRIDGE_H

#include "Define.h"
#include "ObjectGuid.h"
#include "Position.h"

#include "gamebridge/endpoint.h"

#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

class Creature;
class Player;
class Spell;
class SpellInfo;
class Unit;
class WorldPacket;
class WorldSession;

namespace GameBridge
{

struct Config
{
    bool enabled = false;
    uint16 port = gamebridge::kDefaultPort;
    std::string token;                // never logged
    std::string accountName = "GTABRIDGE";
    std::string characterName = "Gtabridge";
    bool createIfMissing = true;
    uint8 race = 1;                   // RACE_HUMAN
    uint8 playerClass = 8;            // CLASS_MAGE
    uint8 gender = 0;
    std::string fixtureId = "m1-arena";
    uint32 creatureEntry = 0;
    float targetDistance = 15.0f;     // yards in front of the player
    std::vector<uint32> spellIds;     // whitelisted for CAST_REQUEST
    bool isolateEntities = true;      // block non-bridge damage to bridge entities (M1 arena policy)
    // Arena anchor (core) and projection
    uint32 mapId = 1;
    Position coreOrigin;              // x y z o
    float gtaOrigin[3] = { 0.0f, 0.0f, 0.0f };
    float scale = 1.0936133f;         // core yards per GTA unit (GTA unit ~ 1 m) - uncalibrated default
    float yawRad = -1.5707963f;       // rotation GTA->core; see docs/world-and-authority.md
    float arenaRadius = 40.0f;        // yards around coreOrigin
    float maxSpeed = 20.0f;           // yards/second accepted from POSITION samples
    uint32 stateMinIntervalMs = 100;  // changed state at most 10 Hz
    uint32 fullSnapshotIntervalMs = 1000;
    std::string coreSha;
};

// Compact records produced by hooks (any thread) and consumed by Update().
struct HookEvent
{
    enum class Kind { CastFailed, SpellPrepared, SpellCast, SpellCancel, SpellDamage, CharCreateResult, DamageBlocked };
    Kind kind;
    uint8 castCount = 0;
    uint32 spellId = 0;
    int32 result = 0;
    ObjectGuid source;
    ObjectGuid target;
    uint32 amount = 0;
    uint32 school = 0;
    bool critical = false;
    size_t thread = 0;   // hash of the thread that ran the hook (evidence of execution context)
};

class Bridge
{
public:
    static Bridge& Instance();

    void LoadConfig();
    void Update(uint32 diff);        // world thread only
    void Shutdown();                 // world thread only (WorldScript::OnShutdown)

    // ---- hooks: any thread, cheap filtering, no game-object mutation ----
    void HookPacketSent(WorldSession* session, WorldPacket const& packet);
    void HookSpellPrepare(Spell* spell, Unit* caster);
    void HookSpellCast(Spell* spell, Unit* caster);
    void HookSpellCancel(Spell* spell, Unit* caster);
    uint32 HookDealDamage(Unit* attacker, Unit* victim, uint32 damage);

private:
    enum class Phase { Disabled, Provisioning, CreatingCharacter, LoggingIn, InWorld, Failed };

    struct BoundEntity
    {
        std::string id;
        bool isPlayer = false;
        ObjectGuid guid;
        uint64 generation = 0;        // core incarnation
        uint64 hostGeneration = 0;    // acknowledged by host (0 = not acked)
        uint64 revision = 0;
        struct State
        {
            uint32 hp = 0, maxHp = 0, power = 0, maxPower = 0, casting = 0;
            uint8 powerType = 0;
            bool alive = true;
            bool operator==(State const& o) const
            {
                return hp == o.hp && maxHp == o.maxHp && power == o.power && maxPower == o.maxPower &&
                       casting == o.casting && powerType == o.powerType && alive == o.alive;
            }
        };
        bool havePublished = false;
        State published;              // last state sent to the host (with `revision`)
        bool deathAnnounced = false;
    };

    struct PendingCast
    {
        std::string requestId;
        std::string epoch;
        uint32 spellId = 0;
        bool accepted = false;
    };

    Bridge() = default;

    // lifecycle steps
    void StartEndpoint();
    void Provision();
    void CreateCharacter();
    void BeginLogin();
    void LogoutHeadless(char const* reason);
    Player* GetPlayer() const;

    // protocol handling (world thread)
    void HandleInbound(gamebridge::CoreEndpoint::Inbound& in);
    void SendWelcome();
    void HandleEnterTest(gamebridge::Envelope const& env);
    void HandleCastRequest(gamebridge::Envelope const& env);
    void HandlePosition(gamebridge::Envelope const& env);
    void HandleHostBindAck(gamebridge::Envelope const& env);
    void ExitTestMode(char const* reason, bool notifyHost);
    void DespawnTarget(char const* reason, bool notifyHost);
    bool SpawnTarget(Player* player);
    void DrainHookEvents();
    void PublishState(uint32 diff, bool forceFull);
    void SendCastStatus(PendingCast const& pc, char const* status, std::string const& reason, int32 nativeResult);
    void SendError(std::string const& code, std::string const& requestId, std::string const& description);
    void Send(gamebridge::MsgType type, gamebridge::Json payload, std::string const& requestId = std::string(),
              std::string const& coalesceKey = std::string());
    gamebridge::Json EntityStateJson(BoundEntity const& e) const;
    BoundEntity* FindEntity(std::string const& id);

    // projection
    void HostToCore(float hx, float hy, float hz, float& cx, float& cy, float& cz) const;
    void CoreToHost(float cx, float cy, float cz, float& hx, float& hy, float& hz) const;

    Config _cfg;
    Phase _phase = Phase::Disabled;
    std::unique_ptr<gamebridge::CoreEndpoint> _endpoint;

    // headless session (owned here, never registered in WorldSessionMgr)
    WorldSession* _session = nullptr;
    uint32 _accountId = 0;
    ObjectGuid _playerGuid;
    uint32 _phaseTimer = 0;
    bool _accountCreateIssued = false;
    bool _loginDispatched = false;

    // protocol state (world thread)
    std::string _epoch;
    bool _welcomePending = false;
    bool _testMode = false;
    uint64 _revision = 0;
    uint64 _eventCounter = 0;
    uint64 _generationCounter = 0;
    uint32 _stateTimer = 0;
    uint32 _fullTimer = 0;
    int64 _lastSampleId = -1;
    uint32 _lastSampleMs = 0;
    std::map<std::string, BoundEntity> _entities;   // entity id -> binding
    std::map<uint8, PendingCast> _pendingCasts;     // native cast_count -> request
    uint8 _castCounter = 0;
    size_t _worldThread = 0;

    // hook-shared state
    std::mutex _hookMx;
    WorldSession const* _hookSession = nullptr;      // copy for hook filtering
    ObjectGuid _hookPlayer;                          // copy for hook filtering
    std::unordered_set<uint64> _hookBound;           // raw GUIDs of bound entities
    std::deque<HookEvent> _hookEvents;
    bool _hookOverflow = false;
};

} // namespace GameBridge

#endif
