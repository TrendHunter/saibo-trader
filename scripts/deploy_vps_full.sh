#!/bin/bash
# Production full deploy on VPS — matches bare-metal layout at /opt/polymarket-bot.
#
# On server (after git pull or manual copy):
#   cd /opt/polymarket-bot && bash scripts/deploy_vps_full.sh
#
# From laptop (recommended):
#   python scripts/deploy_production.py
#
# Env overrides:
#   POLYMARKET_ROOT=/opt/polymarket-bot
#   WEB_MODE=full|fast|skip     (default: skip — bot-only; use full|fast or START_WEB=true)
#   SKIP_BUILD=1                skip C++ rebuild
#   SKIP_GIT=1                  skip git pull
#   SKIP_BOT=1 / SKIP_WEB=1
# Manual dashboard: bash server_start_web.sh  or  bash scripts/start_web.sh
set -euo pipefail

ROOT="${POLYMARKET_ROOT:-/opt/polymarket-bot}"
WEB_MODE="${WEB_MODE:-skip}"
SKIP_BUILD="${SKIP_BUILD:-0}"
SKIP_GIT="${SKIP_GIT:-0}"
SKIP_BOT="${SKIP_BOT:-0}"
SKIP_WEB="${SKIP_WEB:-0}"

log() { echo "[deploy] $*"; }

if [ ! -d "$ROOT" ]; then
  echo "ERROR: $ROOT not found — run: python scripts/deploy_production.py --setup" >&2
  exit 1
fi

cd "$ROOT"
mkdir -p logs

if [ -f .env ]; then
  # shellcheck disable=SC1091
  set -a && source .env && set +a
fi
case "$(echo "${START_WEB:-false}" | tr '[:upper:]' '[:lower:]')" in
  true|1|yes|on) START_WEB_ON=1 ;;
  *) START_WEB_ON=0 ;;
esac
if [ "$START_WEB_ON" = "1" ] && [ "$SKIP_WEB" != "1" ] && [ "$WEB_MODE" = "skip" ]; then
  WEB_MODE=fast
  log "START_WEB=true — web step enabled (WEB_MODE=fast)"
fi

if [ "$SKIP_GIT" != "1" ]; then
  if [ -d .git ]; then
    log "git pull origin main..."
    git fetch origin main
    git pull --ff-only origin main
  else
    log "WARN: no .git — skipping pull (upload-only deploy?)"
  fi
fi

if [ ! -d .venv ]; then
  log "creating Python venv..."
  python3 -m venv .venv
fi
log "pip install -r requirements.txt..."
.venv/bin/pip install -q -r requirements.txt

if [ "$SKIP_BUILD" != "1" ]; then
  log "C++ build (build-lowmem.sh)..."
  bash build-lowmem.sh
else
  log "SKIP_BUILD=1 — using existing build/trading-core"
  test -x build/trading-core || { echo "ERROR: build/trading-core missing" >&2; exit 1; }
fi

if [ "$SKIP_BOT" != "1" ]; then
  log "restart bot..."
  chmod +x server_start_bot.sh 2>/dev/null || true
  bash server_start_bot.sh
  sleep 3
  pgrep -af 'start_bot|trading-core' || { tail -30 logs/bridge.log; exit 1; }
fi

if [ "$SKIP_WEB" = "1" ]; then
  log "SKIP_WEB=1 — bot-only (start dashboard: bash server_start_web.sh)"
  exit 0
fi

if [ "$WEB_MODE" = "skip" ]; then
  log "WEB_MODE=skip — bot-only (start dashboard: bash server_start_web.sh)"
  exit 0
fi

if [ ! -f web.env ]; then
  if [ -f web.env.example ]; then
    log "WARN: web.env missing — copy web.env.example and set AUTH_* / NEXTAUTH_URL"
  else
    log "WARN: web.env missing — Web skipped"
  fi
  exit 0
fi

# shellcheck disable=SC1091
set -a && source web.env && set +a
PUBLIC_URL="${NEXTAUTH_URL:-http://127.0.0.1:${PORT:-3001}}"

chmod +x scripts/web_run.sh scripts/web_watchdog.sh scripts/web_install_watchdog.sh 2>/dev/null || true
chmod +x server_start_web.sh server_restart_web.sh 2>/dev/null || true

case "$WEB_MODE" in
  full)
    log "Web full build (server_start_web.sh) NEXTAUTH_URL=$PUBLIC_URL"
    export NEXTAUTH_URL="$PUBLIC_URL"
    bash server_start_web.sh
    ;;
  fast)
    log "Web fast restart (server_restart_web.sh)..."
    export NEXTAUTH_URL="$PUBLIC_URL"
    bash server_restart_web.sh
    bash scripts/web_install_watchdog.sh 2>/dev/null || true
    ;;
  *)
    echo "ERROR: unknown WEB_MODE=$WEB_MODE (use full|fast|skip)" >&2
    exit 1
    ;;
esac

log "open ports:"
ss -tlnp 2>/dev/null | grep -E ':3001|:8080|:8081' || true
log "health:"
curl -sf -o /dev/null -w "bot_api=%{http_code}\n" http://127.0.0.1:8081/health 2>/dev/null || echo "bot_api=down"
curl -sf -o /dev/null -w "web=%{http_code}\n" "http://127.0.0.1:${PORT:-3001}/login" 2>/dev/null || echo "web=down"
log "done — open ${PUBLIC_URL}/login"
