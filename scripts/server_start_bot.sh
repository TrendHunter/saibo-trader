#!/bin/bash
# Detached bot start (survives SSH session close). Does NOT start Next.js dashboard.
set -e
ROOT="${POLYMARKET_ROOT:-/opt/polymarket-bot}"
cd "$ROOT"
# trading-core popen("python3 fetch_balance.py") must hit project venv + deps
export PATH="$(pwd)/.venv/bin:$PATH"
mkdir -p logs

# Low-memory VPS: bot-only on restart; start dashboard manually via server_start_web.sh
if [ -x "$ROOT/scripts/server_stop_web.sh" ]; then
  bash "$ROOT/scripts/server_stop_web.sh" >/dev/null 2>&1 || true
elif [ -x "$ROOT/server_stop_web.sh" ]; then
  bash "$ROOT/server_stop_web.sh" >/dev/null 2>&1 || true
fi

touch logs/STOP_TRADING
pkill -f 'start_bot.py' 2>/dev/null || true
pkill -f 'dashboard_bridge.py' 2>/dev/null || true
pkill -f "${ROOT}/build/trading-core" 2>/dev/null || true
sleep 2
# Skip preflight/prelive on restart — checks run on deploy or manually.
export START_SKIP_PRELIVE="${START_SKIP_PRELIVE:-1}"
setsid -f -- .venv/bin/python -u start_bot.py --skip-preflight >> logs/bridge.log 2>&1
sleep 3
pgrep -af 'start_bot|trading-core' || { echo "FAILED to start"; tail -20 logs/bridge.log; exit 1; }
echo "bot started (web not started — run: bash server_start_web.sh)"
