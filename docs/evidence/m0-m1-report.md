# Milestone 0 / Milestone 1 evidence report

Environment: Linux x86_64 cloud container (Ubuntu 24.04, 4 cores). **No Windows, no GTA SA.**
Everything GTA-in-game is therefore **PENDING** (checklist: `m1-gta-checklist.md`).

Status legend: PASS = executed here and observed; PENDING = not executable here; FAIL = executed and failed.

## 1. Summary

| Stage | Result | Notes |
|---|---|---|
| M0a source/build audit, pinned revisions | **PASS** | `m0-audit.md`, `deps.lock.json` |
| M0a core builds and boots with real data | **PASS** | AC `950684036946`, client-data v20.0, world init 14 s |
| M0b headless native gameplay proof | **PASS** | No WoW client at any point; see §3 |
| M1a local IPC + diagnostic host | **PASS** | 38 protocol + 5 adapter-logic tests, x64 Linux and x86 Windows (wine) |
| M1b ASI loads on identified GTA binary | **PENDING** | ASI compiled (PE32 i386); not loaded in GTA |
| M1c key 1 → core cast → GTA HP mirrored | **PENDING** | Core half proven with `bridge-cli`; GTA half untested |

Separately, as requested:

- **Implemented:** protocol library, `bridge-cli`, `mod-gamebridge`, GTA adapter (`AdapterCore` + Plugin-SDK seam), scripts, docs.
- **Compiled:** worldserver with module (x86_64 Linux, GCC 13); protocol/tests/cli (x64 Linux; x86 Windows mingw); `AzerothTheftAuto.asi` (x86, clang 18 → i686-w64-mingw32, Plugin-SDK `15f15b60`). MSVC build not run.
- **Automated tests passed:** `protocol_tests` 38/38, `adapter_tests` 5/5 (x64 native, and x86 `.exe` under wine 9.0 — wine is not a real Windows).
- **Native core integration passed:** yes (§3), against the real worldserver, real DB, real data.
- **GTA manual test:** pending.

## 2. Fixture (installed data, confirmed in DB)

| Item | Value | Source |
|---|---|---|
| Account | `GTABRIDGE` (id 1), random unused password | created by `AccountMgr::CreateAccount` |
| Character | `Gtabridge`, guid 1, Human (1) Mage (8), level 1, 52 HP, 165 mana | created by native `HandleCharCreateOpcode`; no WoW client used |
| Spells | 133 Fireball r1 (learned, action bar slot 0 in `playercreateinfo_action`), 168 Frost Armor (learned), 116 Frostbolt (whitelisted but **not** learned at level 1, used to prove the spellbook check) | `GameBridge.SpellIds = "133 168 116"` |
| Target | creature entry **69** "Diseased Timber Wolf", level 2, 55 HP (`creature_classlevelstats` level 2 class 1), no AIName/ScriptName, faction 32 | summoned per ENTER_TEST, `REACT_PASSIVE`, `MoveIdle` |
| Arena | map 1, `CoreOrigin = 16226.2 16257.0 13.2022 1.65007` (GM Island, `game_tele` id 424), radius 40 yd | target spawns 15 yd ahead (12.6 yd surface distance) |
| Projection | `GtaOrigin = 2495.0 -1670.0 13.3` (Grove Street, **uncalibrated**), scale 1.0936 yd/unit, yaw −90° | to calibrate in GTA |

## 3. Native core evidence (no WoW client)

Traces: `traces/m0-scenario.jsonl` (host side) and `traces/m0-scenario.core.txt` (core side), same request IDs.
Reproduce: `scripts/m0-scenario.sh` against a running worldserver.

Login: `headless player Gtabridge (0x…01) in world: map 1 (16226.20, 16257.00, 13.20) level 1 hp 52/52 power 165/165`.

| # | Request | Native outcome (CAST_STATUS) | Core state |
|---|---|---|---|
| 1 | `cast-2` Fireball | accepted (`Spell::prepare`) → completed (`Spell::cast`) | mana 165→157; damage log 17 fire; **wolf HP 55→38** (rev 4) |
| 2 | `cast-3` Fireball during cast | rejected `SPELL_FAILED_SPELL_IN_PROGRESS` | no HP change |
| 3 | `cast-3` resent (same id+payload) | cached status re-sent, **not re-executed** | — |
| 4 | `cast-3` with spell 134 | ERROR `request_payload_mismatch` | — |
| 5 | `cast-4` Frost Armor (instant) | accepted → completed | mana 157→133 |
| 6 | `cast-5` Fireball right after | rejected `SPELL_FAILED_NOT_READY` (GCD) | no HP change from cast |
| 7 | `cast-6` Frostbolt (not learned) | rejected `native_handler_ignored` (native `HasActiveSpell` check returned silently) | — |
| 8 | `cast-7` unknown target id | rejected `unknown_target` (bridge validation) | — |
| 9 | `cast-8` facing away (POSITION heading) | rejected `SPELL_FAILED_UNIT_NOT_INFRONT` | — |
| 10 | `cast-9` after walking ~42 yd away | rejected `SPELL_FAILED_OUT_OF_RANGE` | — |
| 11 | `cast-10`, `cast-11` back in range | accepted → completed | 36→19→0 (Fireball DoT ticks visible as −1 steps) |
| 12 | death | COMBAT_EVENT `death` once, snapshot `alive=false` rev 38 | creature dead; XP awarded natively |
| 13 | `cast-12..15` at dead target | rejected `SPELL_FAILED_BAD_TARGETS` | — |
| 14 | pause / resume | ENTITY_DESPAWN ×2, PAUSE_ACK(final rev 50); fresh ENTER_TEST → new generations | new wolf 55/55 |
| 15 | `cast-19` then host disconnect | accepted; disconnect interrupts it, nothing replayed | new epoch on reconnect, re-bind gen 5/6, full snapshot |

Resource check (`traces/m0-mana.jsonl`): 24 Fireballs with fixture resets; mana 8→0, then
`SPELL_FAILED_NO_POWER` ×3; after native regeneration (5-second rule) casts are accepted again.

Persistence: core shutdown → `WorldSession::LogoutPlayer(true)`; DB after first session:
`xp 0→53`, Frost Armor aura row (`spell 168, remainTime 1737148/1800000`). Restart → log
`loaded state: xp 53 auras 7 (frost armor 168: yes)`. Current DB: xp 159, online 0.

Core restart with host connected (`traces/m0-core-restart.jsonl`): host sees `disconnected`,
reconnects with a new epoch, full ENTITY_BIND + snapshot, new cast 55→43. During this test a
race was found (core PING could precede WELCOME, making the host drop the handshake); fixed in
`CoreEndpoint` (heartbeats start after WELCOME) with two regression tests.

Thread context (core log): `Spell::prepare accepted … [hook thread 42957, world thread 42957]`,
`Spell::cast executed … [hook thread 8016 / 39316, world thread 42957]` — completion and damage
hooks run on map threads and only enqueue; all object work stays on the world thread.

Absolute state, not deltas: every STATE_SNAPSHOT carries absolute `hp/max_hp/alive/power` with a
revision; COMBAT_EVENT `damage` is presentation-only. Ticks of Fireball's periodic component
(not in the damage-log packet) still appear in HP because HP is read from the unit.

## 4. Build commands (as run)

```bash
# protocol, tests, bridge-cli (x64)
cmake -S . -B build/protocol-x64 -DCMAKE_BUILD_TYPE=RelWithDebInfo && make -C build/protocol-x64 -j4
ctest --test-dir build/protocol-x64 --output-on-failure
# same, x86 Windows binaries (run with wine or on Windows)
cmake -S . -B build/protocol-x86-win -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-i686-w64-mingw32.cmake && make -C build/protocol-x86-win
# core + module (see m0-audit.md for the core prerequisites)
scripts/link-module.sh && cmake build/core && make -C build/core -j4 && make -C build/core install
scripts/rebuild-worldserver.sh          # incremental module rebuild + stripped install
# GTA adapter: see adapters/gta-sa/README.md
```

## 5. Binaries and paths (this machine)

| Artifact | Path |
|---|---|
| worldserver (+ mod-gamebridge) | `local/azeroth-server/bin/worldserver` (debug-stripped; unstripped in `build/core`) |
| module config | `local/azeroth-server/etc/modules/mod_gamebridge.conf` |
| bridge token (secret, not in git) | `local/bridge.token` → exported as `ATA_BRIDGE_TOKEN` |
| bridge-cli | `build/protocol-x64/bridge-cli`, `build/protocol-x86-win/bridge-cli.exe` |
| GTA adapter | `build/gta-win32/bin/Release/AzerothTheftAuto.asi` (PE32 i386 DLL; imports KERNEL32, msvcrt, USER32, WS2_32) |
| core logs | `local/logs/core/Server.log`, console `local/logs/worldserver.out` |
| adapter log | `AzerothTheftAuto.log` next to the ASI or `%TEMP%` |

## 6. Startup order

1. MySQL running (`acore_auth/characters/world`).
2. `export ATA_BRIDGE_TOKEN=$(cat local/bridge.token)`; `scripts/worldserver-ctl.sh start`; wait for
   `headless player Gtabridge … in world` in `Server.log`. Authserver is **not** needed (no client).
3. Optional check: `bridge-cli --steps "enter;wait:1500;cast:133;wait:3000;expect_hp_drop"`.
4. Start GTA through the ASI loader with the same token; F9, then key 1.

## 7. Limitations

- GTA side untested in game; projection uncalibrated; ped model 7 placeholder.
- Core verified on Linux only; the spec's Windows x64 core build is not done.
- Pause is implemented as reset (capability `pause_as_reset`), per the documented M1 fallback.
- Fireball r1 cost observed as 8 mana and Frost Armor 24 at level 1 (native values, not assumed).
- `IsolateEntities` filter not exercised; `KickPlayer` on the headless session unsupported.
- The headless player is a normal world object (visible to nearby real clients).
- CAST_STATUS `rejected` with reason `native_handler_ignored` means the native handler returned
  without a result packet (spell not in spellbook / passive) — the core gives no error code there.
