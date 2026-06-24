#!/bin/bash
# Stop Next.js dashboard and remove auto-restart cron (bot keeps running).
set -euo pipefail
ROOT="${POLYMARKET_ROOT:-/opt/polymarket-bot}"
MARK="# polymarket-web-watchdog"

log() { echo "[stop-web] $*"; }

log "killing next/node web processes..."
pkill -9 -f "next-server" 2>/dev/null || true
pkill -f "node.*standalone/server.js" 2>/dev/null || true
pkill -f "npm run start" 2>/dev/null || true
sleep 2

if pgrep -af "next-server|standalone/server.js" 2>/dev/null | grep -v pgrep; then
  log "WARN: some web processes may still be running"
else
  log "web processes stopped"
fi

TMP="$(mktemp)"
( crontab -l 2>/dev/null | grep -v "$MARK" || true ) > "$TMP"
crontab "$TMP" 2>/dev/null || true
rm -f "$TMP"
log "removed web watchdog cron (if any)"

log "listening ports:"
ss -tlnp 2>/dev/null | grep -E ':3001|:8080|:8081' || true

log "bot health:"
curl -sf -o /dev/null -w "bot_api=%{http_code}\n" "http://127.0.0.1:8081/health" 2>/dev/null || echo "bot_api=down"

log "done — bot-only; control via logs/runtime_config.json or curl :8081/api/config"
