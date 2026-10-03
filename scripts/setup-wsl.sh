#!/usr/bin/env bash
# One-shot setup of the AzerothCore side on Ubuntu 24.04 (WSL2 on the GTA PC, or native Linux).
# Reproduces the configuration used for docs/evidence. Idempotent: safe to re-run.
#
#   git clone -b claude/m0-m1-gamebridge https://github.com/ggzdev/wowlossantos.git ~/wowlossantos
#   cd ~/wowlossantos && scripts/setup-wsl.sh
#
# Keep the checkout inside the Linux filesystem (~/...), not under /mnt/c (much slower builds).
# Disk: ~25 GB (build tree ~12 GB, client data ~3 GB). Time: build 20-60 min depending on cores.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"
SUDO=$([ "$EUID" -ne 0 ] && echo sudo || echo "")
AC_SHA=950684036946011c3d597382b3cfb5de99f7adbc
DATA_URL=https://github.com/wowgaming/client-data/releases/download/v20.0/data.zip
DATA_SHA256=a3d4df635ae6c2c8f08052c32a79e0f806955150ad36b014a823dd08a32a4610
JOBS="${JOBS:-$(nproc)}"
step() { printf '\n==== %s\n' "$*"; }

step "1/8 system packages"
$SUDO apt-get update -qq
$SUDO env DEBIAN_FRONTEND=noninteractive apt-get install -y -qq git cmake make clang gcc g++ curl unzip \
  libmysqlclient-dev libssl-dev libbz2-dev libreadline-dev libncurses-dev libboost-all-dev \
  google-perftools mysql-server

step "2/8 AzerothCore @ $AC_SHA"
if [[ ! -d vendor/azerothcore/.git ]]; then
  mkdir -p vendor/azerothcore
  git -C vendor/azerothcore init -q
  git -C vendor/azerothcore remote add origin https://github.com/azerothcore/azerothcore-wotlk.git
fi
if [[ "$(git -C vendor/azerothcore rev-parse HEAD 2>/dev/null || true)" != "$AC_SHA" ]]; then
  git -C vendor/azerothcore fetch -q --depth 1 origin "$AC_SHA"
  git -C vendor/azerothcore checkout -q --detach FETCH_HEAD
fi
scripts/link-module.sh vendor/azerothcore

step "3/8 MySQL"
$SUDO service mysql start >/dev/null || true
for _ in $(seq 1 30); do $SUDO mysqladmin ping >/dev/null 2>&1 && break; sleep 1; done
$SUDO mysql < vendor/azerothcore/data/sql/create/create_mysql.sql

step "4/8 configure + build worldserver with mod-gamebridge (long)"
cmake -S vendor/azerothcore -B build/core -DCMAKE_INSTALL_PREFIX="$root/local/azeroth-server" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSCRIPTS=static -DMODULES=static -DTOOLS_BUILD=none > /dev/null
make -C build/core -j"$JOBS"
make -C build/core install > /dev/null
strip --strip-debug local/azeroth-server/bin/worldserver

step "5/8 client data v20.0"
mkdir -p local/data local/logs/core
if [[ ! -f local/data/data-version ]]; then
  curl -fL --retry 4 "$DATA_URL" -o local/data/data.zip
  echo "$DATA_SHA256  local/data/data.zip" | sha256sum -c -
  unzip -q -o local/data/data.zip -d local/data && rm local/data/data.zip
  echo "INSTALLED_VERSION=v20.0" > local/data/data-version
fi

step "6/8 configuration"
etc=local/azeroth-server/etc
sed -e "s|^DataDir = .*|DataDir = \"$root/local/data\"|" \
    -e "s|^LogsDir = .*|LogsDir = \"$root/local/logs/core\"|" \
    -e 's|^MapUpdate.Threads = 1|MapUpdate.Threads = 2|' "$etc/worldserver.conf.dist" > "$etc/worldserver.conf"
sed -e 's/^GameBridge.Enable = 0/GameBridge.Enable = 1/' \
    -e 's/^GameBridge.Fixture.CreatureEntry = 0/GameBridge.Fixture.CreatureEntry = 69/' \
    -e 's/^GameBridge.SpellIds = ""/GameBridge.SpellIds = "133 168 116"/' \
    -e 's/^GameBridge.Arena.CoreOrigin = ""/GameBridge.Arena.CoreOrigin = "16226.2 16257.0 13.2022 1.65007"/' \
    -e 's/^GameBridge.Projection.GtaOrigin = "0 0 0"/GameBridge.Projection.GtaOrigin = "2495.0 -1670.0 13.3"/' \
    "$etc/modules/mod_gamebridge.conf.dist" > "$etc/modules/mod_gamebridge.conf"

step "7/8 local bridge token (secret, never commit)"
if [[ ! -s local/bridge.token ]]; then
  (umask 077; head -c 24 /dev/urandom | od -An -tx1 | tr -d ' \n' > local/bridge.token)
fi

step "8/8 bridge-cli + protocol tests"
cmake -S . -B build/protocol-x64 -DCMAKE_BUILD_TYPE=RelWithDebInfo > /dev/null
make -C build/protocol-x64 -j"$JOBS" > /dev/null
ctest --test-dir build/protocol-x64 --output-on-failure | tail -3

cat <<EOF

Setup done. Next:
  scripts/worldserver-ctl.sh start && scripts/worldserver-ctl.sh wait-ready
  grep "in world" local/logs/core/Server.log        # headless player ready (first start creates it)
  scripts/check-bridge.sh                            # native cast check without GTA
Token for GTA (copy into AzerothTheftAuto.token next to the .asi):
  $(cat local/bridge.token)
EOF
