# Architecture contract

## Components

| Component | Role | Build/process |
|---|---|---|
| GTA adapter | Input, ped mapping, movement, animation, mirrored state | x86 ASI in GTA SA |
| Protocol library | Framing, validation, message definitions | standalone x86/x64 builds |
| mod-gamebridge | Commands, server-side session adapter, native entity ownership, events | compiled into x64 worldserver |
| AzerothCore | Actual spell/aura/stat/combat rules, DB persistence | worldserver plus normal DB/data prerequisites |
| bridge-cli | Fake host, diagnostic traces, transport integration tests | standalone console |

The module is the initial server endpoint. There is no extra combat executable and no second reimplementation of WoW rules. Authserver availability for ordinary setup is separate from the local bridge session mechanism; whether it is needed by that mechanism must be determined in M0.

## Thread boundaries

GTA IO worker parses and enqueues messages. GTA's main game update validates/re-resolves ped handles and applies state. Input events enqueue intents. DllMain must not perform heavy initialization, create game objects or wait for a network connection.

Core IO worker parses bounded commands and enqueues them. A verified world/map scheduling path resolves and accesses Player/Creature objects in their owning execution context. Native spell execution occurs there. The same context records results/state into an outbound queue. A WorldScript update hook alone is not automatically safe for every map object when parallel map updates exist; inspect the pinned implementation.

No thread retains an engine pointer across despawn, map transfer or logout. Use IDs and resolve at the point of use. Audit shutdown ordering and prevent callbacks from touching a destroyed session.

## Gameplay flow

1. GTA requests a spell on a bridge target.
2. Module validates connection, epoch, controlled player, entity generation and whitelisted ability.
3. Native spell path checks the real Player/Creature state.
4. Core performs cast/effects and changes its authoritative health/power.
5. Module publishes accepted/rejected cast status, combat presentation events and absolute state.
6. GTA applies the newest state revision once; events only play effects/logs.

A cast accepted at request time may later interrupt, miss, resist or cause no damage. CAST_STATUS must not promise a damage number. Health snapshots are the truth, including overheal, absorption and death ordering.

## Repository ownership

Keep upstream clones separate. Own adapter/module/protocol code and narrow patches here. Record upstream licenses and preserve notices. Dependency source pins and data fixture provenance are required for reproducibility. Do not package game executables, client archives or extracted assets in this repository.

The module can eventually expose capability flags and alternate standard/CoA session adapters. Do not implement an abstract universal engine framework before the concrete proof works.
