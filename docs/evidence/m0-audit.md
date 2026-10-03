# M0 audit — AzerothCore headless session and native cast path

Pinned core: `azerothcore-wotlk` **`950684036946011c3d597382b3cfb5de99f7adbc`** (master, 2026-10-02).
All references below are `path:line` at that revision (`vendor/azerothcore/src/server/…`).
Audit date: 2026-10-02/03. Upstream `AGENTS.md`/`CLAUDE.md` read before building (they ask not
to build unless asked; building is the explicit task here; no core file was edited).

## Decision

**Path 1 — module-managed headless session, no core patch.** `patches/azerothcore/` is not needed.

The pinned revision contains upstream headless-session support, commit
`92fed92eaa34` *"feat(Core): support headless sessions and add session-less APIs (#27533)"*
(2026-09-23, playerbots lineage). It makes `WorldSession` safe with a null socket and exposes
the login pieces a module needs. Everything else is reached through existing public handlers
and script hooks.

## Findings

| Topic | Source | Finding / consequence |
|---|---|---|
| Session ctor | `game/Server/WorldSession.cpp:110` | `_headless(!sock)`; no `account.online` writes for headless sessions; address `"headless"`. |
| `IsHeadless()` | `game/Server/WorldSession.h:1234` | Public; used by our packet hook to filter. |
| Packet output | `game/Server/WorldSession.cpp:308-312` | `SendPacket` calls `sScriptMgr->OnPacketSent` **then** returns if `!m_Socket`: nothing is queued, so no unbounded packet accumulation; module observes results through `ServerScript::OnPacketSent`. |
| Session update | `game/Server/WorldSession.cpp:385` | Packets are only read `while (m_Socket && …)`; with a `WorldSessionFilter` a socket-less session returns `false`. |
| Session manager | `game/Server/WorldSessionMgr.cpp:93` | `UpdateSessions` deletes sessions whose `Update` returns false → a headless session **must not** be registered there. The module owns it. |
| Map update | `game/Maps/Map.cpp:450` | Each in-world player's session is updated by its map with `MapSessionFilter` (query callbacks processed there; unsafe branch skipped). |
| Login holder | `game/Server/WorldSession.h:293` | `LoginQueryHolder` is public since `92fed92e`. |
| Login | `game/Handlers/CharacterHandler.cpp:796` | `HandlePlayerLoginFromDB` (public) creates the `Player`, `LoadFromDB` (checks account ownership), `ObjectAccessor::AddObject`, `Map::AddPlayerToMap`, marks online. Same path as a client login. |
| Char create | `game/Handlers/CharacterHandler.cpp:264` | `HandleCharCreateOpcode` (public) validates name/race/class/realm rules and saves; async parts complete through session query/transaction callbacks. |
| Async callbacks | `WorldSession::ProcessQueryCallbacks` (public since `92fed92e`) | Before the player is on a map nobody updates our session → module calls it from the world thread. |
| Account | `game/Accounts/AccountMgr.cpp:43` | `CreateAccount` inserts with `LoginDatabase.Execute` (async) → id not visible on the same tick. Module polls (found during integration). |
| Cast | `game/Handlers/SpellHandler.cpp:378` | `HandleCastSpellOpcode` (public): mover check, spell queue (`CanExecutePendingSpellCastRequest`/`CanRequestSpellCast`), `HasActiveSpell` spellbook check, `new Spell(..., TRIGGERED_NONE)`, `prepare`. Fed a real CMSG_CAST_SPELL payload; nothing triggered or forced. |
| Spell checks | `game/Spells/Spell.cpp:3556` | `prepare` → `CheckCast(true)` (power, cooldown/GCD, range, LOS, facing, target state) → `SendCastResult` → `SMSG_CAST_FAILED` layout `uint8 castCount, uint32 spellId, uint8 result` (`Spell.cpp:4704`). |
| Result hooks | `Spell.cpp:3804` (`OnSpellPrepare`), `Spell.cpp:4208` (`OnSpellCast`, after all `_cast` failure exits) | Correlated by the `m_cast_count` we put in the packet. Interrupt = `SMSG_CAST_FAILED` after acceptance, or `OnSpellCastCancel`. |
| Damage log | `game/Entities/Unit/Unit.cpp:6487` | `SMSG_SPELLNONMELEEDAMAGELOG` layout parsed for presentation only (periodic ticks are not in this packet; they still show in absolute HP). |
| Damage filter | `game/Entities/Unit/Unit.cpp:992` | `UnitScript::DealDamage` is dispatched to every UnitScript; used for the scoped arena isolation policy. |
| Execution context | `game/World/World.cpp:1355`, `game/Maps/MapMgr.cpp:280` | `MapMgr::Update` waits for map threads before `OnWorldUpdate`; module does all object work there (same phase as CLI commands). Hooks may run on map threads and only enqueue. Verified at runtime: prepare on the world thread, cast completion/damage on two different map threads (`MapUpdate.Threads = 2`). |
| Movement | `game/Maps/Map.h:206` | `Map::PlayerRelocation` is the primitive used by the movement handler; used for projected POSITION samples inside the arena. |
| Shutdown | `apps/worldserver/Main.cpp:426` | `OnShutdown` runs after the world loop, before session kick and `MapMgr::UnloadAll` (deleters at function exit) → module logs out/saves there. |
| Maps/data | client-data v20.0 | Terrain height, LOS and mmaps are the core's own; GM Island (map 1) chosen as an isolated, open proxy arena. |

## Known gaps (not blockers for M0)

- `WorldSession::KickPlayer` on the headless session would set `_kicked` but nothing logs it out until shutdown; not reachable from the bridge.
- The headless player is visible to real clients near GM Island (normal player object). Acceptable for local single-player use.
- `IsolateEntities` damage filter is implemented but was not exercised (no third-party attacker in the arena).
- Windows x64 core build not performed (Linux x86_64 build used for evidence).

## Build and data used

```bash
git clone https://github.com/azerothcore/azerothcore-wotlk.git vendor/azerothcore   # @950684036946
apt-get install cmake clang make libmysqlclient-dev libssl-dev libbz2-dev libreadline-dev \
  libncurses-dev libboost-all-dev google-perftools mysql-server            # Ubuntu 24.04
mysql < vendor/azerothcore/data/sql/create/create_mysql.sql
cmake -S vendor/azerothcore -B build/core -DCMAKE_INSTALL_PREFIX=$PWD/local/azeroth-server \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSCRIPTS=static -DMODULES=static -DTOOLS_BUILD=none
make -C build/core -j4 && make -C build/core install
# client data: https://github.com/wowgaming/client-data/releases/download/v20.0/data.zip
#   sha256 a3d4df635ae6c2c8f08052c32a79e0f806955150ad36b014a823dd08a32a4610 -> local/data
# worldserver.conf: DataDir=local/data, LogsDir=local/logs/core, MapUpdate.Threads=2
```

First boot with `Updates.AutoSetup = 1` populated the three databases; world initialised in 14 s.
