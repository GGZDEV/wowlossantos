#!/usr/bin/env bash
# Links modules/mod-gamebridge into an AzerothCore checkout's modules/ directory so
# that AzerothCore's own module build picks it up (static modules).
# Usage: scripts/link-module.sh [path/to/azerothcore]   (default: vendor/azerothcore)
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
core="${1:-$root/vendor/azerothcore}"
if [[ ! -f "$core/modules/CMakeLists.txt" ]]; then
  echo "not an AzerothCore checkout: $core" >&2
  exit 1
fi
target="$core/modules/mod-gamebridge"
if [[ -e "$target" && ! -L "$target" ]]; then
  echo "$target exists and is not a symlink; refusing to overwrite" >&2
  exit 1
fi
ln -sfn "$root/modules/mod-gamebridge" "$target"
echo "linked $target -> $root/modules/mod-gamebridge"
