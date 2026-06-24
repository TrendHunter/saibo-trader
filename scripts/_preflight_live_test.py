#!/usr/bin/env python3
"""Preflight before live test round."""
from __future__ import annotations

import json
import sys
from pathlib import Path

import paramiko

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from remote_deploy import HOST, PROJ, USER, load_password  # noqa: E402


def ro(c, cmd, t=30):
    _, o, e = c.exec_command(cmd, timeout=t)
    return (o.read() + e.read()).decode(errors="replace").strip()


def main():
    c = paramiko.SSHClient()
    c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    c.connect(HOST, username=USER, password=load_password(), timeout=60)
    try:
        print("=== ENV ===")
        print(ro(c, f"grep -E '^(LIVE_LIH_DRY_RUN|RISK_MAX|LIH_PAUSE|AUTO_REDEEM|LIH_LEG1_MODE)=' '{PROJ}/.env'"))

        print("\n=== PROCESSES ===")
        print(ro(c, "pgrep -af 'start_bot|trading-core' | grep -v pgrep || echo DOWN"))

        print("\n=== FIX CHECKS ===")
        print("redeem:", ro(c, f"strings '{PROJ}/build/trading-core' | grep 'python3 redeem' | head -1"))
        print("hedge fix:", ro(c, f"grep -c 'buy_usdc_for_shares' '{PROJ}/clob_live.py' || echo 0"))
        print("frontend build:", ro(c, f"stat -c '%y' '{PROJ}/frontend/.next/BUILD_ID' 2>/dev/null || echo missing"))
        print("disk_guard cron:", ro(c, "crontab -l 2>/dev/null | grep disk_guard || echo none"))

        print("\n=== API ===")
        raw = ro(c, "curl -s -m 8 http://127.0.0.1:8081/api/config", t=15)
        try:
            data = json.loads(raw)
            live = data.get("live", {})
            print(json.dumps({
                "balance": live.get("balance"),
                "open": live.get("openCount"),
                "riskMax": live.get("riskMaxConcurrentPositions"),
                "status": live.get("status"),
                "reason": live.get("statusReason"),
                "sess": f"{live.get('lihSessionLegsUsed')}/{live.get('lihSessionMaxLegs')}",
            }, indent=2))
            th = live.get("tradeHistory") or []
            if th:
                r = th[0]
                print("latest trade:", {
                    "id": r.get("id"),
                    "yes": r.get("yesEntryPrice"),
                    "no": r.get("noEntryPrice"),
                    "pnl": r.get("pnlUsdc"),
                    "exitReason": r.get("exitReason"),
                })
        except Exception as e:
            print("api parse fail:", e, raw[:200])
    finally:
        c.close()


if __name__ == "__main__":
    main()
