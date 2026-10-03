// AdapterCore tests: GTA adapter logic against a real CoreEndpoint over loopback,
// with a recording fake GameApi. This proves adapter lifecycle rules only
// (absolute HP, single death presentation, generations, pause/reset); it is NOT
// gameplay proof and does not exercise Plugin-SDK or GTA.
#include "minitest.h"

#include "AdapterCore.h"

#include <chrono>
#include <mutex>
#include <thread>

using namespace gamebridge;
using namespace std::chrono_literals;

namespace {

std::string const kToken = "adapter-test-token-0123456789";

struct FakeGame : ata::GameApi
{
    std::mutex mx;
    int nextSlot = 1;
    std::map<int, float> health;    // live peds -> health
    std::vector<std::string> calls;
    int deaths = 0;
    bool playerProofs = false;

    bool playerPosition(ata::Vec3& pos, float& heading) override
    {
        pos = { 100.0f, 200.0f, 10.0f };
        heading = 0.0f;
        return true;
    }
    int spawnTargetPed(ata::Vec3 const&, float, int) override
    {
        // handle = slot << 8 | generation, like GTA script handles
        int h = (nextSlot++ << 8) | 1;
        health[h] = 100.0f;
        calls.push_back("spawn");
        return h;
    }
    bool pedExists(int h) override { return health.count(h) != 0; }
    void deletePed(int h) override
    {
        health.erase(h);
        calls.push_back("delete");
    }
    void setPedHealth(int h, float hp, float) override
    {
        if (health.count(h))
            health[h] = hp;
    }
    void presentDeath(int) override { ++deaths; }
    void setPedProofs(int, bool) override { calls.push_back("proofs"); }
    void setPlayerProofs(bool on) override { playerProofs = on; }
    void log(std::string const&) override { }
    // simulate GTA native damage the proofs should have prevented
    void gtaHazard(int h, float dmg)
    {
        if (health.count(h))
            health[h] -= dmg;
    }
};

// Scripted core side: real CoreEndpoint, test-controlled responses.
struct ScriptedCore
{
    CoreEndpoint ep;
    std::string epoch;
    std::vector<Envelope> received;

    ScriptedCore() : ep(cfg()) { }
    static CoreEndpoint::Config cfg()
    {
        CoreEndpoint::Config c;
        c.port = 0;
        c.token = kToken;
        c.timings.heartbeatMs = 200;
        return c;
    }
    void pump(ata::AdapterCore& a, std::uint64_t& now, int ms)
    {
        auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < end)
        {
            CoreEndpoint::Inbound in;
            while (ep.pollInbound(in))
            {
                if (in.kind == CoreEndpoint::Inbound::Kind::Connected)
                {
                    epoch = in.epoch;
                    Json w = Json::object();
                    w.set("core_sha", Json::string("scripted"));
                    Json caps = Json::array();
                    caps.push(Json::string("transport_test_only"));
                    w.set("capabilities", caps);
                    w.set("character_id", Json::string("1"));
                    w.set("player_entity_id", Json::string("player-1"));
                    w.set("data_fixture", Json::string("m1-arena"));
                    Json sp = Json::array();
                    sp.push(Json::string("133"));
                    w.set("spell_ids", sp);
                    ep.send(epoch, MsgType::Welcome, w);
                }
                else if (in.kind == CoreEndpoint::Inbound::Kind::Message)
                    received.push_back(in.env);
            }
            a.onFrame(now += 16, ata::FrameInput {});
            std::this_thread::sleep_for(5ms);
        }
    }
    void bind(std::string const& id, char const* kind, int gen)
    {
        Json b = Json::object();
        b.set("entity_id", Json::string(id));
        b.set("kind", Json::string(kind));
        b.set("core_guid", Json::string("0x1"));
        b.set("template_tag", Json::string("t"));
        b.set("generation", Json::integer(gen));
        Json p = Json::object();
        p.set("x", Json::number(110.0));
        p.set("y", Json::number(200.0));
        p.set("z", Json::number(10.0));
        b.set("host_position", p);
        ep.send(epoch, MsgType::EntityBind, b);
    }
    void state(int gen, int rev, int hp, bool alive)
    {
        Json e = Json::object();
        e.set("entity_id", Json::string("target-1"));
        e.set("generation", Json::integer(gen));
        e.set("revision", Json::integer(rev));
        e.set("hp", Json::integer(hp));
        e.set("max_hp", Json::integer(100));
        e.set("alive", Json::boolean(alive));
        Json arr = Json::array();
        arr.push(e);
        Json p = Json::object();
        p.set("full", Json::boolean(false));
        p.set("revision", Json::integer(rev));
        p.set("entities", arr);
        ep.send(epoch, MsgType::StateSnapshot, p);
    }
    void damageEvent(std::string const& id, int amount)
    {
        Json p = Json::object();
        p.set("event_id", Json::string(id));
        p.set("kind", Json::string("damage"));
        p.set("source_id", Json::string("player-1"));
        p.set("target_id", Json::string("target-1"));
        p.set("amount", Json::integer(amount));
        p.set("school", Json::integer(4));
        p.set("critical", Json::boolean(false));
        ep.send(epoch, MsgType::CombatEvent, p);
    }
    int count(MsgType t) const
    {
        int n = 0;
        for (auto const& e : received)
            n += e.type == t;
        return n;
    }
};

ata::AdapterConfig adapterCfg(std::uint16_t port)
{
    ata::AdapterConfig c;
    c.port = port;
    c.token = kToken;
    return c;
}

ata::FrameInput key(bool toggle, bool reset, bool cast, bool menu = false)
{
    ata::FrameInput f;
    f.toggleTest = toggle;
    f.resetFixture = reset;
    f.cast = cast;
    f.menuOrLoading = menu;
    return f;
}

} // namespace

TEST_CASE("adapter: F9 enters test, binds target once, acks with host generation")
{
    ScriptedCore core;
    CHECK(core.ep.start());
    FakeGame game;
    ata::AdapterCore a(adapterCfg(core.ep.port()), game);
    a.start();
    std::uint64_t now = 0;
    core.pump(a, now, 600);
    CHECK(a.connected());
    a.onFrame(now += 16, key(true, false, false));
    core.pump(a, now, 300);
    CHECK_EQ(core.count(MsgType::EnterTest), 1);
    CHECK(game.playerProofs);
    core.bind("player-1", "player", 1);
    core.bind("target-1", "creature", 2);
    core.bind("target-1", "creature", 2);   // duplicate bind is idempotent
    core.pump(a, now, 300);
    CHECK_EQ(core.count(MsgType::HostBindAck), 2);
    CHECK(a.targetHandle() != 0);
    CHECK_EQ(game.health.size(), std::size_t(1));
    a.stop();
}

TEST_CASE("adapter: absolute HP mirrored once; combat events never subtract; old revisions ignored")
{
    ScriptedCore core;
    CHECK(core.ep.start());
    FakeGame game;
    ata::AdapterCore a(adapterCfg(core.ep.port()), game);
    a.start();
    std::uint64_t now = 0;
    core.pump(a, now, 600);
    a.onFrame(now += 16, key(true, false, false));
    core.pump(a, now, 200);
    core.bind("player-1", "player", 1);
    core.bind("target-1", "creature", 2);
    core.pump(a, now, 200);
    int h = a.targetHandle();
    core.state(2, 1, 100, true);
    core.pump(a, now, 150);
    core.damageEvent("ev-1", 30);           // presentation event arrives first
    core.state(2, 2, 70, true);             // authoritative state
    core.damageEvent("ev-1", 30);           // duplicate event
    core.state(2, 1, 100, true);            // late/reordered older revision
    core.pump(a, now, 300);
    CHECK_EQ(game.health[h], 70.0f);        // exactly the core value: no double subtraction, no rollback
    game.gtaHazard(h, 25.0f);               // a native GTA hazard leaking through
    core.pump(a, now, 100);
    CHECK_EQ(game.health[h], 70.0f);        // re-asserted absolute state wins
    a.stop();
}

TEST_CASE("adapter: death presented once; stale alive after death ignored")
{
    ScriptedCore core;
    CHECK(core.ep.start());
    FakeGame game;
    ata::AdapterCore a(adapterCfg(core.ep.port()), game);
    a.start();
    std::uint64_t now = 0;
    core.pump(a, now, 600);
    a.onFrame(now += 16, key(true, false, false));
    core.pump(a, now, 200);
    core.bind("player-1", "player", 1);
    core.bind("target-1", "creature", 2);
    core.pump(a, now, 200);
    core.state(2, 1, 10, true);
    core.state(2, 2, 0, false);
    core.state(2, 3, 0, false);             // full snapshot repeating the death
    core.state(2, 4, 50, true);             // must not resurrect within the same generation
    core.pump(a, now, 300);
    CHECK_EQ(a.deathPresentations(), 1);
    CHECK_EQ(game.health[a.targetHandle()], 0.0f);
    a.stop();
}

TEST_CASE("adapter: cast uses acked host generation; rebind invalidates old ped")
{
    ScriptedCore core;
    CHECK(core.ep.start());
    FakeGame game;
    ata::AdapterCore a(adapterCfg(core.ep.port()), game);
    a.start();
    std::uint64_t now = 0;
    core.pump(a, now, 600);
    a.onFrame(now += 16, key(true, false, false));
    core.pump(a, now, 200);
    core.bind("player-1", "player", 1);
    core.bind("target-1", "creature", 2);
    core.pump(a, now, 200);
    int first = a.targetHandle();
    a.onFrame(now += 16, key(false, false, true));
    core.pump(a, now, 200);
    CHECK_EQ(core.count(MsgType::CastRequest), 1);
    std::int64_t g1 = -1;
    for (auto const& e : core.received)
        if (e.type == MsgType::CastRequest)
        {
            g1 = e.payload.find("host_generation")->asInt();
            CHECK_EQ(e.payload.find("spell_id")->asInt(), 133);
            CHECK_EQ(e.payload.find("caster_id")->asString(), std::string("player-1"));
        }
    CHECK(g1 > 0);
    core.bind("target-1", "creature", 3);   // F8-style reset on core: new incarnation
    core.pump(a, now, 200);
    CHECK(a.targetHandle() != first);
    CHECK(!game.pedExists(first));
    core.state(2, 9, 1, true);              // late state for the old incarnation
    core.pump(a, now, 150);
    CHECK(game.health[a.targetHandle()] == 100.0f);
    a.stop();
}

TEST_CASE("adapter: menu pause resets fixture; resume re-enters; disconnect freezes")
{
    ScriptedCore core;
    CHECK(core.ep.start());
    FakeGame game;
    ata::AdapterCore a(adapterCfg(core.ep.port()), game);
    a.start();
    std::uint64_t now = 0;
    core.pump(a, now, 600);
    a.onFrame(now += 16, key(true, false, false));
    core.pump(a, now, 200);
    core.bind("player-1", "player", 1);
    core.bind("target-1", "creature", 2);
    core.pump(a, now, 200);
    a.onFrame(now += 16, key(false, false, true, true));   // menu opened; key ignored while paused
    core.pump(a, now, 0);
    a.onFrame(now += 16, key(false, false, false, true));
    std::this_thread::sleep_for(100ms);
    CHECK(game.health.empty());                             // fixture ped released
    a.onFrame(now += 16, key(false, false, false, false)); // resume
    core.pump(a, now, 300);
    CHECK_EQ(core.count(MsgType::SetPaused), 2);
    CHECK_EQ(core.count(MsgType::EnterTest), 2);
    CHECK_EQ(core.count(MsgType::CastRequest), 0);
    core.bind("target-1", "creature", 4);
    core.pump(a, now, 200);
    CHECK_EQ(game.health.size(), std::size_t(1));
    core.ep.stop();                                         // core goes away
    std::uint64_t t = now;
    for (int i = 0; i < 60 && a.connected(); ++i)
    {
        a.onFrame(t += 16, ata::FrameInput {});
        std::this_thread::sleep_for(50ms);
    }
    a.onFrame(t += 16, ata::FrameInput {});
    CHECK(!a.connected());
    CHECK(game.health.empty());                             // nothing shown as synchronised
    a.onFrame(t += 16, key(true, false, false));            // F9 off restores player proofs
    CHECK(!game.playerProofs);
    a.stop();
}
