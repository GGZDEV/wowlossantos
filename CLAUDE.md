# Azeroth Theft Auto — master engineering instructions

## Product intent

Build a local single-player proof of concept where **a real AzerothCore 3.3.5a worldserver owns RPG rules and GTA San Andreas renders/interacts with corresponding entities**. The long-term product supports character creation with race/class, spells, progression and eventually an audited Conquest of Azeroth provider. Do not reinterpret this as GTA-inspired combat formulas or as a WoW client overlay.

This repository currently contains specifications only. Start with source inspection and implementation; never report the starter as a working mod.

## Required reading

Read README.md, docs/architecture.md, docs/feasibility.md, docs/milestones.md, docs/protocol.md, docs/world-and-authority.md, docs/setup.md, docs/acceptance.md and docs/sources.md. Read docs/coa.md before proposing that provider. Read applicable instructions inside any cloned dependency before editing it.

## First objective

Milestone 0 proves that a server-side real Player with a valid session, database state and map can cast a native learned spell at a real Creature **without a connected WoW client during execution**. Milestone 1 connects that result to one dedicated GTA test ped. This is an engineering feasibility gate, not an already solved integration.

The initial bridge can load one prepared character. Rich character creation is a later milestone. If local WoW client use is needed solely to prepare a test character, document it; it must not remain required while the GTA bridge runs.

## Authority rules

- AzerothCore owns health, death, power, spell validation, cast/channel timing, GCD, cooldowns, auras, combat damage, progression and persistence.
- GTA owns visible geometry, local movement/collisions and rendering. The first proxy map only represents a bounded open arena, not all Los Santos.
- GTA sends intents and movement observations. It cannot submit authoritative damage, health, XP, mana or loot.
- Apply **absolute health state**, not damage deltas. Combat events are for animation/log display. They must never damage an entity a second time.
- Every core object belongs to a valid map/session lifecycle. No dangling Player or naked Creature detached from a map.
- No pointers cross the protocol. Core GUIDs are strings; bridge entity IDs are session scoped; GTA handles include a generation.
- For M1 no native damage is allowed to mutate the mirrored test entities independently. Scope interception to bridge entities. Validate death, ragdoll and re-entry behavior.

## Implementation constraints

1. Pin source SHAs before claiming reproducibility. null in deps.lock.json means unresolved, not latest certified stable.
2. Prefer a core module. If hooks cannot cover the required session, event or damage lifecycle, implement a narrow reviewable patch and document each changed symbol. Do not pretend the module API exposes everything.
3. Use the native core build/toolchain standard. Do not force a C++ standard incompatible with its pinned revision. Protocol and plugin code may use C++17 where supported.
4. GTA adapter is x86. The core can be x64. Serialize fixed-width values; never share native object layout, pointers, size_t or STL objects across the boundary.
5. No socket/DB blocking on the GTA game thread. No RE/GTA API usage on the IO thread. Core object work runs in the owning world/map execution context, not the socket reader thread.
6. Use bounded queues. Telemetry positions can coalesce; casts, deaths and despawns cannot silently disappear. Overflow freezes the bridge and initiates a fresh snapshot.
7. Bootstrap with loopback TCP plus bounded framed JSON. Shared memory is future optimization after evidence of a bottleneck.
8. Validate type, size, enum, finite numbers, authorization, identity, lifecycle and version. Restrict server to loopback and use a per-install local secret stored outside git.
9. Disconnect/pause/loading reset invalidates pending requests. Reconnection requires a new session epoch and full snapshot. Never replay stale cast or death events.
10. Native spell request path must retain resource, cooldown, target, range and casting checks. A forced/triggered cast that bypasses them is not the accepted player-cast implementation.
11. The core retains its native timer cadence. Do not bolt on an independent 20 Hz combat simulator. Proposed movement/state publication rates are bridge rates only.
12. Use SDK/source symbols validated against the actual binary. No invented absolute offsets or speculative function signatures.

## Scope and working order

Audit/build → headless native-core proof → IPC fake-host tests → GTA plugin loading → real spell reflected in GTA. Compile and validate after each meaningful step. A fake endpoint is useful for transport tests but is never gameplay proof.

Keep CJ and an existing ped model initially. Do not import assets, implement new races/classes, build a custom map, integrate CoA, multiplayer, quests, vehicles, the police system, a talent UI or a renderer before M1. None of those is needed for the proof.

Use spell and creature IDs confirmed from the installed core data. Prefer one simple known direct-damage spell with clear range and resource cost. Record the actual IDs, character level, learned-spell state and database fixture. Do not assume Shred or another ability works outside its native prerequisites.

## Suggested source tree to implement

```text
CMakeLists.txt                     # standalone protocol + harness, not a build of GTA/core together
protocol/include/gamebridge/
protocol/src/
modules/mod-gamebridge/
  src/                            # SessionAdapter, CommandQueue, EntityRegistry, StatePublisher, Transport
  conf/
  sql/                            # separately documented migrations/fixtures
  README.md
adapters/gta-sa/
  src/                            # Plugin, InputAdapter, PedRegistry, HealthMirror, WorldAdapter, Transport
  conf/
tools/bridge-cli/
tests/protocol/
tests/integration/
scripts/
patches/azerothcore/               # only if audit proves module hooks insufficient
vendor/                           # ignored clones; lock exact revisions
docs/evidence/
```

Build the module through AzerothCore's actual module integration. Build the ASI separately with the verified Plugin-SDK workflow for Win32. Do not manufacture fake CMake dependencies that merely compile empty stubs.

## Completion report

Give exact build commands, dependency SHAs, binaries and install paths, database/data prerequisites, startup order, keys, log files, tests executed and limitations. Include native spell cast evidence, core before/after HP and matching GTA HP. If you cannot run GTA, label its acceptance test pending and provide a short reproducible checklist. Stop after verified M1 before expanding scope.
