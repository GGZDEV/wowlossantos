# Native-core integration tests

These need a running worldserver with mod-gamebridge (real DB + client data); they are not part of `ctest`.

- `scripts/m0-scenario.sh [trace.jsonl]` — native cast, duplicate/mismatch, in-progress, GCD, not learned,
  unknown target, facing, range, kill, dead target, pause/resume, disconnect/reconnect, resync.
- Mana exhaustion and core restart runs: commands in `docs/evidence/m0-m1-report.md` §3.

Results and traces: `docs/evidence/`.
