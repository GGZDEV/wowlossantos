// Azeroth Theft Auto - GTA San Andreas ASI entry point (Plugin-SDK, Win32, GTA SA 1.0 US).
//
// Everything engine-related lives here; AdapterCore holds the bridge logic.
// The static object below only registers Plugin-SDK events: no thread, socket,
// file or game object is created during DllMain. The network client starts on
// Events::initGameEvent and is driven from Events::gameProcessEvent (game thread).
#include "plugin.h"

#include "AdapterCore.h"

#include "CMenuManager.h"
#include "CPed.h"
#include "CPools.h"
#include "CTimer.h"
#include "common.h"
#include "extensions/FontPrint.h"
#include "extensions/Paths.h"
#include "extensions/ScriptCommands.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>

using namespace plugin;

namespace {

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
        if (eq == std::string::npos)
            continue;
        auto trim = [](std::string s) {
            while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\t')) s.pop_back();
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
            return s;
        };
        v.kv[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    return v;
}

std::string readTokenFile(std::string const& path)
{
    std::ifstream in(path);
    std::string t;
    std::getline(in, t);
    while (!t.empty() && (t.back() == '\r' || t.back() == ' '))
        t.pop_back();
    return t;
}

// ---------------------------------------------------------------- engine seam

class SdkGameApi : public ata::GameApi
{
public:
    bool playerPosition(ata::Vec3& pos, float& heading) override
    {
        CPed* p = FindPlayerPed();
        if (!p)
            return false;
        CVector const& v = p->GetPosition();
        pos = { v.x, v.y, v.z };
        heading = p->GetHeading();
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
        if (!handle || !CPools::GetPed(handle))
            return 0;
        Command<Commands::SET_CHAR_HEADING>(handle, headingRad * 57.2957795f);   // script headings are degrees
        Command<Commands::SET_CHAR_STAY_IN_SAME_PLACE>(handle, 1);               // stationary fixture
        return handle;
    }

    bool pedExists(int handle) override
    {
        // CPools::GetPed validates the slot's generation byte embedded in the handle.
        return handle && CPools::GetPed(handle) != nullptr;
    }

    void deletePed(int handle) override
    {
        if (pedExists(handle))
            Command<Commands::DELETE_CHAR>(handle);
    }

    void setPedHealth(int handle, float hp, float maxHp) override
    {
        if (CPed* ped = CPools::GetPed(handle))
        {
            ped->m_fMaxHealth = maxHp;
            ped->m_fHealth = hp;
        }
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
        CPed* p = FindPlayerPed();
        if (!p)
            return;
        if (on && !_playerProofsSaved)
        {
            _saved = { p->bBulletProof, p->bFireProof, p->bExplosionProof,
                       p->bCollisionProof, p->bMeleeProof };
            _playerProofsSaved = true;
            int h = CPools::GetPedRef(p);
            Command<Commands::SET_CHAR_PROOFS>(h, 1, 1, 1, 1, 1);
        }
        else if (!on && _playerProofsSaved)
        {
            int h = CPools::GetPedRef(p);
            Command<Commands::SET_CHAR_PROOFS>(h, int(_saved.bullet), int(_saved.fire), int(_saved.explosion),
                                               int(_saved.collision), int(_saved.melee));
            _playerProofsSaved = false;
        }
    }

    void forgetPlayerProofs() { _playerProofsSaved = false; }   // new game: CJ is a different ped

    void log(std::string const& line) override { g_log.write(line); }

private:
    struct Proofs
    {
        bool bullet, fire, explosion, collision, melee;
    };
    Proofs _saved {};
    bool _playerProofsSaved = false;
};

// ---------------------------------------------------------------- plugin

class AzerothTheftAuto
{
public:
    AzerothTheftAuto()
    {
        // DllMain context: register callbacks only.
        Events::initGameEvent += [this] { init(); };
        Events::gameProcessEvent += [this] { process(); };
        Events::drawingEvent += [this] { draw(); };
        Events::restartGameEvent += [this] {
            _game.forgetPlayerProofs();
            if (_core)
                _core->onGameRestart();
        };
        Events::shutdownRwEvent += [this] {
            if (_core)
                _core->stop();
        };
    }

private:
    void init()
    {
        if (_core)
            return;
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
            cfg.token = env;
        else
            cfg.token = readTokenFile(std::string(paths::GetPluginDirPathA()) + ini.get("TokenFile", "AzerothTheftAuto.token"));
        g_log.write("Azeroth Theft Auto adapter loading; log at " + g_log.path());
        if (cfg.token.size() < 16)
        {
            g_log.write("no bridge token (ATA_BRIDGE_TOKEN or token file); adapter stays offline, game unaffected");
            return;
        }
        _core = std::make_unique<ata::AdapterCore>(cfg, _game);
        _core->start();
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
        if (!_core)
            return;
        ata::FrameInput in;
        in.menuOrLoading = FrontEndMenuManager.m_bMenuActive || CTimer::m_UserPause || CTimer::m_CodePause;
        bool t = edge(_keyToggle, _prevToggle), r = edge(_keyReset, _prevReset), c = edge(_keyCast, _prevCast);
        if (!in.menuOrLoading)
        {
            in.toggleTest = t;
            in.resetFixture = r;
            in.cast = c;
        }
        _core->onFrame(CTimer::m_snTimeInMillisecondsNonClipped, in);
    }

    void draw()
    {
        if (!_core || !_core->testMode())
            return;
        gamefont::Print(_core->overlay(), 20.0f, 220.0f, 1.0f, FONT_DEFAULT, 0.6f, 1.2f, CRGBA(255, 255, 255, 255));
    }

    SdkGameApi _game;
    std::unique_ptr<ata::AdapterCore> _core;
    unsigned _keyToggle = 0x78, _keyReset = 0x77, _keyCast = 0x31;
    bool _prevToggle = false, _prevReset = false, _prevCast = false;
} g_plugin;

} // namespace
