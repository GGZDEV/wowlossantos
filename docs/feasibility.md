# Feasibility gate — do this before broad implementation

AzerothCore is a server application, not a plug-and-play combat DLL. Native rules depend on a database, loaded spell data, world maps and the Player/WorldSession lifecycle. Existing module extensibility is helpful but does not prove support for a GTA client.

## M0 source audit deliverable

Create docs/evidence/m0-audit.md with pinned SHA, exact source file/symbol links, the chosen integration path and build/test results.

Inspect:
- WorldSession constructor, socket assumptions, packet output, Update and logout.
- Character creation and asynchronous DB loading; callbacks and ownership.
- Player registration in map/object accessors, update scheduling, removal and persistence.
- Map validity, grid loading, map/vmap/mmap requirements and terrain queries.
- Normal player spell cast request handling, validation and asynchronous effects.
- Hook coverage for health/power changes, auras, cast outcomes and deaths.
- Sending outgoing updates in a session with no network client; prevent unbounded packet accumulation.

Candidate paths, to investigate rather than assume:
1. Module-managed server-side session using existing supported lifecycle paths.
2. Narrow core patch exposing the necessary lifecycle and result hooks.
3. Existing bot/session implementation as a reference only after matching branch, licensing and lifecycle inspection. Do not automatically add a playerbots fork and change the core baseline.

A dummy socket or a simple `new Player()` is not a proven solution. Do not assume a method called CreateHeadlessPlayer, CastAbility or SpawnBridgeCreature exists. Name your adapter methods however you like, but implement them with source-verified underlying calls.

## Native proof

Load a prepared real character, attach it correctly to a map, create a target Creature, ensure a spell is learned and cast it through a non-bypassing native path. Observe native health and power changes, failed requests, cooldown and logout/save. Repeat with no WoW client connected. A GM/triggered spell can help diagnosis but cannot pass the final player-cast gate.

## World restriction

The first world is a small open test rectangle mapped by a calibrated affine transform into a valid open region of an existing server map. Verify ground, separation and range against both representations. Do not assert that stock Azeroth map/vmap/mmap files describe Los Santos.

If M0 cannot be achieved with the inspected core, keep the findings and a minimal reproducible failure. Explain the smallest remaining patch or external prerequisite. Do not switch silently to fabricated combat rules.
