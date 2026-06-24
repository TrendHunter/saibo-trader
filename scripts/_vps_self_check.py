#!/usr/bin/env python3
"""Run VPS self-check: paused, dry-run, preflight, health — no live orders."""
from __future__ import annotations

import json
import sys
from pathlib import Path

import paramiko

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from remote_deploy import HOST, PROJ, USER, load_password  # noqa: E402


def ro(c, cmd, t=120):
    _, o, e = c.exec_command(cmd, timeout=t)
    out = (o.read() + e.read()).decode(errors="replace").strip()
    return out


def main() -> int:
    c = paramiko.SSHClient()
    c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    c.connect(HOST, username=USER, password=load_password(), timeout=60)
    rc = 0
    try:
        print("=== PAUSE + DRY-RUN ===")
        print(ro(c, f"touch '{PROJ}/logs/STOP_TRADING' && test -f '{PROJ}/logs/STOP_TRADING' && echo STOP_TRADING=ok"))
        env = ro(c, f"grep -E '^(LIVE_LIH_DRY_RUN|PAPER_MODE|LIH_LEG1_MODE|LIH_TARGET_COMBINED)=' '{PROJ}/.env'")
        print(env)
        if "LIVE_LIH_DRY_RUN=true" not in env:
            print("WARN: LIVE_LIH_DRY_RUN not true")

        print("\n=== GIT HEAD ===")
        print(ro(c, f"cd '{PROJ}' && git rev-parse --short HEAD && git log -1 --oneline"))

        print("\n=== BINARY ===")
        print(ro(c, f"strings '{PROJ}/build/trading-core' | grep -E 'LegInHedge|DumpHedge' | head -5"))

        print("\n=== PREFLIGHT ===")
        pf = ro(
            c,
            f"cd '{PROJ}' && .venv/bin/python start_bot.py --preflight-only --skip-prelive 2>&1 | tail -40",
            t=180,
        )
        print(pf)
        if "✅ 自检通过" in pf or "preflight ok" in pf.lower():
            print("preflight: PASS")
        elif "[FAIL]" in pf or "自检未通过" in pf:
            print("preflight: FAIL")
            rc = 1
        else:
            print("preflight: see output above")

        print("\n=== PROCESSES ===")
        print(ro(c, "pgrep -af 'start_bot|trading-core' | grep -v pgrep || echo DOWN"))

        print("\n=== HEALTH ===")
        print(ro(c, "curl -s -m 8 http://127.0.0.1:8081/health"))
        raw = ro(c, "curl -s -m 8 http://127.0.0.1:8081/api/config", t=15)
        try:
            data = json.loads(raw)
            live = data.get("live", {})
            print(json.dumps({
                "status": live.get("status"),
                "reason": live.get("statusReason"),
                "balance": live.get("balance"),
                "open": live.get("openCount"),
                "strategy": live.get("strategyMode") or live.get("strategy"),
            }, indent=2))
            if live.get("status") == "trading" or live.get("status") == 1:
                print("WARN: bot actively trading — expected paused")
                rc = 1
            elif live.get("status") == 3:
                print("paused/startup (STOP_TRADING): OK")
        except Exception as ex:
            print("api parse fail:", ex, raw[:300])
            rc = 1

        print("\n=== BRIDGE LOG (last 8) ===")
        print(ro(c, f"tail -8 '{PROJ}/logs/bridge.log'"))
    finally:
        c.close()
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
