#!/usr/bin/env python3
from __future__ import annotations

import json
import statistics as st
from collections import Counter
from datetime import datetime, timezone
from pathlib import Path
from zoneinfo import ZoneInfo

ROOT = Path(__file__).resolve().parents[1]
PULL = ROOT / "data" / "pull_20260729_bprime"
OUT = ROOT / "data" / "compare_triple" / "bprime_fix_compare.txt"
BJ = ZoneInfo("Asia/Shanghai")


def load_jsonl(path: Path) -> list[dict]:
    rows: list[dict] = []
    if not path.exists():
        return rows
    for ln in path.read_text(encoding="utf-8", errors="replace").splitlines():
        ln = ln.strip()
        if not ln.startswith("{"):
            continue
        try:
            rows.append(json.loads(ln))
        except json.JSONDecodeError:
            pass
    return rows


def side_norm(s: str) -> str:
    s = (s or "").upper()
    if s in ("YES", "UP"):
        return "YES"
    if s in ("NO", "DOWN"):
        return "NO"
    return s or "?"


def timing_bucket(sec_in: float) -> str:
    left = 300.0 - sec_in
    if left > 180:
        return "fav_early_b段"
    if left > 100:
        return "过早但未进fav_early"
    return "主窗"


def fnum(v, default=None):
    if v in (None, ""):
        return default
    try:
        return float(v)
    except (TypeError, ValueError):
        return default


def would_fail_hole_fix(m2w: dict | None) -> tuple[bool | None, str]:
    if m2w is None:
        return None, "no_m2"
    # Refined hole fix proxy:
    # early YES must not be underdog/cheap side, and YES ask itself should not be below 0.65.
    for pref in ("snap_leg1", "snap_120", "snap_60", "snap_30", "snap_0"):
        y = fnum(m2w.get(f"{pref}_yes_ask"))
        n = fnum(m2w.get(f"{pref}_no_ask"))
        if y is None or n is None:
            continue
        if y < n:
            return True, f"{pref}:yes<{n:.2f}"
        if y < 0.65:
            return True, f"{pref}:yes={y:.2f}<0.65"
        return False, f"{pref}:yes={y:.2f} no={n:.2f}"
    return None, "no_snap"


def summarize(rows: list[dict]) -> dict:
    pnls = [r["pnl"] for r in rows]
    wins = sum(1 for p in pnls if p > 0)
    med = st.median(pnls) if pnls else 0.0
    return {"n": len(rows), "wins": wins, "pnl": sum(pnls), "med": med}


def fmt(name: str, rows: list[dict]) -> str:
    s = summarize(rows)
    return f"{name}: n={s['n']} 胜={s['wins']}/{s['n']} pnl={s['pnl']:+.1f} 中位={s['med']:+.1f}"


def main() -> int:
    leg1_by_id: dict[str, dict] = {}
    bot: list[dict] = []
    for e in load_jsonl(PULL / "bot_sample" / "fills.jsonl"):
        if str(e.get("asset") or "").lower() != "btc" or int(e.get("windowMinutes") or 0) != 5:
            continue
        ev = str(e.get("event") or "").upper()
        if ev == "LEG1":
            leg1_by_id[str(e["id"])] = e
        elif ev == "CLOSED":
            lid = str(e.get("id") or "")
            if lid not in leg1_by_id:
                continue
            l = leg1_by_id[lid]
            sec = float(l.get("secIn") or 0.0)
            start = int(float(l.get("windowStartTs") or (float(l["endDateTs"]) - 300)))
            bot.append(
                {
                    "start": start,
                    "utc": datetime.fromtimestamp(start, tz=timezone.utc).strftime("%m-%d %H:%M"),
                    "bj_h": datetime.fromtimestamp(start, tz=timezone.utc).astimezone(BJ).hour,
                    "side": side_norm(l.get("side")),
                    "price": float(l.get("price") or 0.0),
                    "sec_in": sec,
                    "timing": timing_bucket(sec),
                    "pnl": float(e.get("pnlUsdc") or 0.0),
                }
            )

    m2: dict[int, dict] = {}
    for day in ("2026-07-28", "2026-07-29"):
        for w in load_jsonl(ROOT / "m2" / day / "windows.jsonl"):
            ts = w.get("window_start_ts")
            if ts is not None:
                m2[int(float(ts))] = w

    for r in bot:
        w = m2.get(r["start"])
        r["m2_skip"] = str(w.get("skip_reason") or "?") if w else "no_m2_row"
        fail, detail = would_fail_hole_fix(w)
        r["hole_fix_fail"] = fail
        r["hole_fix_detail"] = detail

    baseline = bot
    session_aligned = [r for r in bot if r["m2_skip"] != "session_off"]
    hole_fixed = [
        r
        for r in bot
        if not (r["side"] == "YES" and r["timing"] == "过早但未进fav_early" and r["hole_fix_fail"] is True)
    ]
    combined = [
        r
        for r in session_aligned
        if not (r["side"] == "YES" and r["timing"] == "过早但未进fav_early" and r["hole_fix_fail"] is True)
    ]
    all_yes_early_block = [
        r for r in bot if not (r["side"] == "YES" and r["timing"] == "过早但未进fav_early")
    ]

    removed_session = [r for r in bot if r["m2_skip"] == "session_off"]
    removed_hole = [
        r
        for r in bot
        if r["side"] == "YES" and r["timing"] == "过早但未进fav_early" and r["hole_fix_fail"] is True
    ]
    combined_keys = {r["start"] for r in combined}
    removed_combined = [r for r in bot if r["start"] not in combined_keys]
    overlap = [r for r in removed_hole if r["m2_skip"] == "session_off"]

    lines: list[str] = []
    lines.append("=== B′ 后：session 对齐 vs 补洞 回测 ===")
    lines.append(f"generated UTC {datetime.now(timezone.utc).strftime('%Y-%m-%d %H:%M')}")
    lines.append("样本: bot-early-yes-guard-bprime-20260728 后 112 单")
    lines.append("")
    lines.append("### 总对比")
    for name, rows in (
        ("baseline", baseline),
        ("1) 仅 session 对齐", session_aligned),
        ("2) 仅补洞", hole_fixed),
        ("1+2 组合", combined),
        ("补充上界: 全挡 YES过早", all_yes_early_block),
    ):
        lines.append("  " + fmt(name, rows))

    base_pnl = sum(r["pnl"] for r in baseline)
    lines.append("")
    lines.append("### 相对 baseline 改善")
    for name, rows in (
        ("1) session 对齐", session_aligned),
        ("2) 补洞", hole_fixed),
        ("1+2 组合", combined),
        ("上界: 全挡 YES过早", all_yes_early_block),
    ):
        pnl = sum(r["pnl"] for r in rows)
        lines.append(f"  {name}: Δpnl={pnl - base_pnl:+.1f}  少做={len(baseline) - len(rows)}单")

    lines.append("")
    lines.append("### 被去掉的单")
    lines.append("  " + fmt("session_off 去掉", removed_session))
    lines.append("  " + fmt("补洞去掉(代理应挡)", removed_hole))
    lines.append("  " + fmt("组合去掉", removed_combined))
    lines.append(
        f"  overlap(session_off ∩ 补洞)={len(overlap)} 单 pnl={sum(r['pnl'] for r in overlap):+.1f}"
    )

    lines.append("")
    lines.append("### 补洞去掉的组成")
    for k, _ in Counter(r["m2_skip"] for r in removed_hole).most_common():
        sub = [r for r in removed_hole if r["m2_skip"] == k]
        lines.append("  " + fmt(f"m2_skip={k}", sub))

    lines.append("")
    lines.append("### 结论")
    lines.append("  session 对齐收益主要来自砍掉 m2 session_off 时段的 bot-only 单。")
    lines.append("  补洞收益主要来自砍掉 YES × 过早 × 低价(cheap YES) 单。")
    lines.append("  若两者高度重叠，先做 session；若补洞在非session窗口也显著赚钱，再紧跟补洞。")

    OUT.write_text("\n".join(lines), encoding="utf-8")
    print("\n".join(lines))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
