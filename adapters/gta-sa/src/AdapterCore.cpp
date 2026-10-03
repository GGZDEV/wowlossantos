#include "AdapterCore.h"

#include <cmath>
#include <cstdlib>
#include <cstdio>

using gamebridge::Envelope;
using gamebridge::Json;
using gamebridge::MsgType;

namespace ata {

AdapterCore::AdapterCore(AdapterConfig cfg, GameApi& game) : _cfg(std::move(cfg)), _game(game)
{
}

AdapterCore::~AdapterCore()
{
    stop();
}

void AdapterCore::start()
{
    if (_client)
        return;
    gamebridge::HostClient::Config hc;
    hc.port = _cfg.port;
    hc.token = _cfg.token;
    hc.adapter = "gta-sa";
    // The logger is called from the IO thread; GameApi::log implementations must be thread-safe.
    _client = std::make_unique<gamebridge::HostClient>(hc, [this](std::string const& s) { _game.log("[io] " + s); });
    _client->start();
    log("adapter started; connecting to 127.0.0.1:" + std::to_string(_cfg.port));
}

void AdapterCore::stop()
{
    if (!_client)
        return;
    _client->stop();
    _client.reset();
}

int AdapterCore::targetHandle() const
{
    for (auto const& [id, b] : _bindings)
        if (!b.isPlayer)
            return b.handle;
    return 0;
}

void AdapterCore::dropBindings(char const* why)
{
    for (auto& [id, b] : _bindings)
        if (!b.isPlayer && b.handle)
        {
            if (_game.pedExists(b.handle))
                _game.deletePed(b.handle);
            log("released target ped (" + std::string(why) + ")");
        }
    _bindings.clear();
    _revisions.reset();
    _targetHp = -1;
}

void AdapterCore::leaveTest(char const* why, bool notifyCore)
{
    if (notifyCore && _client && _client->connected())
    {
        Json p = Json::object();
        p.set("reason", Json::string(why));
        _client->send(MsgType::Reset, p);
    }
    dropBindings(why);
    _game.setPlayerProofs(false);
    _entering = false;
    _status = std::string("test off (") + why + ")";
}

void AdapterCore::onGameRestart()
{
    // New game / load: every ped handle from before is invalid; never touch them.
    for (auto& [id, b] : _bindings)
        b.handle = 0;
    _bindings.clear();
    _revisions.reset();
    if (_wantTest)
    {
        _wantTest = false;
        if (_client && _client->connected())
        {
            Json p = Json::object();
            p.set("reason", Json::string("gta_game_restart"));
            _client->send(MsgType::Reset, p);
        }
    }
    _entering = false;
    _paused = false;
    _status = "test off (game restart)";
    log("game restart/load: bridge bindings discarded");
}

void AdapterCore::sendEnterTest()
{
    Vec3 pos;
    float heading;
    if (!_client || !_client->connected() || !_welcome || !_game.playerPosition(pos, heading))
        return;
    Json p = Json::object();
    p.set("test_fixture_id", Json::string(_cfg.fixtureId));
    Json o = Json::object();
    o.set("x", Json::number(pos.x));
    o.set("y", Json::number(pos.y));
    o.set("z", Json::number(pos.z));
    p.set("observed_player_position", o);
    std::string id = _client->nextRequestId("enter");
    _client->send(MsgType::EnterTest, p, id);
    _entering = true;
    _game.setPlayerProofs(true);
    _status = "entering test";
    log("ENTER_TEST " + id + " at gta (" + std::to_string(pos.x) + ", " + std::to_string(pos.y) + ", " + std::to_string(pos.z) + ")");
}

void AdapterCore::sendCast()
{
    if (!_client || !_client->connected() || _spells.empty())
        return;
    Binding const* target = nullptr;
    for (auto const& [id, b] : _bindings)
        if (!b.isPlayer)
            target = &b;
    if (!target || !target->hostGeneration || _playerEntityId.empty())
    {
        _lastCastStatus = "no bound target";
        return;
    }
    Json p = Json::object();
    p.set("caster_id", Json::string(_playerEntityId));
    p.set("target_id", Json::string(target->id));
    p.set("spell_id", Json::integer(_spells.front()));
    p.set("host_generation", Json::integer(target->hostGeneration));
    std::string id = _client->nextRequestId("cast");
    if (_client->send(MsgType::CastRequest, p, id))
    {
        _lastCastStatus = id + " sent";
        char buf[160];
        std::snprintf(buf, sizeof(buf), "CAST_REQUEST %s spell %u target %s hp_before(mirror)=%.0f", id.c_str(),
                      _spells.front(), target->id.c_str(), target->hp);
        log(buf);
    }
}

void AdapterCore::onFrame(std::uint64_t nowMs, FrameInput const& input)
{
    // ---- network -> game state ----
    if (_client)
    {
        gamebridge::HostClient::Inbound in;
        for (int i = 0; i < 128 && _client->pollInbound(in); ++i)
            handle(in);
    }

    // ---- pause / menus / loading: M1 implements pause as reset ----
    if (input.menuOrLoading != _paused)
    {
        _paused = input.menuOrLoading;
        if (_wantTest && _client && _client->connected())
        {
            Json p = Json::object();
            p.set("paused", Json::boolean(_paused));
            p.set("reason", Json::string("gta_menu_or_loading"));
            _client->send(MsgType::SetPaused, p, _client->nextRequestId("pause"));
            if (_paused)
            {
                dropBindings("paused");
                _status = "paused (fixture reset)";
            }
            else
                sendEnterTest();   // fresh recreation on resume
        }
    }
    if (_paused)
        return;   // ignore input while menus/loading are active

    if (input.toggleTest)
    {
        _wantTest = !_wantTest;
        log(std::string("F9: test mode ") + (_wantTest ? "requested" : "off"));
        if (_wantTest)
            sendEnterTest();
        else
            leaveTest("user_toggle", true);
    }
    if (input.resetFixture && _wantTest)
    {
        log("F8: fixture reset");
        dropBindings("fixture_reset");
        sendEnterTest();
    }
    if (input.cast && _wantTest)
        sendCast();

    if (!_wantTest)
        return;

    // ---- validate host entities and re-assert absolute state ----
    for (auto it = _bindings.begin(); it != _bindings.end();)
    {
        Binding& b = it->second;
        if (!b.isPlayer && b.handle && !_game.pedExists(b.handle))
        {
            log("target ped lost on host (streamed out / destroyed)");
            if (_client && _client->connected())
            {
                Json p = Json::object();
                p.set("entity_id", Json::string(b.id));
                p.set("generation", Json::integer(b.coreGeneration));
                p.set("reason", Json::string("host_handle_invalid"));
                _client->send(MsgType::HostEntityLost, p);
            }
            it = _bindings.erase(it);
            _targetHp = -1;
            continue;
        }
        if (!b.isPlayer && b.handle && b.haveState && !b.deathPresented)
            _game.setPedHealth(b.handle, b.hp, b.maxHp);   // absolute value, never a delta
        ++it;
    }

    // ---- movement observations (bridge rate, coalesced) ----
    auto pit = _bindings.find(_playerEntityId);
    if (pit != _bindings.end() && nowMs - _lastPositionMs >= _cfg.positionIntervalMs && _client && _client->connected())
    {
        Vec3 pos;
        float heading;
        if (_game.playerPosition(pos, heading) && std::isfinite(heading))
        {
            Json p = Json::object();
            p.set("entity_id", Json::string(_playerEntityId));
            p.set("generation", Json::integer(pit->second.coreGeneration));
            p.set("sample_id", Json::integer(++_sampleId));
            Json o = Json::object();
            o.set("x", Json::number(pos.x));
            o.set("y", Json::number(pos.y));
            o.set("z", Json::number(pos.z));
            p.set("position", o);
            p.set("orientation", Json::number(heading));
            _client->send(MsgType::Position, p, std::string(), "pos:" + _playerEntityId);
            _lastPositionMs = nowMs;
        }
    }
}

void AdapterCore::handle(gamebridge::HostClient::Inbound const& in)
{
    using Kind = gamebridge::HostClient::Inbound::Kind;
    if (in.kind == Kind::Connected)
    {
        // Fresh epoch: nothing from before is synchronised.
        dropBindings("new_epoch");
        _events.reset();
        _welcome = true;
        _playerEntityId = in.env.payload.find("player_entity_id")->asString();
        _spells.clear();
        if (Json const* s = in.env.payload.find("spell_ids"))
            for (Json const& v : s->items())
                _spells.push_back(static_cast<std::uint32_t>(std::strtoul(v.asString().c_str(), nullptr, 10)));
        log("WELCOME epoch " + in.epoch + " core " + in.env.payload.find("core_sha")->asString());
        _status = "connected";
        if (_wantTest)
            sendEnterTest();
        return;
    }
    if (in.kind == Kind::Disconnected)
    {
        _welcome = false;
        dropBindings("disconnected");   // freeze: no stale state shown as synchronised
        _status = "DISCONNECTED (" + in.reason + ")";
        log("core connection lost: " + in.reason);
        return;
    }
    Envelope const& env = in.env;
    switch (env.type)
    {
        case MsgType::EntityBind: handleBind(env); break;
        case MsgType::StateSnapshot: handleSnapshot(env); break;
        case MsgType::CastStatus:
        {
            Json const* reason = env.payload.find("reason");
            _lastCastStatus = env.requestId + " " + env.payload.find("status")->asString() +
                              (reason && reason->isString() ? " (" + reason->asString() + ")" : "");
            log("CAST_STATUS " + _lastCastStatus);
            break;
        }
        case MsgType::CombatEvent:
        {
            if (!_events.firstTime(env.payload.find("event_id")->asString()))
                break;   // duplicate presentation event
            // Presentation only. Health comes exclusively from STATE_SNAPSHOT.
            char buf[160];
            std::snprintf(buf, sizeof(buf), "%s %s amount %lld%s", env.payload.find("kind")->asString().c_str(),
                          env.payload.find("target_id")->isString() ? env.payload.find("target_id")->asString().c_str() : "-",
                          static_cast<long long>(env.payload.find("amount")->asInt()),
                          env.payload.find("critical")->asBool() ? " CRIT" : "");
            _lastEvent = buf;
            log(std::string("COMBAT_EVENT ") + buf);
            break;
        }
        case MsgType::EntityDespawn:
        {
            auto it = _bindings.find(env.payload.find("entity_id")->asString());
            if (it != _bindings.end())
            {
                if (!it->second.isPlayer && it->second.handle && _game.pedExists(it->second.handle))
                    _game.deletePed(it->second.handle);
                _revisions.forget(it->first);
                _bindings.erase(it);
                _targetHp = -1;
            }
            log("ENTITY_DESPAWN " + env.payload.find("entity_id")->asString() + " (" + env.payload.find("reason")->asString() + ")");
            break;
        }
        case MsgType::Error:
            log("core ERROR " + env.payload.find("code")->asString() + ": " + env.payload.find("description")->asString());
            _status = "error: " + env.payload.find("code")->asString();
            break;
        case MsgType::Reset:
            dropBindings("core_reset");
            break;
        case MsgType::PauseAck:
            log(std::string("PAUSE_ACK paused=") + (env.payload.find("paused")->asBool() ? "1" : "0"));
            break;
        default:
            break;
    }
}

void AdapterCore::handleBind(Envelope const& env)
{
    std::string id = env.payload.find("entity_id")->asString();
    bool isPlayer = env.payload.find("kind")->asString() == "player";
    std::int64_t gen = env.payload.find("generation")->asInt();

    auto old = _bindings.find(id);
    if (old != _bindings.end())
    {
        if (old->second.coreGeneration == gen)
            return;   // duplicate bind (e.g. after RESYNC)
        if (!old->second.isPlayer && old->second.handle && _game.pedExists(old->second.handle))
            _game.deletePed(old->second.handle);
        _revisions.forget(id);
        _bindings.erase(old);
    }

    Binding b;
    b.id = id;
    b.isPlayer = isPlayer;
    b.coreGeneration = gen;
    if (!isPlayer)
    {
        Json const* hp = env.payload.find("host_position");
        if (!hp || !_wantTest)
            return;
        Vec3 pos { float(hp->find("x")->asDouble()), float(hp->find("y")->asDouble()), float(hp->find("z")->asDouble()) };
        Vec3 me;
        float heading = 0;
        _game.playerPosition(me, heading);
        float faceMe = std::atan2(-(me.x - pos.x), me.y - pos.y);   // GTA heading towards the player
        b.handle = _game.spawnTargetPed(pos, faceMe, _cfg.targetModelId);
        if (!b.handle)
        {
            log("failed to spawn target ped");
            Json p = Json::object();
            p.set("entity_id", Json::string(id));
            p.set("generation", Json::integer(gen));
            p.set("reason", Json::string("host_spawn_failed"));
            _client->send(MsgType::HostEntityLost, p);
            return;
        }
        _game.setPedProofs(b.handle, true);
        log("bound " + id + " gen " + std::to_string(gen) + " -> ped handle " + std::to_string(b.handle));
    }
    b.hostGeneration = ++_hostGenerationCounter;
    Json ack = Json::object();
    ack.set("entity_id", Json::string(id));
    ack.set("host_generation", Json::integer(b.hostGeneration));
    _client->send(MsgType::HostBindAck, ack);
    _bindings[id] = b;
    _entering = false;
    _status = "TEST ACTIVE";
}

void AdapterCore::handleSnapshot(Envelope const& env)
{
    for (Json const& e : env.payload.find("entities")->items())
    {
        std::string id = e.find("entity_id")->asString();
        auto it = _bindings.find(id);
        if (it == _bindings.end())
            continue;
        Binding& b = it->second;
        std::int64_t gen = e.find("generation")->asInt();
        if (gen != b.coreGeneration)
            continue;   // state for another incarnation
        if (!_revisions.accept(id, gen, e.find("revision")->asInt()))
            continue;   // equal/older revision never rolls back newer state
        float hp = float(e.find("hp")->asInt());
        float maxHp = float(e.find("max_hp")->asInt());
        bool alive = e.find("alive")->asBool();
        if (b.isPlayer)
        {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "player hp %.0f/%.0f %s %lld/%lld", hp, maxHp,
                          e.find("power_type") ? e.find("power_type")->asString().c_str() : "",
                          e.find("power") ? static_cast<long long>(e.find("power")->asInt()) : 0LL,
                          e.find("max_power") ? static_cast<long long>(e.find("max_power")->asInt()) : 0LL);
            _playerLine = buf;
            continue;
        }
        if (b.dead && alive)
        {
            log("ignoring alive=true after death for the same generation");
            continue;   // no accidental respawn from stale state
        }
        b.hp = hp;
        b.maxHp = maxHp;
        b.haveState = true;
        b.dead = !alive;
        _targetHp = hp;
        if (b.handle && _game.pedExists(b.handle))
        {
            _game.setPedHealth(b.handle, hp, maxHp);
            if (b.dead && !b.deathPresented)
            {
                b.deathPresented = true;
                ++_deathPresentations;
                _game.presentDeath(b.handle);
                log("target died on core: death presented once");
            }
        }
        char buf[128];
        std::snprintf(buf, sizeof(buf), "STATE %s rev %lld hp %.0f/%.0f alive %d", id.c_str(),
                      static_cast<long long>(e.find("revision")->asInt()), hp, maxHp, alive ? 1 : 0);
        log(buf);
    }
}

std::vector<std::string> AdapterCore::overlay() const
{
    std::vector<std::string> lines;
    lines.push_back(std::string("ATA bridge: ") + (connected() ? "connected" : "offline") + " | " + _status);
    char buf[96];
    if (_targetHp >= 0)
        std::snprintf(buf, sizeof(buf), "target hp (core): %.0f", _targetHp);
    else
        std::snprintf(buf, sizeof(buf), "target: none");
    lines.push_back(buf);
    lines.push_back(_playerLine);
    lines.push_back("last cast: " + _lastCastStatus);
    lines.push_back("last event: " + _lastEvent);
    return lines;
}

} // namespace ata
