#!/usr/bin/env bash
# Quick native check without GTA: ENTER_TEST, one Fireball, expect the wolf's HP to drop.
# Prints CAST_STATUS / HP lines; exit code 0 = core side works.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export ATA_BRIDGE_TOKEN="${ATA_BRIDGE_TOKEN:-$(cat "$root/local/bridge.token")}"
out="$("$root/build/protocol-x64/bridge-cli" --steps "enter;wait:1500;cast:133;wait:3500;expect_hp_drop")" && rc=0 || rc=$?
printf '%s\n' "$out" | grep -E 'CAST_STATUS|target hp|"ERROR"|could not' | cut -c1-200
[[ $rc -eq 0 ]] && echo "OK: native cast changed the target HP" || echo "FAILED (rc=$rc)"
exit $rc
