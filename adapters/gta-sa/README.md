# GTA SA adapter (`AzerothTheftAuto.asi`)

ASI plugin built with the pinned [Plugin-SDK](https://github.com/DK22Pac/plugin-sdk) (`15f15b60`) for:

- **GTA San Andreas – The Definitive Edition** (x64, Plugin-SDK `GTASA_UNREAL` target; the SDK locates
  game functions by binary pattern, support is experimental upstream) → `build/gta/bin/DE/Release/`;
- **GTA San Andreas 1.0 US** (x86) → `build/gta/bin/Classic/Release/`.

Prebuilt copies: `dist/gta-sa/de/`, `dist/gta-sa/classic/`. It is the host side of
GameBridge protocol v1: it sends intents (ENTER_TEST, CAST_REQUEST) and movement
observations (POSITION), and mirrors **absolute** state from AzerothCore onto one
dedicated stock ped. It never computes or applies damage itself.

## Layout

| File | Role |
|---|---|
| `src/AdapterCore.{h,cpp}` | Bridge logic (bindings, revisions, death-once, pause-as-reset). No SDK dependency; unit-tested on the host (`tests/adapter`). |
| `src/Plugin.cpp` | Plugin-SDK seam, same source for both games: events, keys, ped spawn/health/proofs/position through the game's script interpreter. Classic adds the text overlay and CJ proof save/restore (DE's SDK exposes no CPed fields and no font helper). |
| `premake5.lua` | Build script for the SDK's own premake workflows (vs2022 or clang/mingw). |
| `conf/AzerothTheftAuto.ini` | Ports, keys, ped model. No secrets. |

Engine access (all from the pinned SDK; no hand-written offsets): `Events::gameProcessEvent`,
`shutdownRwEvent` (both games), `drawingEvent`/`restartGameEvent` (classic), `CPools::GetPed/GetPedRef`,
`KeyPressed`, `paths::GetPluginDirPathA`, and script commands `IS_PLAYER_PLAYING`, `GET_PLAYER_CHAR`,
`GET_CHAR_COORDINATES`, `GET_CHAR_HEADING`, `GET_GAME_TIMER`, `REQUEST_MODEL`, `LOAD_ALL_MODELS_NOW`,
`HAS_MODEL_LOADED`, `GET_GROUND_Z_FOR_3D_COORD`, `CREATE_CHAR`, `MARK_MODEL_AS_NO_LONGER_NEEDED`,
`SET_CHAR_HEADING`, `SET_CHAR_STAY_IN_SAME_PLACE`, `SET_CHAR_PROOFS`, `SET_CHAR_MAX_HEALTH`,
`SET_CHAR_HEALTH`, `GET_CHAR_HEALTH` (mirror read-back logged as `MIRROR …`), `TASK_DIE`,
`DOES_CHAR_EXIST`, `DELETE_CHAR`. Pause/menu/loading = game clock (`GET_GAME_TIMER`) not advancing;
on DE a game restart is detected from a new player handle or the clock going backwards.

Threading: the static plugin object only registers events (DllMain). Config, log and the network
client (`gamebridge::HostClient`, own IO thread, bounded queues) start lazily on the first
`gameProcessEvent`; all game calls happen on the game thread.

## Build

Windows / MSVC (SDK primary workflow, not yet run by us):

```bat
set PLUGIN_SDK_DIR=C:\path\to\azeroth-theft-auto\vendor\plugin-sdk
rem build the SDK once with its own instructions (output\lib\Plugin.lib)
%PLUGIN_SDK_DIR%\tools\premake\premake5.exe --file=adapters\gta-sa\premake5.lua vs2022
msbuild build\gta\AzerothTheftAuto.sln /p:Configuration=Release /p:Platform=DE
```

Linux cross build (verified here, clang 18 + mingw-w64 13):

```bash
export PLUGIN_SDK_DIR=$PWD/vendor/plugin-sdk
(cd $PLUGIN_SDK_DIR/tools/premake && ./linux-clang-example/premake5 --file=premake5.lua gmake2)
make -C $PLUGIN_SDK_DIR/plugin_sa config=release -j4            # -> output/lib/libPlugin.a
$PLUGIN_SDK_DIR/tools/premake/linux-clang-example/premake5 --file=adapters/gta-sa/premake5.lua gmake2
make -C $PLUGIN_SDK_DIR/plugin_sa_unreal config=release -j4    # -> output/lib/libPlugin_Unreal.a (DE)
make -C build/gta config=release_de        # PE32+ x86-64 DLL
make -C build/gta config=release_classic   # PE32 i386 DLL
```

## Install (manual, on the GTA PC)

1. Identify the game: Definitive Edition (`SanAndreas.exe`, x64 → `dist/gta-sa/de`) or classic
   1.0 US (`gta_sa.exe`, x86 → `dist/gta-sa/classic`). Other classic versions are unsupported.
2. Install Ultimate ASI Loader (x64 build `dinput8-x64.zip` for DE, x86 v9.7.4 for classic) only if no
   loader is present; choose its proxy DLL name after checking existing files. Never overwrite DLLs.
3. Copy `AzerothTheftAuto.asi` and `conf/AzerothTheftAuto.ini` next to the game exe (or `scripts\`/`plugins\`).
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
