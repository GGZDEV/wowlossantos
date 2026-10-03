// bridge-cli: diagnostic fake host for the GameBridge protocol.
//
// It speaks to a running mod-gamebridge exactly like the GTA adapter does and
// writes a JSONL trace. It is a transport/diagnostic tool, never gameplay proof
// by itself: gameplay evidence comes from the worldserver's native results it
// reports (CAST_STATUS / STATE_SNAPSHOT from the core).
//
// Usage:
//   ATA_BRIDGE_TOKEN=... bridge-cli [--port 17635] [--trace file.jsonl] [--origin x,y,z] --steps "<step;step;...>"
// Steps:
//   enter                 ENTER_TEST at --origin (simulated CJ position, GTA units), then ack ENTITY_BINDs
//   wait:<ms>             process incoming messages for <ms>
//   cast:<spell>          CAST_REQUEST player -> target with a new request id
//   dup                   resend the previous CAST_REQUEST (same id, same payload)
//   mismatch              resend the previous request id with a different spell
//   badtarget:<spell>     CAST_REQUEST at an unknown entity id
//   pause / resume        SET_PAUSED true/false
//   resync                RESYNC
//   lost                  HOST_ENTITY_LOST for the target (streaming loss)
//   move:<dx>,<dy>[,<h>]  POSITION sample: player at bind position + (dx,dy) host units, GTA heading h (rad)
//   disconnect            drop the connection (the client reconnects with a new epoch)
//   expect_hp_drop        exit non-zero unless target hp decreased since enter
#include "gamebridge/endpoint.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace gamebridge;

namespace {

std::ofstream g_trace;

long long nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

void trace(std::string const& dir, std::string const& line)
{
    std::string out = "{\"t\":" + std::to_string(nowMs()) + ",\"dir\":\"" + dir + "\",\"msg\":" + line + "}";
    std::printf("%s\n", out.c_str());
    std::fflush(stdout);
    if (g_trace.is_open())
    {
        g_trace << out << "\n";
        g_trace.flush();
    }
}

void note(std::string const& text)
{
    trace("note", Json::string(text).dump());
}

struct State
{
    std::string playerId;
    std::string targetId;
    std::map<std::string, std::int64_t> generation;  // entity -> core generation
    std::map<std::string, std::int64_t> hostGeneration; // entity -> host generation we acked
    std::map<std::string, Json> bindPos;              // entity -> host_position
    std::map<std::string, std::int64_t> hp;
    std::int64_t initialTargetHp = -1;
    std::string lastCastId;
    Json lastCastPayload;
    std::string fixture = "m1-arena";
    RevisionGate revisions;
    EventDedupe events;
    std::uint64_t hostGen = 0;
    std::int64_t sample = 0;
};

void handle(HostClient& host, State& st, HostClient::Inbound const& in)
{
    if (in.kind == HostClient::Inbound::Kind::Connected)
    {
        trace("in", in.env.toJson().dump());
        st.playerId = in.env.payload.find("player_entity_id")->asString();
        st.fixture = in.env.payload.find("data_fixture")->asString();
        st.revisions.reset();
        st.events.reset();
        st.generation.clear();
        st.hostGeneration.clear();
        st.initialTargetHp = -1;
        return;
    }
    if (in.kind == HostClient::Inbound::Kind::Disconnected)
    {
        note("disconnected: " + in.reason + " (epoch " + in.epoch + ")");
        return;
    }
    Envelope const& e = in.env;
    trace("in", e.toJson().dump());
    switch (e.type)
    {
        case MsgType::EntityBind:
        {
            std::string id = e.payload.find("entity_id")->asString();
            st.generation[id] = e.payload.find("generation")->asInt();
            if (Json const* hp = e.payload.find("host_position"))
                st.bindPos[id] = *hp;
            if (e.payload.find("kind")->asString() == "creature")
                st.targetId = id;
            Json ack = Json::object();
            ack.set("entity_id", Json::string(id));
            st.hostGeneration[id] = static_cast<std::int64_t>(++st.hostGen);
            ack.set("host_generation", Json::integer(st.hostGeneration[id]));
            host.send(MsgType::HostBindAck, ack);
            break;
        }
        case MsgType::StateSnapshot:
            for (Json const& ent : e.payload.find("entities")->items())
            {
                std::string id = ent.find("entity_id")->asString();
                if (!st.revisions.accept(id, ent.find("generation")->asInt(), ent.find("revision")->asInt()))
                    continue;
                st.hp[id] = ent.find("hp")->asInt();
                if (id == st.targetId && st.initialTargetHp < 0)
                    st.initialTargetHp = st.hp[id];
            }
            break;
        case MsgType::CombatEvent:
            if (!st.events.firstTime(e.payload.find("event_id")->asString()))
                note("duplicate combat event ignored");
            break;
        case MsgType::EntityDespawn:
            st.generation.erase(e.payload.find("entity_id")->asString());
            break;
        default:
            break;
    }
}

void pump(HostClient& host, State& st, int ms)
{
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    do
    {
        HostClient::Inbound in;
        while (host.pollInbound(in))
            handle(host, st, in);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (std::chrono::steady_clock::now() < end);
}

Json castPayload(State const& st, std::uint32_t spell, std::string const& target)
{
    Json p = Json::object();
    p.set("caster_id", Json::string(st.playerId));
    p.set("target_id", Json::string(target));
    p.set("spell_id", Json::integer(spell));
    // The host generation we acknowledged for the target binding; a recycled or
    // re-bound target gets a new one, so stale requests are rejected by the core.
    auto it = st.hostGeneration.find(target);
    p.set("host_generation", Json::integer(it == st.hostGeneration.end() ? 0 : it->second));
    return p;
}

std::vector<std::string> split(std::string const& s, char sep)
{
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, sep))
        if (!item.empty())
            out.push_back(item);
    return out;
}

} // namespace

int main(int argc, char** argv)
{
    std::uint16_t port = kDefaultPort;
    std::string steps = "enter;wait:3000";
    std::string tracePath;
    double origin[3] = { 2495.0, -1670.0, 13.3 };   // matches the evidence GtaOrigin
    char const* tokenEnv = "ATA_BRIDGE_TOKEN";
    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "--port" && i + 1 < argc)
            port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
        else if (a == "--steps" && i + 1 < argc)
            steps = argv[++i];
        else if (a == "--trace" && i + 1 < argc)
            tracePath = argv[++i];
        else if (a == "--origin" && i + 1 < argc)
        {
            auto parts = split(argv[++i], ',');
            if (parts.size() != 3)
                return 2;
            for (int k = 0; k < 3; ++k)
                origin[k] = std::atof(parts[k].c_str());
        }
        else if (a == "--token-env" && i + 1 < argc)
            tokenEnv = argv[++i];
        else
        {
            std::fprintf(stderr, "usage: bridge-cli [--port N] [--trace file] [--origin x,y,z] [--token-env VAR] --steps \"enter;wait:2000;cast:133;...\"\n");
            return 2;
        }
    }
    char const* token = std::getenv(tokenEnv);
    if (!token || std::strlen(token) < 16)
    {
        std::fprintf(stderr, "bridge token missing: set %s (>=16 chars, same value as GameBridge.Token)\n", tokenEnv);
        return 2;
    }
    if (!tracePath.empty())
        g_trace.open(tracePath, std::ios::app);

    HostClient::Config cfg;
    cfg.port = port;
    cfg.token = token;
    cfg.adapter = "bridge-cli";
    HostClient host(cfg, [](std::string const& s) { note(s); });
    host.start();

    State st;
    auto waitConnected = [&](int ms) {
        auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (!host.connected() && std::chrono::steady_clock::now() < end)
            pump(host, st, 20);
        pump(host, st, 50);
        return host.connected();
    };
    if (!waitConnected(10000))
    {
        note("could not connect/handshake with core");
        return 1;
    }

    int rc = 0;
    for (std::string const& step : split(steps, ';'))
    {
        std::string cmd = step, arg;
        std::size_t colon = step.find(':');
        if (colon != std::string::npos)
        {
            cmd = step.substr(0, colon);
            arg = step.substr(colon + 1);
        }
        note("step " + step);
        if (!host.connected() && cmd != "wait")
            waitConnected(10000);

        if (cmd == "wait")
            pump(host, st, std::atoi(arg.c_str()));
        else if (cmd == "enter")
        {
            Json p = Json::object();
            p.set("test_fixture_id", Json::string(st.fixture));
            Json pos = Json::object();
            pos.set("x", Json::number(origin[0]));
            pos.set("y", Json::number(origin[1]));
            pos.set("z", Json::number(origin[2]));
            p.set("observed_player_position", pos);
            host.send(MsgType::EnterTest, p, host.nextRequestId("enter"));
            pump(host, st, 1500);
        }
        else if (cmd == "cast" || cmd == "badtarget")
        {
            std::uint32_t spell = static_cast<std::uint32_t>(std::strtoul(arg.c_str(), nullptr, 10));
            st.lastCastId = host.nextRequestId("cast");
            st.lastCastPayload = castPayload(st, spell, cmd == "cast" ? st.targetId : std::string("target-404"));
            trace("out", "{\"type\":\"CAST_REQUEST\",\"request_id\":\"" + st.lastCastId + "\",\"payload\":" + st.lastCastPayload.dump() + "}");
            host.send(MsgType::CastRequest, st.lastCastPayload, st.lastCastId);
            pump(host, st, 100);
        }
        else if (cmd == "dup" || cmd == "mismatch")
        {
            Json p = st.lastCastPayload;
            if (cmd == "mismatch")
                p.set("spell_id", Json::integer(p.find("spell_id")->asInt() + 1));
            trace("out", "{\"type\":\"CAST_REQUEST\",\"request_id\":\"" + st.lastCastId + "\",\"payload\":" + p.dump() + "}");
            host.send(MsgType::CastRequest, p, st.lastCastId);
            pump(host, st, 100);
        }
        else if (cmd == "pause" || cmd == "resume")
        {
            Json p = Json::object();
            p.set("paused", Json::boolean(cmd == "pause"));
            p.set("reason", Json::string("bridge-cli"));
            host.send(MsgType::SetPaused, p, host.nextRequestId("pause"));
            pump(host, st, 200);
        }
        else if (cmd == "resync")
        {
            Json p = Json::object();
            p.set("last_revision", Json::integer(0));
            p.set("reason", Json::string("bridge-cli"));
            host.send(MsgType::Resync, p);
            pump(host, st, 200);
        }
        else if (cmd == "lost")
        {
            Json p = Json::object();
            p.set("entity_id", Json::string(st.targetId));
            p.set("generation", Json::integer(st.generation.count(st.targetId) ? st.generation[st.targetId] : 0));
            p.set("reason", Json::string("streamed_out"));
            host.send(MsgType::HostEntityLost, p);
            pump(host, st, 200);
        }
        else if (cmd == "move")
        {
            auto parts = split(arg, ',');
            if ((parts.size() != 2 && parts.size() != 3) || !st.bindPos.count(st.playerId))
            {
                note("move needs dx,dy and a bound player");
                continue;
            }
            Json const& b = st.bindPos[st.playerId];
            Json pos = Json::object();
            pos.set("x", Json::number(b.find("x")->asDouble() + std::atof(parts[0].c_str())));
            pos.set("y", Json::number(b.find("y")->asDouble() + std::atof(parts[1].c_str())));
            pos.set("z", Json::number(b.find("z")->asDouble()));
            Json p = Json::object();
            p.set("entity_id", Json::string(st.playerId));
            p.set("generation", Json::integer(st.generation[st.playerId]));
            p.set("sample_id", Json::integer(++st.sample));
            p.set("position", pos);
            p.set("orientation", Json::number(parts.size() == 3 ? std::atof(parts[2].c_str()) : 0.0));
            host.send(MsgType::Position, p, std::string(), "pos:" + st.playerId);
            pump(host, st, 100);
        }
        else if (cmd == "disconnect")
        {
            host.stop();
            pump(host, st, 100);
            host.start();
        }
        else if (cmd == "expect_hp_drop")
        {
            std::int64_t now = st.hp.count(st.targetId) ? st.hp[st.targetId] : -1;
            note("target hp initial=" + std::to_string(st.initialTargetHp) + " now=" + std::to_string(now));
            if (!(st.initialTargetHp > 0 && now >= 0 && now < st.initialTargetHp))
                rc = 1;
        }
        else
        {
            note("unknown step " + step);
            rc = 2;
        }
    }
    host.stop();
    return rc;
}
