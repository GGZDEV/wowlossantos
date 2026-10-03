# M1 GTA manual checklist (PENDING — needs a Windows PC with GTA SA)

Record each line as PASS / FAIL with the log excerpt. Core must already pass `docs/evidence/m0-m1-report.md` §3.

## Preparation

1. Record `gta_sa.exe` size + SHA-256 and confirm it is **1.0 US** (Plugin-SDK events use 1.0 US
   addresses). Other versions: stop, unsupported.
2. Record repo commit, `deps.lock.json` SHAs, and the fixture (character Gtabridge, entry 69, spell 133).
3. Install Ultimate ASI Loader v9.7.4 x86 only if no loader exists; never overwrite existing DLLs.
4. Copy `AzerothTheftAuto.asi` + `AzerothTheftAuto.ini` to the loader plugin dir; token via
   `ATA_BRIDGE_TOKEN` or `AzerothTheftAuto.token`.
5. Core reachable on 127.0.0.1:17635 from the GTA PC (same machine; Windows build of the core, or
   WSL with port forwarding — note which).

## Checks

| # | Action | Expected | Result |
|---|---|---|---|
| 1 | Start GTA with core **offline** | game starts normally; log "connecting"; no freeze | |
| 2 | Start core, load a save | log `WELCOME epoch …` | |
| 3 | Walk to Grove Street (≈2495, −1670) and press **F9** | ENTER_TEST; overlay "TEST ACTIVE"; one ped appears ~14 m west of CJ; log `bound target-1 gen N -> ped handle H` on both sides | |
| 4 | Press **1** | adapter log `CAST_REQUEST cast-n`; core log `HandleCastSpellOpcode … target hp 55/55`, `prepare accepted`, `cast executed`, `state rev r: target-1 hp X/55`; overlay `target hp (core): X`; ped `m_fHealth == X` (exactly once, no extra drop on COMBAT_EVENT) | |
| 5 | Press **1** twice quickly | second → `SPELL_FAILED_SPELL_IN_PROGRESS`; no HP change | |
| 6 | Turn CJ away, press 1 | `SPELL_FAILED_UNIT_NOT_INFRONT`; no HP change | |
| 7 | Cast until death | one death animation (`TASK_DIE`), log "death presented once"; no respawn from later snapshots | |
| 8 | **F8** | old ped deleted, new ped (new generation), HP 55 | |
| 9 | Shoot/punch/burn/run over the ped and CJ | no HP change on either (proofs), mirrored HP unchanged | |
| 10 | Open the pause menu, then resume | SET_PAUSED, ped removed; on resume fresh ENTER_TEST, new ped | |
| 11 | Stop the core while in test mode | overlay DISCONNECTED, ped removed; restart core → new epoch, F8 rebinding works | |
| 12 | Load a different save / new game | log "game restart/load: bridge bindings discarded"; no crash; F9 works again | |
| 13 | **F9** off | ped removed, CJ proofs restored (CJ takes normal damage again), keys 1/F8 inert | |
| 14 | Calibrate | walk 10 m in GTA, compare core position in log → adjust `Projection.Scale/YawDegrees/GtaOrigin`; confirm range failure distance ≈ 35 yd | |

Attach: adapter log, core `Server.log` excerpt with the same request ids, the core HP before/after and the ped HP observed.
