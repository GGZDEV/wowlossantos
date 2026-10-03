# Local setup and builds

## Prerequisites to inspect

- Windows PC with GTA SA classic installed, executable variant identified; initial target is 1.0 US x86. Definitive Edition, mobile/UWP and arbitrary Steam executables are outside the first supported target.
- Visual Studio C++ tools including Win32/x64 compilers and Windows SDK; toolset must match the pinned Plugin-SDK/core requirements.
- Git and CMake, plus the pinned AzerothCore installation guide's actual database and library versions.
- Local auth/characters/world databases and matching DBC/map/vmap/mmap inputs required by the selected core build. The server source alone is insufficient. Obtain/prepare the local data separately; do not embed assets in this starter.
- Ultimate ASI Loader x86 version compatible with the observed GTA executable. Document selected proxy DLL name only after identifying loader conflicts.

## Clone examples (run at project root)

```powershell
git clone https://github.com/azerothcore/azerothcore-wotlk.git vendor/azerothcore
git clone https://github.com/DK22Pac/plugin-sdk.git vendor/plugin-sdk
```

These commands select the repository's default branch initially. They are not version pins. Immediately record `git rev-parse HEAD` for each checkout, inspect applicable upstream instructions and choose the tested revisions. Update deps.lock.json. Do not download a cracked executable or silently modify the user's GTA install to make a binary match.

## Build separation

1. Standalone protocol/harness: create a real CMake build with meaningful tests.
2. Core: follow its verified installation guide; integrate the module via the current module build mechanism. Record any required narrow patch before building. Build and boot the native core first.
3. GTA: build the adapter as a Win32 DLL with `.asi` output against the pinned Plugin-SDK workflow. Use a separate build directory from the x64 core.

VS CMake generator architecture must not be reused across Win32/x64 build trees. Example directory names: build/protocol-x64, build/protocol-x86, build/gta-win32. Exact core flags/library paths remain audit outputs, not fictional commands in this specification.

## Startup target after implementation

1. Start DB and the required local core services with the bridge module loaded.
2. Confirm module endpoint and character fixture ready via bridge-cli.
3. Start GTA through the validated ASI loader.
4. Enable test mode and bind the fixture; wait for WELCOME and full state.
5. Spawn/reset dedicated target, press key 1, inspect logs and HP.

Expected project outputs after implementation: `AzerothTheftAuto.asi`, standalone bridge-cli, updated worldserver with mod-gamebridge, configs and migrations. This archive does not contain those binaries.

## Installation/reporting

Implement install scripts only after paths are known. Copy just the adapter/config to the documented loader plugin directory, never blindly overwrite existing proxy DLLs or game files. Keep test mode opt-in. Config secrets are local and ignored by git.

Report logs by explicit path: suggested adapter log under the project-selected writable game user-data location, bridge log via native server logging, CLI trace under docs/evidence. Confirm the actual paths on the machine rather than assuming Program Files is writable.
