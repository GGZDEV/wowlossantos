// Azeroth Theft Auto - GTA San Andreas ASI entry point (Plugin-SDK).
//
// One source, two builds:
//   - GTASA          : classic GTA SA 1.0 US, Win32 .asi
//   - GTASA_UNREAL   : GTA SA The Definitive Edition, x64 .asi (Plugin-SDK "unreal" target;
//                      functions located by binary patterns, not fixed addresses)
//
// Engine access goes through the game's own script interpreter (Plugin-SDK
// Command<...>), which exists identically in both versions: no hand-written offsets.
// The static object only registers Plugin-SDK events (DllMain context). Config,
// log and network client are created lazily on the first gameProcessEvent, and all
// game calls happen on the game thread inside that event.
#include "plugin.h"

#include "AdapterCore.h"

#include "CPools.h"
#include "extensions/Paths.h"
#include "extensions/ScriptCommands.h"
#ifdef GTASA
#include "CMenuManager.h"
#include "CPed.h"
#include "common.h"
#include "extensions/FontPrint.h"
#endif

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>

using namespace plugin;

namespace {

#ifdef GTASA_UNREAL
constexpr char const* kBuild = "GTA SA Definitive Edition (x64)";
#else
constexpr char const* kBuild = "GTA SA 1.0 US (x86)";
#endif

// ---------------------------------------------------------------- logging

class FileLog
{
public:
    void open()
    {
        std::lock_guard<std::mutex> g(_mx);
        if (_f)
            return;
        std::string path = std::string(paths::GetPluginDirPathA()) + "AzerothTheftAuto.log";
        _f = std::fopen(path.c_str(), "a");
        if (!_f)
        {
            char tmp[MAX_PATH] = { 0 };
            GetTempPathA(MAX_PATH, tmp);
            path = std::string(tmp) + "AzerothTheftAuto.log";
            _f = std::fopen(path.c_str(), "a");
        }
        _path = path;
    }
    void write(std::string const& line)
    {
        std::lock_guard<std::mutex> g(_mx);
        if (!_f)
            return;
        SYSTEMTIME st;
        GetLocalTime(&st);
        std::fprintf(_f, "%02d:%02d:%02d.%03d %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, line.c_str());
        std::fflush(_f);
    }
    std::string path() const { return _path; }

private:
    std::mutex _mx;
    FILE* _f = nullptr;
    std::string _path;
};

FileLog g_log;

// ---------------------------------------------------------------- config

struct IniValues
{
    std::map<std::string, std::string> kv;
    std::string get(std::string const& k, std::string const& def) const
    {
        auto it = kv.find(k);
        return it == kv.end() ? def : it->second;
    }
};

std::string trim(std::string s)
{
    while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\n' || s.back() == '\t'))
        s.pop_back();
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.erase(s.begin());
    return s;
}

IniValues readIni(std::string const& path)
{
    IniValues v;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line))
    {
        if (line.empty() || line[0] == ';' || line[0] == '#' || line[0] == '[')
            continue;
        auto eq = line.find('=');
        if (eq != std::string::npos)
            v.kv[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    return v;
}

std::string readTokenFile(std::string const& path)
{
    std::ifstream in(path);
    std::string t;
    std::getline(in, t);
    return trim(t);
}

// ---------------------------------------------------------------- engine seam (script commands)

int playerHandle()
{
    if (!Command<Commands::IS_PLAYER_PLAYING>(0))   // no player ped yet (menus/loading/wasted)
        return 0;
    int h = 0;
    Command<Commands::GET_PLAYER_CHAR>(0, &h);
    return h;
}

class ScriptGameApi : public ata::GameApi
{
public:
    bool playerPosition(ata::Vec3& pos, float& headingRad) override
    {
        int h = playerHandle();
        if (!h || !Command<Commands::DOES_CHAR_EXIST>(h))
            return false;
        float x = 0, y = 0, z = 0, deg = 0;
        Command<Commands::GET_CHAR_COORDINATES>(h, &x, &y, &z);
        Command<Commands::GET_CHAR_HEADING>(h, &deg);   // script headings are degrees, 0 = north, CCW
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || !std::isfinite(deg))
            return false;
        pos = { x, y, z };
        headingRad = deg * 0.0174532925f;
        return true;
    }

    int spawnTargetPed(ata::Vec3 const& pos, float headingRad, int modelId) override
    {
        Command<Commands::REQUEST_MODEL>(modelId);
        Command<Commands::LOAD_ALL_MODELS_NOW>();
        if (!Command<Commands::HAS_MODEL_LOADED>(modelId))
            return 0;
        float groundZ = pos.z;
        Command<Commands::GET_GROUND_Z_FOR_3D_COORD>(pos.x, pos.y, pos.z + 10.0f, &groundZ);
        int handle = 0;
        Command<Commands::CREATE_CHAR>(4 /* PED_TYPE_CIVMALE */, modelId, pos.x, pos.y, groundZ, &handle);
        Command<Commands::MARK_MODEL_AS_NO_LONGER_NEEDED>(modelId);
        if (!handle || !Command<Commands::DOES_CHAR_EXIST>(handle))
            return 0;
        Command<Commands::SET_CHAR_HEADING>(handle, headingRad * 57.2957795f);
        Command<Commands::SET_CHAR_STAY_IN_SAME_PLACE>(handle, 1);   // stationary fixture
        return handle;
    }

    bool pedExists(int handle) override
    {
        // Script handles embed the pool slot generation; DOES_CHAR_EXIST rejects recycled slots.
        return handle && Command<Commands::DOES_CHAR_EXIST>(handle);
    }

    void deletePed(int handle) override
    {
        if (pedExists(handle))
            Command<Commands::DELETE_CHAR>(handle);
    }

    void setPedHealth(int handle, float hp, float maxHp) override
    {
        if (!pedExists(handle))
            return;
        Command<Commands::SET_CHAR_MAX_HEALTH>(handle, static_cast<int>(maxHp));
        Command<Commands::SET_CHAR_HEALTH>(handle, static_cast<int>(hp));
    }

    float pedHealth(int handle) override
    {
        if (!pedExists(handle))
            return -1.0f;
        int hp = -1;
        Command<Commands::GET_CHAR_HEALTH>(handle, &hp);
        return static_cast<float>(hp);
    }

    void presentDeath(int handle) override
    {
        if (pedExists(handle))
            Command<Commands::TASK_DIE>(handle);
    }

    void setPedProofs(int handle, bool on) override
    {
        if (pedExists(handle))
        {
            int v = on ? 1 : 0;
            Command<Commands::SET_CHAR_PROOFS>(handle, v, v, v, v, v);   // bullet fire explosion collision melee
        }
    }

    void setPlayerProofs(bool on) override
    {
        int h = playerHandle();
        if (!h)
            return;
        if (on && !_active)
        {
#ifdef GTASA
            // Classic: remember CJ's current proofs (CPhysical bits, VALIDATE_OFFSET'd in the SDK).
            if (CPed* p = CPools::GetPed(h))
                _saved = { p->bBulletProof, p->bFireProof, p->bExplosionProof, p->bCollisionProof, p->bMeleeProof };
#endif
            Command<Commands::SET_CHAR_PROOFS>(h, 1, 1, 1, 1, 1);
            _active = true;
        }
        else if (!on && _active)
        {
            // DE: the SDK exposes no CPed fields, so CJ's proofs return to the game default (off).
            Command<Commands::SET_CHAR_PROOFS>(h, int(_saved.bullet), int(_saved.fire), int(_saved.explosion),
                                               int(_saved.collision), int(_saved.melee));
            _active = false;
        }
    }

    void forgetPlayerProofs() { _active = false; _saved = {}; }

    void log(std::string const& line) override { g_log.write(line); }

private:
    struct Proofs
    {
        bool bullet = false, fire = false, explosion = false, collision = false, melee = false;
    };
    Proofs _saved {};
    bool _active = false;
};

// ---------------------------------------------------------------- plugin

class AzerothTheftAuto
{
public:
    AzerothTheftAuto()
    {
        // DllMain context: register callbacks only.
        Events::gameProcessEvent += [this] { process(); };
#ifdef GTASA
        Events::drawingEvent += [this] { draw(); };
        Events::restartGameEvent += [this] { restart("restartGameEvent"); };
#endif
        Events::shutdownRwEvent += [this] {
            if (_core)
                _core->stop();
        };
    }

private:
    void init()
    {
        _initDone = true;
        g_log.open();
        IniValues ini = readIni(std::string(paths::GetPluginDirPathA()) + "AzerothTheftAuto.ini");
        ata::AdapterConfig cfg;
        cfg.port = static_cast<std::uint16_t>(std::atoi(ini.get("Port", "17635").c_str()));
        cfg.fixtureId = ini.get("FixtureId", "m1-arena");
        cfg.targetModelId = std::atoi(ini.get("TargetModel", "7").c_str());
        _keyToggle = static_cast<unsigned>(std::strtoul(ini.get("KeyToggle", "0x78").c_str(), nullptr, 0));   // VK_F9
        _keyReset = static_cast<unsigned>(std::strtoul(ini.get("KeyReset", "0x77").c_str(), nullptr, 0));     // VK_F8
        _keyCast = static_cast<unsigned>(std::strtoul(ini.get("KeyCast", "0x31").c_str(), nullptr, 0));       // '1'
        if (char const* env = std::getenv("ATA_BRIDGE_TOKEN"))
            cfg.token = trim(env);
        else
            cfg.token = readTokenFile(std::string(paths::GetPluginDirPathA()) + ini.get("TokenFile", "AzerothTheftAuto.token"));
        g_log.write(std::string("Azeroth Theft Auto adapter loaded for ") + kBuild + "; log at " + g_log.path());
        if (cfg.token.size() < 16)
        {
            g_log.write("no bridge token (ATA_BRIDGE_TOKEN or token file); adapter stays offline, game unaffected");
            return;
        }
        _core = std::make_unique<ata::AdapterCore>(cfg, _game);
        _core->start();
    }

    void restart(char const* why)
    {
        _game.forgetPlayerProofs();
        if (_core)
        {
            g_log.write(std::string("game restart detected (") + why + ")");
            _core->onGameRestart();
        }
    }

    bool edge(unsigned key, bool& prev)
    {
        bool down = KeyPressed(key);
        bool fired = down && !prev;   // ignore auto-repeat
        prev = down;
        return fired;
    }

    void process()
    {
        if (!_initDone)
            init();
        if (!_core)
            return;

        ULONGLONG wall = GetTickCount64();
        int gameTimer = 0;
        Command<Commands::GET_GAME_TIMER>(&gameTimer);
        int player = playerHandle();

#ifdef GTASA_UNREAL
        // DE has no restartGameEvent in the SDK: a new game/load recreates CJ or rewinds the game clock.
        if (_lastPlayer && player && player != _lastPlayer)
            restart("player handle changed");
        else if (_lastGameTimer && gameTimer + 2000 < _lastGameTimer)
            restart("game clock went backwards");
#endif
        _lastPlayer = player;

        // Paused / menus / loading: the game clock stops while frames may continue.
        if (gameTimer != _lastGameTimer)
        {
            _lastGameTimer = gameTimer;
            _lastClockMove = wall;
        }
        bool stalled = wall - _lastClockMove > 300;

        ata::FrameInput in;
        in.menuOrLoading = stalled || !player;
#ifdef GTASA
        in.menuOrLoading = in.menuOrLoading || FrontEndMenuManager.m_bMenuActive;
#endif
        bool t = edge(_keyToggle, _prevToggle), r = edge(_keyReset, _prevReset), c = edge(_keyCast, _prevCast);
        if (!in.menuOrLoading)
        {
            in.toggleTest = t;
            in.resetFixture = r;
            in.cast = c;
        }
        _core->onFrame(static_cast<std::uint64_t>(wall), in);
    }

#ifdef GTASA
    void draw()
    {
        if (!_core || !_core->testMode())
            return;
        gamefont::Print(_core->overlay(), 20.0f, 220.0f, 1.0f, FONT_DEFAULT, 0.6f, 1.2f, CRGBA(255, 255, 255, 255));
    }
#endif

    ScriptGameApi _game;
    std::unique_ptr<ata::AdapterCore> _core;
    bool _initDone = false;
    unsigned _keyToggle = 0x78, _keyReset = 0x77, _keyCast = 0x31;
    bool _prevToggle = false, _prevReset = false, _prevCast = false;
    int _lastPlayer = 0;
    int _lastGameTimer = 0;
    ULONGLONG _lastClockMove = 0;
} g_plugin;

} // namespace
