# Acceptance and evidence

## Automated transport/adapter tests

- Split length header, split body, concatenated frames and partial writes.
- Zero/oversize frame, duplicate JSON keys, malformed UTF-8, huge nesting, invalid enum and non-finite position.
- Unsupported version, wrong token, unauthorized player and spell.
- Old epoch after reconnect and late state for recycled ped generation.
- Duplicate CAST_REQUEST accepted once, payload mismatch rejected, replay outside window rejected.
- Reordered/duplicate state revision and duplicate presentation event.
- Queue overflow must freeze/resync, not silently lose casts or keep unbounded memory.
- Peer disappears mid-cast; reconnect snapshot discards pending requests.

These validate protocol/lifecycle only; they do not prove server gameplay.

## Native-core integration

Prepared real Player + real Creature + installed data, without a WoW client connected. Cast native learned spell, observe HP and resource changes. Insufficient resource, cooldown, invalid target and out-of-range requests fail through native checks. Test session teardown/save/reload and core restart without leaked entity/session or outgoing packet queues. Prove callback thread context is safe.

## GTA M1 manual checklist

- Identify GTA executable and record adapter/core SHAs plus data fixture.
- ASI loads, no freeze; core offline permits GTA to start normally.
- F9 enters test mode, F8 creates one dedicated fixture; host/core mapping is logged.
- Press key 1. Log request ID, native spell ID, accepted/rejected outcome, before/after core HP, state revision and reflected GTA HP.
- Native health delta is nonzero on successful damage and matches the mirror; no second subtraction occurs from COMBAT_EVENT.
- Reject invalid/GCD/resource request; no reflected HP loss.
- Kill fixture: one death presentation, no accidental respawn from stale snapshot.
- Pause/loading/despawn/disconnect: no stale damage, crash or event to a reused ped slot.
- Native GTA hazards cannot independently alter the scoped test entities.
- Exit bridge mode: controls and non-bridge gameplay behave normally.

## Evidence bundle

docs/evidence/m0-audit.md, dependency SHAs, build commands/log excerpt, test summary, fixture IDs/calibration, core and adapter trace for the same request ID, short manual result table. Mark each result PASS/FAIL/PENDING. If no Windows/GTA runtime is available, keep manual tests PENDING.

M1 may pass with CJ, stock ped, minimal logs and reset-on-pause. It cannot pass with mocked damage, bypassed spell validation, a disconnected fake Player or only a transport ping.
