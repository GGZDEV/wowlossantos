#!/usr/bin/env bash
# M0/M1a evidence scenario: drives the running worldserver (mod-gamebridge) with bridge-cli.
# Requires ATA_BRIDGE_TOKEN and a worldserver started with the same token.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cli="${ATA_BRIDGE_CLI:-$root/build/protocol-x64/bridge-cli}"
trace="${1:-$root/local/logs/m0-scenario.jsonl}"
: > "$trace"
H=1.5708   # GTA heading facing the target (target spawns at core +y == host -x)
away=""; back=""
for d in 5 10 15 20 25; do away+="move:$d,0,$H;wait:400;"; done
for d in 20 15 10 5 0; do back+="move:$d,0,$H;wait:400;"; done
steps="enter;wait:1500;"
steps+="cast:133;cast:133;dup;mismatch;wait:2700;"          # cast in progress, duplicate id, payload mismatch
steps+="cast:168;cast:133;wait:1700;"                        # instant spell then GCD
steps+="cast:116;wait:300;"                                  # whitelisted but not in spellbook
steps+="badtarget:133;wait:300;"                             # bridge validation: unknown entity
steps+="move:0,0,-$H;wait:400;cast:133;wait:700;"            # facing away
steps+="move:0,0,$H;wait:400;${away}cast:133;wait:700;${back}" # out of range, then back
for _ in 1 2 3 4 5; do steps+="cast:133;wait:2800;"; done   # kill the target natively
steps+="expect_hp_drop;cast:133;wait:700;"                   # cast at a dead target
steps+="pause;wait:600;resume;wait:300;enter;wait:1500;cast:133;wait:150;"
steps+="disconnect;wait:2500;enter;wait:1500;resync;wait:800"
exec "$cli" --trace "$trace" --steps "$steps"
