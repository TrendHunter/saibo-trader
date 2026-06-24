#!/usr/bin/env python3
"""Print shadow trade ledger from VPS API or local logs/shadow_trades.jsonl."""
from __future__ import annotations

import json
import sys
from pathlib import Path

import paramiko

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from remote_deploy import HOST, PROJ, USER, load_password  # noqa: E402


def ro(c, cmd, t=60):
    _, o, e = c.exec_command(cmd, timeout=t)
    return (o.read() + e.read()).decode(errors="replace").strip()


def fmt_row(r: dict) -> str:
    ev = r.get("event") or r.get("status", "?")
    asset = r.get("asset", "?")
    wid = r.get("windowMinutes") or r.get("window", "")
    side = r.get("side", "")
    px = r.get("price") or r.get("entryPrice")
    sh = r.get("shares") or r.get("size")
    hedged = r.get("fullyHedged")
    pnl = r.get("pnlUsdc")
    parts = [f"{ev:6}", f"{asset:4}", f"{wid}m" if wid else "", side]
    if px is not None:
        parts.append(f"@{px}")
    if sh is not None:
        parts.append(f"{sh}sh")
    if hedged is not None:
        parts.append("hedged" if hedged else "leg1-only")
    if pnl is not None and ev in ("CLOSED", "closed"):
        parts.append(f"pnl=${pnl:+.2f}")
    return " ".join(str(p) for p in parts if p != "")


def main() -> int:
    c = paramiko.SSHClient()
    c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    c.connect(HOST, username=USER, password=load_password(), timeout=60)
    try:
        raw = ro(c, "curl -s -m 8 http://127.0.0.1:8081/api/config", t=15)
        try:
            live = json.loads(raw).get("live", {})
            hist = live.get("shadowTradeHistory") or []
            print(f"shadowOpen={live.get('shadowOpenCount')} shadowPnl=${live.get('shadowLihPnl', 0):.2f}")
            print(f"shadowTradeHistory rows={len(hist)}")
            for r in hist[:25]:
                print(" ", fmt_row(r))
            if len(hist) > 25:
                print(f"  ... +{len(hist) - 25} more")
        except Exception as ex:
            print("API parse:", ex)

        print("\n--- shadow_trades.jsonl (last 15 events) ---")
        tail = ro(
            c,
            f"tail -15 '{PROJ}/logs/shadow_trades.jsonl' 2>/dev/null || echo '(file not created yet — rebuild core)'",
        )
        for ln in tail.splitlines():
            ln = ln.strip()
            if not ln:
                continue
            try:
                print(" ", fmt_row(json.loads(ln)))
            except json.JSONDecodeError:
                print(" ", ln[:120])
    finally:
        c.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
