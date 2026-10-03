#!/usr/bin/env bash
# Local worldserver helper for headless (no terminal) runs.
#   scripts/worldserver-ctl.sh start      start worldserver; console fed through a FIFO
#   scripts/worldserver-ctl.sh cmd "..."  send a console command (e.g. "server info")
#   scripts/worldserver-ctl.sh stop       "server shutdown 1" and wait for exit
#   scripts/worldserver-ctl.sh wait-ready wait until the world is initialised
# Environment: ATA_SERVER_DIR (default local/azeroth-server), ATA_LOG (default local/logs/worldserver.out)
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
server="${ATA_SERVER_DIR:-$root/local/azeroth-server}"
fifo="$root/local/ws.stdin"
out="${ATA_LOG:-$root/local/logs/worldserver.out}"
pidfile="$root/local/worldserver.pid"

case "${1:-}" in
  start)
    # MySQL is often not auto-started (WSL, containers).
    if ! mysqladmin ping >/dev/null 2>&1 && ! sudo -n mysqladmin ping >/dev/null 2>&1; then
      echo "starting mysql..."
      $([ "$EUID" -ne 0 ] && echo sudo) service mysql start >/dev/null
      for _ in $(seq 1 30); do mysqladmin ping >/dev/null 2>&1 || sudo -n mysqladmin ping >/dev/null 2>&1 && break; sleep 1; done
    fi
    if [[ -z "${ATA_BRIDGE_TOKEN:-}" && -s "$root/local/bridge.token" ]]; then
      export ATA_BRIDGE_TOKEN="$(cat "$root/local/bridge.token")"
    fi
    mkdir -p "$(dirname "$out")"
    [[ -p "$fifo" ]] || mkfifo "$fifo"
    if [[ -f "$pidfile" ]] && kill -0 "$(cat "$pidfile")" 2>/dev/null; then
      echo "worldserver already running (pid $(cat "$pidfile"))"; exit 0
    fi
    # Keep a writer open on the FIFO so the console never sees EOF.
    nohup bash -c "exec 3<>'$fifo'; cd '$server/bin' && exec ./worldserver -c '$server/etc/worldserver.conf' <&3" > "$out" 2>&1 &
    echo $! > "$pidfile"
    echo "worldserver started (pid $!), output: $out"
    ;;
  cmd)
    shift
    printf '%s\n' "$*" > "$fifo"
    ;;
  wait-ready)
    for _ in $(seq 1 600); do
      if grep -qi "World initialized" "$out" 2>/dev/null; then echo ready; exit 0; fi
      if [[ -f "$pidfile" ]] && ! kill -0 "$(cat "$pidfile")" 2>/dev/null; then echo "worldserver exited"; tail -30 "$out"; exit 1; fi
      sleep 1
    done
    echo "timeout"; exit 1
    ;;
  stop)
    if [[ -f "$pidfile" ]] && kill -0 "$(cat "$pidfile")" 2>/dev/null; then
      printf 'server shutdown 1\n' > "$fifo"
      for _ in $(seq 1 120); do kill -0 "$(cat "$pidfile")" 2>/dev/null || break; sleep 1; done
      if kill -0 "$(cat "$pidfile")" 2>/dev/null; then echo "still running"; exit 1; fi
    fi
    rm -f "$pidfile"
    echo stopped
    ;;
  *)
    echo "usage: $0 start|cmd <text>|wait-ready|stop" >&2; exit 2;;
esac
