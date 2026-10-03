# mod-gamebridge

AzerothCore module that exposes **one** prepared character to a local game host (the GTA SA
adapter or `bridge-cli`) over the GameBridge v1 protocol (loopback TCP, framed JSON).
AzerothCore remains the only rules engine: the module translates host intents into the
core's own handlers and publishes the resulting absolute state.

No core patch is required at the pinned revision (`950684036946`): it relies on upstream
headless-session support (`92fed92e`, 2026-09-23) and public script hooks.

## What it does

| Step | Native path used |
|---|---|
| Account | `AccountMgr::CreateAccount` (only if missing; random unused password) |
| Character | `WorldSession::HandleCharCreateOpcode` with a CMSG_CHAR_CREATE payload (only if missing) |
| Fixture placement | `Player::SavePositionInDB` in a transaction, before loading |
| Session | `WorldSession(..., sock = nullptr, ...)` → `IsHeadless()`; owned by the module, **not** in `WorldSessionMgr` (which deletes socket-less sessions) |
| Login | `LoginQueryHolder` + `WorldSession::HandlePlayerLoginFromDB` (normal login, map add) |
| Async DB callbacks | `WorldSession::ProcessQueryCallbacks` from the world thread until the player is on a map; afterwards `Map::Update` updates the session like any player |
| Target | `Map::SummonCreature` (map-owned TempSummon, manual despawn), `REACT_PASSIVE`, `MoveIdle` |
| Cast | `WorldSession::HandleCastSpellOpcode` with a CMSG_CAST_SPELL payload → spellbook check, native spell queue, `Spell::prepare`/`CheckCast` (power, cooldown, GCD, range, LOS, facing, target) |
| Results | `AllSpellScript` OnSpellPrepare/OnSpellCast/OnSpellCastCancel + `SMSG_CAST_FAILED` and `SMSG_SPELLNONMELEEDAMAGELOG` observed through `ServerScript::OnPacketSent` (called before the null-socket early return in `WorldSession::SendPacket`; nothing is queued for a headless session) |
| State | HP/power/alive/casting read from the Player/Creature each world tick, published with revisions |
| Movement | `Map::PlayerRelocation` inside the arena, from projected POSITION samples |
| Logout | `WorldSession::LogoutPlayer(true)` from `WorldScript::OnShutdown` (world loop stopped, maps not yet unloaded) |

Threading: all game-object work happens in `WorldScript::OnUpdate`, which `World::Update`
calls after `MapMgr::Update` has joined the map threads (same phase as CLI commands). Hooks
on map threads only append compact records to a mutex-protected bounded queue.

M1 arena policy (`GameBridge.Fixture.IsolateEntities = 1`): damage to the bridge player/target
from anything other than the bridge player is zeroed by the `UnitScript::DealDamage` hook and
logged. Pause is implemented as reset (despawn + fresh ENTER_TEST), reported via capability
`pause_as_reset`.

## Build

```bash
scripts/link-module.sh                # symlink into vendor/azerothcore/modules (junction: link-module.ps1)
cmake <core-build-dir>                # re-run CMake so the module is picked up (MODULES=static)
make -j$(nproc) && make install
```

`mod-gamebridge.cmake` adds the protocol sources from `../../protocol` to AzerothCore's
`modules` target (static module build only). Override with `-DGAMEBRIDGE_PROTOCOL_DIR=...`
if the checkout layout differs.

## Configure

Copy `conf/mod_gamebridge.conf.dist` to `<install>/etc/modules/mod_gamebridge.conf`; set
`GameBridge.Enable = 1`, `Fixture.CreatureEntry`, `SpellIds`, `Arena.CoreOrigin`, and provide
the token through `ATA_BRIDGE_TOKEN`. The evidence fixture is in `docs/evidence/m0-m1-report.md`.

Logs: `module.gamebridge` / `module.gamebridge.io` loggers (Server.log and console).
