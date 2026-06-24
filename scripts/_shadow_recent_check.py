#!/usr/bin/env python3
"""Analyze recent shadow order activity on VPS."""
from __future__ import annotations

import json
import re
import sys
from datetime import datetime, timezone
from pathlib import Path

import paramiko

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from remote_deploy import HOST, PROJ, USER, load_password  # noqa: E402

SHADOW_RE = re.compile(
    r"\[LIVE LIH SHADOW\] (LEG1|HEDGE|SCALE|REBALANCE)\s+(\w+)\s+(\d+m)?",
    re.I,
)


def ro(c, cmd, t=90):
    _, o, e = c.exec_command(cmd, timeout=t)
    return (o.read() + e.read()).decode(errors="replace").strip()


def main() -> int:
    c = paramiko.SSHClient()
    c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    c.connect(HOST, username=USER, password=load_password(), timeout=60)
    try:
        now = datetime.now(timezone.utc)
        print(f"=== CHECK @ {now.strftime('%Y-%m-%d %H:%M UTC')} ===\n")

        print("=== PAUSE ===")
        print(ro(c, f"test -f '{PROJ}/logs/STOP_TRADING' && echo PAUSED || echo RUNNING"))

        print("\n=== SHADOW COUNTS (bridge.log) ===")
        for kind in ("LEG1", "HEDGE", "SCALE", "REBALANCE"):
            n = ro(c, f"grep -c 'LIVE LIH SHADOW] {kind}' '{PROJ}/logs/bridge.log' 2>/dev/null || echo 0")
            print(f"  {kind}: {n}")

        print("\n=== LAST 15 SHADOW LINES (bridge.log) ===")
        tail = ro(c, f"grep 'LIVE LIH SHADOW' '{PROJ}/logs/bridge.log' | tail -15")
        if not tail:
            print("  (none)")
        else:
            for ln in tail.splitlines():
                print(f"  {ln[:160]}")

        print("\n=== LAST 2h SHADOW IN bot.log (if any) ===")
        print(ro(c, f"grep 'LIVE LIH SHADOW' '{PROJ}/bot.log' 2>/dev/null | tail -10 || echo none"))

        print("\n=== STOP / PAUSE EVENTS (recent) ===")
        print(ro(c, f"grep -E 'STOP_TRADING|CONFIG PAUSE|CONFIG RESUME|manual Web resume' '{PROJ}/logs/bridge.log' | tail -8"))

        print("\n=== API open + session ===")
        raw = ro(c, "curl -s -m 8 http://127.0.0.1:8081/api/config", t=15)
        try:
            live = json.loads(raw).get("live", {})
            print(
                f"  status={live.get('status')} reason={live.get('statusReason')!r} "
                f"open={live.get('openCount')} sess={live.get('lihSessionLegsUsed')}/{live.get('lihSessionMaxLegs')}"
            )
            ops = live.get("openPositions") or []
            print(f"  openPositions: {len(ops)}")
            th = live.get("tradeHistory") or []
            shadow_trades = [t for t in th if t.get("isShadow") or t.get("strategy") == "LIH-SHADOW"]
            print(f"  tradeHistory total={len(th)} shadow-flagged={len(shadow_trades)}")
            if shadow_trades[:3]:
                for t in shadow_trades[:3]:
                    print(f"    - {t.get('id')} {t.get('asset')} {t.get('status')}")
        except Exception as ex:
            print("  parse fail:", ex)

        print("\n=== CORE TELEMETRY (last 5 with SHADOW) ===")
        # bot.log has [CORE INFO] prefix from bridge
        telem = ro(
            c,
            f"grep 'LIVE LIH SHADOW' '{PROJ}/logs/bridge.log' | wc -l; "
            f"stat -c 'bridge.log mtime=%y size=%s' '{PROJ}/logs/bridge.log'",
        )
        print(telem)
    finally:
        c.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
