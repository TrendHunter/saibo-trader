#!/usr/bin/env python3
"""Append hourly shadow sample to logs/shadow_hourly.log (run on VPS via cron)."""
from __future__ import annotations

import json
import sys
from datetime import datetime, timezone
from pathlib import Path

PROJ = Path(__file__).resolve().parents[1]
LEDGER = PROJ / "logs" / "shadow_trades.jsonl"
OUT = PROJ / "logs" / "shadow_hourly.log"


def load_events() -> list[dict]:
    if not LEDGER.exists():
        return []
    out: list[dict] = []
    for ln in LEDGER.read_text(encoding="utf-8", errors="replace").splitlines():
        ln = ln.strip()
        if not ln or ln.startswith("#"):
            continue
        try:
            out.append(json.loads(ln))
        except json.JSONDecodeError:
            pass
    return out


def latest_marker(events: list[dict]) -> str:
    markers: list[str] = []
    for ln in LEDGER.read_text(encoding="utf-8", errors="replace").splitlines():
        ln = ln.strip()
        if ln.startswith("#"):
            markers.append(ln[1:].strip())
    return markers[-1] if markers else "all"


def segment_since_marker(raw: str, marker: str) -> list[dict]:
    seg: list[dict] = []
    after = marker == "all"
    for ln in raw.splitlines():
        ln = ln.strip()
        if not ln:
            continue
        if ln.startswith("#"):
            if marker != "all" and marker in ln:
                after = True
                seg = []
            elif after and marker != "all":
                break
            continue
        if after:
            try:
                seg.append(json.loads(ln))
            except json.JSONDecodeError:
                pass
    return seg if marker != "all" else load_events()


def summarize(events: list[dict]) -> dict:
    closed = [e for e in events if e.get("event") == "CLOSED"]
    leg1 = [e for e in events if e.get("event") == "LEG1"]
    hedge = [e for e in events if e.get("event") == "HEDGE"]
    pnl = sum(float(e.get("pnlUsdc") or 0) for e in closed)
    hedged = sum(
        1
        for e in closed
        if e.get("fullyHedged") or "(hedged)" in str(e.get("exitReason", ""))
    )
    unhedged = len(closed) - hedged
    wins = sum(1 for e in closed if float(e.get("pnlUsdc") or 0) > 0)
    losses = sum(1 for e in closed if float(e.get("pnlUsdc") or 0) < 0)
    big = sum(1 for e in closed if float(e.get("pnlUsdc") or 0) < -10)
    return {
        "leg1": len(leg1),
        "hedge": len(hedge),
        "closed": len(closed),
        "hedged": hedged,
        "unhedged": unhedged,
        "wins": wins,
        "losses": losses,
        "big_loss": big,
        "pnl": pnl,
    }


def main() -> int:
    raw = LEDGER.read_text(encoding="utf-8", errors="replace") if LEDGER.exists() else ""
    marker = latest_marker(load_events())
    events = segment_since_marker(raw, marker)
    s = summarize(events)
    ts = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M UTC")
    line = (
        f"{ts} | marker={marker[:48]} | "
        f"LEG1={s['leg1']} HEDGE={s['hedge']} CLOSED={s['closed']} "
        f"hedged={s['hedged']} unhedged={s['unhedged']} "
        f"W/L={s['wins']}/{s['losses']} bigLoss={s['big_loss']} "
        f"pnl=${s['pnl']:+.2f}\n"
    )
    OUT.parent.mkdir(parents=True, exist_ok=True)
    with OUT.open("a", encoding="utf-8") as f:
        f.write(line)
    print(line, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
