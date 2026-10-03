# GameBridge protocol v1 — initial design

This is the proposed application protocol, not WoW's wire protocol. Implement and pin it together with the first endpoints.

## Transport/framing

Local TCP server on 127.0.0.1:17635, one active host connection. Never bind 0.0.0.0. Each frame is a 4-byte unsigned big-endian length N, followed by N UTF-8 JSON bytes. 1 <= N <= 65536. Reject zero, oversize, invalid UTF-8, malformed/duplicate-key JSON, excessive nesting (>16), missing fields and non-finite numbers. Read/write loops must handle partial IO and multiple frames per read. A peer stalling midway through a frame expires after a configurable 2 seconds of inactivity.

Use fixed-width wire numbers. Core GUIDs use opaque string encodings. Revisions/counters are safe integers <= 2^53-1 and reset on a fresh epoch before rollover. Length framing is independent of pointer size or ABI.

## Envelope

Fields: version=1, type, epoch, seq, request_id (string or null), payload (object). seq is strictly increasing per sender per epoch. Epoch is assigned by the core on WELCOME; initial HELLO uses the empty string. The first HELLO seq is 0; each sender continues its sequence in the accepted epoch. HELLO never carries gameplay commands.

Heartbeat PING every 1s, PONG deadline 3s. Local bridge token is passed only during HELLO and never written to logs. Reject an unsupported version before allocating game entities. WELCOME returns epoch, core SHA, data fixture identifier, capabilities and permitted character ID. The host cannot select arbitrary accounts/characters without server-side authorization.

## Messages

| Type | Direction | Essential payload |
|---|---|---|
| HELLO | host→core | adapter, version, token |
| WELCOME | core→host | fresh epoch, capabilities, core_sha, character_id |
| ENTER_TEST | host→core | test_fixture_id, observed_player_position |
| ENTITY_BIND | core→host | entity_id, kind, core_guid, template_tag |
| HOST_BIND_ACK | host→core | entity_id, host_generation |
| POSITION | host→core | entity_id, generation, sample_id, position, orientation |
| CAST_REQUEST | host→core | caster_id, target_id, spell_id, host_generation |
| CAST_STATUS | core→host | accepted/rejected/interrupted/completed, reason, spell_id |
| STATE_SNAPSHOT | core→host | full flag, revision, entities with absolute state |
| COMBAT_EVENT | core→host | event_id, event kind, source/target, amount, school, critical |
| HOST_ENTITY_LOST | host→core | entity_id, generation, reason |
| ENTITY_DESPAWN | core→host | entity_id, final revision, reason |
| SET_PAUSED | host→core | paused, reason |
| PAUSE_ACK | core→host | paused, final revision |
| RESYNC | host→core | last_revision, reason |
| RESET | either | reason |
| PING / PONG | either | nonce |
| ERROR | core→host | code, related request_id, safe description |

Requests requiring a response carry request_id; responses echo it. Client can't submit caster identity different from its controlled character. Spell IDs and fixtures are restricted by configuration for the first test.

## Request deduplication

Within epoch retain a bounded cache of completed/in-progress gameplay request IDs and their payload hashes/results. Repeating the same ID/payload must not cast twice. Repeating with a different payload is rejected. Expired/evicted IDs are not reusable: store a bounded replay window based on monotonic seq and reject older requests. Transport reconnection creates a new epoch and drops all pending casts rather than retrying them.

## State versus events

STATE_SNAPSHOT contains entity_id, generation, revision, hp, max_hp, alive, resource type/current/max, cast state and position where applicable. It is authoritative. Cast/tick damage events never independently modify GTA health. Ignore duplicate event_id and older state revisions. On core death set the reflected entity to its final state and invoke the adapter's verified death presentation once.

Startup and reconnect require a full snapshot before input is enabled. Every entity update carries a revision so equal/older state cannot roll back a newer one. Full snapshots at 1Hz and changed state at at most 10Hz are initial publication targets; deaths/cast failures can be immediate. Position samples can be sent at 20Hz; these are not combat simulation ticks.

## Reset/disconnect

At socket loss invalidate the epoch and freeze dedicated test entities/input. Do not continue showing gameplay as synchronized. Clear pending effects, retain no raw handles, and require a new binding plus full snapshot on reconnect. Disabling test mode restores/releases host entities as defined by adapter ownership; it does not replay queued damage into normal GTA play.

Malformed frames, duplicate messages, stale epochs and state-revision regressions are transport tests. Native spell mechanics require separate core integration tests.

## v1 decisions fixed during implementation (2026-10)

Implemented in `protocol/` (C++17, no dependencies) and used by both endpoints.

- **Request IDs.** Deduplicated gameplay requests (CAST_REQUEST) use `"<prefix>-<counter>"`, counter a
  decimal safe integer strictly increasing per epoch for *new* requests; a retry reuses the ID.
  The core keeps the last 256 IDs with payload hashes (canonical JSON, FNV-1a). Same ID + same payload →
  cached response re-sent, never re-executed; same ID + different payload → ERROR `request_payload_mismatch`;
  an evicted or lower counter → ERROR `request_replay`. Request counters restart at 1 in a new epoch.
- **Generations.** ENTITY_BIND carries the core `generation` (incarnation). HOST_BIND_ACK carries the
  host's `host_generation` for that binding; CAST_REQUEST must echo the acknowledged `host_generation`
  of its target, otherwise `stale_target_generation`. GTA ped handles are script handles embedding
  the pool slot generation.
- **Positions.** ENTITY_BIND includes `position` (core) and `host_position` (projected for the host).
  POSITION is in host units; the core projects it (`Projection.*`), enforces the arena radius and a
  max speed; a jump or leaving the arena ends test mode (ENTITY_DESPAWN).
- **WELCOME** payload: `core_sha`, `capabilities`, `character_id`, `player_entity_id`, `data_fixture`,
  optional `spell_ids` (strings). WELCOME is always the first core message of an epoch; heartbeats
  start after it.
- **CAST_STATUS**: `accepted` when `Spell::prepare` succeeded (or the spell executed), `completed` when
  `Spell::cast` ran, `rejected` with the native `SpellCastResult` name (e.g. `SPELL_FAILED_OUT_OF_RANGE`,
  plus `native_result` code) or a bridge reason (`unknown_target`, `spell_not_permitted`,
  `native_handler_ignored`, …), `interrupted` after acceptance. No damage number is promised.
- **STATE_SNAPSHOT entity fields**: `entity_id, generation, revision, hp, max_hp, alive` plus
  `power_type, power, max_power, casting_spell_id`. Hosts apply an entity state only if
  (generation, revision) is newer than what they applied.
- **COMBAT_EVENT kinds**: `damage, heal, death, cast_start, cast_go, cast_failed` (presentation only).
- **Pause**: capability `pause_as_reset` — SET_PAUSED true despawns the fixture (ENTITY_DESPAWN) and
  answers PAUSE_ACK; the host sends a fresh ENTER_TEST on resume.
- **Payload schemas** are exact (unknown fields rejected); see `protocol/src/protocol.cpp`.
