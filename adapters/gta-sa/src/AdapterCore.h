// Azeroth Theft Auto - GTA SA adapter logic, independent of Plugin-SDK.
//
// All methods are called from GTA's main game thread (gameProcessEvent /
// drawingEvent). Network IO happens on gamebridge::HostClient's own thread and
// only reaches this class through its bounded queue. Every engine interaction
// goes through GameApi so that the same logic runs in-game and in host tests.
#pragma once

#include "gamebridge/endpoint.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ata {

struct Vec3
{
    float x = 0, y = 0, z = 0;
};

// Engine seam. The in-game implementation uses Plugin-SDK script commands; handles
// are GTA script handles, which embed the pool slot generation (CPools::GetPedRef),
// so a recycled slot never validates as the old ped.
class GameApi
{
public:
    virtual ~GameApi() = default;
    virtual bool playerPosition(Vec3& pos, float& headingRad) = 0;
    virtual int spawnTargetPed(Vec3 const& pos, float headingRad, int modelId) = 0;   // 0 = failed
    virtual bool pedExists(int handle) = 0;
    virtual void deletePed(int handle) = 0;
    // Absolute state only: sets the ped's health/max health fields.
    virtual void setPedHealth(int handle, float hp, float maxHp) = 0;
    virtual void presentDeath(int handle) = 0;
    // Scope native GTA damage away from bridge entities (bullet/fire/explosion/collision/melee proofs).
    virtual void setPedProofs(int handle, bool on) = 0;
    virtual void setPlayerProofs(bool on) = 0;
    virtual void log(std::string const& line) = 0;
};

struct AdapterConfig
{
    std::uint16_t port = gamebridge::kDefaultPort;
    std::string token;
    std::string fixtureId = "m1-arena";
    int targetModelId = 7;              // stock ped model used as placeholder
    unsigned positionIntervalMs = 50;   // 20 Hz telemetry (bridge rate, not combat tick)
};

struct FrameInput
{
    bool toggleTest = false;   // edge-triggered (F9)
    bool resetFixture = false; // edge-triggered (F8)
    bool cast = false;         // edge-triggered (key 1)
    bool menuOrLoading = false;
};

class AdapterCore
{
public:
    AdapterCore(AdapterConfig cfg, GameApi& game);
    ~AdapterCore();

    void start();     // starts the network thread (never from DllMain)
    void stop();

    void onFrame(std::uint64_t nowMs, FrameInput const& input);
    void onGameRestart();                 // new game / load: bindings are invalid
    std::vector<std::string> overlay() const;

    // Introspection for tests/overlay
    bool testMode() const { return _wantTest; }
    bool connected() const { return _client && _client->connected(); }
    int targetHandle() const;
    float targetHp() const { return _targetHp; }
    std::string lastCastStatus() const { return _lastCastStatus; }
    int deathPresentations() const { return _deathPresentations; }

private:
    struct Binding
    {
        std::string id;
        bool isPlayer = false;
        std::int64_t coreGeneration = 0;
        std::int64_t hostGeneration = 0;
        int handle = 0;               // GTA script handle (target only)
        bool dead = false;
        bool deathPresented = false;
        float hp = 0, maxHp = 0;
        bool haveState = false;
    };

    void handle(gamebridge::HostClient::Inbound const& in);
    void handleBind(gamebridge::Envelope const& env);
    void handleSnapshot(gamebridge::Envelope const& env);
    void sendEnterTest();
    void sendCast();
    void dropBindings(char const* why);
    void leaveTest(char const* why, bool notifyCore);
    void log(std::string const& s) { _game.log(s); }

    AdapterConfig _cfg;
    GameApi& _game;
    std::unique_ptr<gamebridge::HostClient> _client;

    bool _wantTest = false;
    bool _paused = false;
    bool _welcome = false;
    bool _entering = false;
    std::string _playerEntityId;
    std::vector<std::uint32_t> _spells;
    std::map<std::string, Binding> _bindings;
    gamebridge::RevisionGate _revisions;
    gamebridge::EventDedupe _events;
    std::int64_t _hostGenerationCounter = 0;
    std::int64_t _sampleId = 0;
    std::uint64_t _lastPositionMs = 0;
    float _targetHp = -1;
    std::string _status = "idle";
    std::string _lastCastStatus = "-";
    std::string _lastEvent = "-";
    std::string _playerLine = "-";
    int _deathPresentations = 0;
};

} // namespace ata
