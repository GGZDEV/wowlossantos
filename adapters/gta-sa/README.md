# GTA SA adapter (`AzerothTheftAuto.asi`)

Win32 ASI plugin for **GTA San Andreas 1.0 US (x86)** built with the pinned
[Plugin-SDK](https://github.com/DK22Pac/plugin-sdk) (`15f15b60`). It is the host side of
GameBridge protocol v1: it sends intents (ENTER_TEST, CAST_REQUEST) and movement
observations (POSITION), and mirrors **absolute** state from AzerothCore onto one
dedicated stock ped. It never computes or applies damage itself.

## Layout

| File | Role |
|---|---|
| `src/AdapterCore.{h,cpp}` | Bridge logic (bindings, revisions, death-once, pause-as-reset). No SDK dependency; unit-tested on the host (`tests/adapter`). |
| `src/Plugin.cpp` | Plugin-SDK seam: events, keys, overlay, ped spawn/health/proofs via GTA script commands. |
| `premake5.lua` | Build script for the SDK's own premake workflows (vs2022 or clang/mingw). |
| `conf/AzerothTheftAuto.ini` | Ports, keys, ped model. No secrets. |

Engine access used (all from the pinned SDK headers, no hand-written offsets):
`Events::initGameEvent/gameProcessEvent/drawingEvent/restartGameEvent/shutdownRwEvent`,
`FindPlayerPed`, `CPlaceable::GetHeading`, `CPools::GetPed/GetPedRef`, `CPed::m_fHealth/m_fMaxHealth`
(`VALIDATE_OFFSET` 0x540/0x544), `CPhysical` proof bits, `FrontEndMenuManager.m_bMenuActive`,
`CTimer::m_UserPause/m_CodePause`, and script commands `REQUEST_MODEL`, `LOAD_ALL_MODELS_NOW`,
`HAS_MODEL_LOADED`, `GET_GROUND_Z_FOR_3D_COORD`, `CREATE_CHAR`, `MARK_MODEL_AS_NO_LONGER_NEEDED`,
`SET_CHAR_HEADING`, `SET_CHAR_STAY_IN_SAME_PLACE`, `SET_CHAR_PROOFS`, `TASK_DIE`, `DELETE_CHAR`.

Threading: the static plugin object only registers events (DllMain). The network client
(`gamebridge::HostClient`, own IO thread, bounded queues) starts on `initGameEvent`; all
game calls happen in `gameProcessEvent`/`drawingEvent` on the game thread.

## Build

Windows / MSVC (SDK primary workflow, not yet run by us):

```bat
set PLUGIN_SDK_DIR=C:\path\to\azeroth-theft-auto\vendor\plugin-sdk
rem build the SDK once with its own instructions (output\lib\Plugin.lib)
%PLUGIN_SDK_DIR%\tools\premake\premake5.exe --file=adapters\gta-sa\premake5.lua vs2022
msbuild build\gta-win32\AzerothTheftAuto.sln /p:Configuration=Release /p:Platform=Win32
```

Linux cross build (verified here, clang 18 + mingw-w64 13):

```bash
export PLUGIN_SDK_DIR=$PWD/vendor/plugin-sdk
(cd $PLUGIN_SDK_DIR/tools/premake && ./linux-clang-example/premake5 --file=premake5.lua gmake2)
make -C $PLUGIN_SDK_DIR/plugin_sa config=release -j4            # -> output/lib/libPlugin.a
$PLUGIN_SDK_DIR/tools/premake/linux-clang-example/premake5 --file=adapters/gta-sa/premake5.lua gmake2
make -C build/gta-win32 config=release
# -> build/gta-win32/bin/Release/AzerothTheftAuto.asi (PE32 i386 DLL)
```

## Install (manual, on the GTA PC)

1. Identify the executable (`gta_sa.exe` 1.0 US; see checklist). Other versions are unsupported:
   Plugin-SDK events use 1.0 US addresses.
2. Install Ultimate ASI Loader v9.7.4 **x86** only if no loader is present; choose its proxy DLL
   name after checking existing files. Never overwrite existing DLLs.
3. Copy `AzerothTheftAuto.asi` and `conf/AzerothTheftAuto.ini` into the loader's plugin directory.
4. Provide the token: `ATA_BRIDGE_TOKEN` env var for the GTA process, or `AzerothTheftAuto.token`
   next to the ASI (first line). Same value as the core's `ATA_BRIDGE_TOKEN`/`GameBridge.Token`.
5. Log: `AzerothTheftAuto.log` next to the ASI, or `%TEMP%\AzerothTheftAuto.log` if not writable.

Without a token, or with the core offline, the adapter stays idle and the game runs normally.

## Controls (test mode only)

- **F9** toggle bridge test mode (sends ENTER_TEST with CJ's position; CJ gets proofs while active).
- **F8** reset the fixture (fresh ENTER_TEST → new target incarnation).
- **1** CAST_REQUEST for the first whitelisted spell (from WELCOME) at the bound target.
- Input is ignored while menus/pause/loading are active; pausing resets the fixture (M1 policy).

See `docs/evidence/m1-gta-checklist.md` for the manual acceptance procedure.
