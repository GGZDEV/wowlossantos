#!/usr/bin/env bash
# Incremental rebuild of worldserver (after module edits) and install of a debug-stripped copy.
# Usage: scripts/rebuild-worldserver.sh [build-dir] [install-dir]
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="${1:-$root/build/core}"
install="${2:-$root/local/azeroth-server}"
make -C "$build" -j"$(nproc)" worldserver
cp "$build/src/server/apps/worldserver" "$install/bin/worldserver"
strip --strip-debug "$install/bin/worldserver"   # unstripped binary stays in the build tree for gdb
echo "installed $install/bin/worldserver"
