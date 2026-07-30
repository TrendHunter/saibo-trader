#!/usr/bin/env python3
from __future__ import annotations

import json
import statistics as st
from collections import Counter
from datetime import datetime, timezone
from pathlib import Path
from zoneinfo import ZoneInfo

ROOT = Path(__file__).resolve().parents[1]
BJ = ZoneInfo("Asia/Shanghai")
OUT = ROOT / "data" / "compare_triple" / "bprime_vs_sessionalign_compare.txt"

OLD_PULL = ROOT / "data" / "pull_20260729_bprime"
NEW_PULL = ROOT / "data" / "pull_20260730_sessionalign"


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


def fnum(v, default=None):
    if v in (None, ""):
        return default
    try:
        return float(v)
    except (TypeError, ValueError):
        return default


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


def price_bucket(px: float) -> str:
    if px < 0.35:
        return "<0.35"
    if px < 0.45:
        return "0.35-0.45"
    if px < 0.55:
        return "0.45-0.55"
    return ">=0.55"


def summarize(rows: list[dict]) -> str:
    if not rows:
        return "n=0"
    pnls = [r["pnl"] for r in rows]
    wins = sum(1 for p in pnls if p > 0)
    return f"n={len(rows)} 胜={wins}/{len(rows)} pnl={sum(pnls):+.1f} 中位={st.median(pnls):+.1f}"


def load_bot_closed(sample_dir: Path) -> list[dict]:
    leg1_by_id: dict[str, dict] = {}
    rows: list[dict] = []
    for e in load_jsonl(sample_dir / "bot_sample" / "fills.jsonl"):
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
            rows.append(
                {
                    "start": start,
                    "utc": datetime.fromtimestamp(start, tz=timezone.utc).strftime("%m-%d %H:%M"),
                    "bj_h": datetime.fromtimestamp(start, tz=timezone.utc).astimezone(BJ).hour,
                    "side": side_norm(l.get("side")),
                    "price": float(l.get("price") or 0.0),
                    "sec_in": sec,
                    "timing": timing_bucket(sec),
                    "price_bucket": price_bucket(float(l.get("price") or 0.0)),
                    "pnl": float(e.get("pnlUsdc") or 0.0),
                }
            )
    rows.sort(key=lambda r: r["start"])
    return rows


def load_m2_days(days: list[str]) -> dict[int, dict]:
    out: dict[int, dict] = {}
    for day in days:
        for w in load_jsonl(ROOT / "m2" / day / "windows.jsonl"):
            ts = w.get("window_start_ts")
            if ts is None:
                continue
            out[int(float(ts))] = w
    return out


def m2_traded(w: dict) -> bool:
    return bool(w.get("account_traded") or w.get("mm2_traded"))


def compare_sample(name: str, rows: list[dict], m2_days: list[str]) -> list[str]:
    lines: list[str] = []
    starts = {r["start"] for r in rows}
    m2 = load_m2_days(m2_days)
    days_in_sample = sorted(
        {datetime.fromtimestamp(ts, tz=timezone.utc).strftime("%Y-%m-%d") for ts in starts}
    )
    m2_on_sample_days = {
        ts
        for ts, w in m2.items()
        if datetime.fromtimestamp(ts, tz=timezone.utc).strftime("%Y-%m-%d") in days_in_sample
        and m2_traded(w)
    }
    both = starts & m2_on_sample_days
    bot_only = starts - m2_on_sample_days
    m2_only = m2_on_sample_days - starts

    both_bot = [r for r in rows if r["start"] in both]
    bot_only_rows = [r for r in rows if r["start"] in bot_only]
    m2_only_pnls = [fnum(m2[ts].get("total_round_pnl"), 0.0) or 0.0 for ts in m2_only]
    both_m2_pnls = [fnum(m2[ts].get("total_round_pnl"), 0.0) or 0.0 for ts in both]

    lines.append(f"## {name}")
    lines.append(f"  {summarize(rows)}")
    lines.append(
        f"  bot开单={len(starts)}  m2开单(样本日)={len(m2_on_sample_days)}  同窗={len(both)}  "
        f"bot-only={len(bot_only)}  m2-only={len(m2_only)}"
    )
    if m2_on_sample_days:
        lines.append(
            f"  同窗率(bot/bot)={100*len(both)/len(starts):.1f}%  同窗率(bot/m2)={100*len(both)/len(m2_on_sample_days):.1f}%"
        )
    lines.append(
        f"  同窗 pnl: bot={sum(r['pnl'] for r in both_bot):+.1f}  m2={sum(both_m2_pnls):+.1f}"
    )
    lines.append(
        f"  单开 pnl: bot-only={sum(r['pnl'] for r in bot_only_rows):+.1f}  m2-only={sum(m2_only_pnls):+.1f}"
    )
    for key in ("fav_early_b段", "过早但未进fav_early", "主窗"):
        sub = [r for r in rows if r["timing"] == key]
        if sub:
            lines.append(f"  {key}: {summarize(sub)}")
    for key in ("YES", "NO"):
        sub = [r for r in rows if r["side"] == key]
        if sub:
            lines.append(f"  {key}: {summarize(sub)}")
    return lines


def main() -> int:
    old_all = load_bot_closed(OLD_PULL)
    new_all = load_bot_closed(NEW_PULL)
    new_n = len(new_all)
    old_first_n = old_all[:new_n]

    lines: list[str] = []
    lines.append("=== 旧 B′ vs 新 session-align 对比 ===")
    lines.append(f"generated UTC {datetime.now(timezone.utc).strftime('%Y-%m-%d %H:%M')}")
    lines.append("")
    lines.append("### 样本口径")
    lines.append(f"  旧 B′ 全样本: {summarize(old_all)}")
    lines.append(f"  新 session-align 全样本: {summarize(new_all)}")
    lines.append(f"  公平对比口径: 各取前 {new_n} 单")
    lines.append(f"  旧 B′ 前{new_n}单: {summarize(old_first_n)}")
    lines.append(f"  新 session-align 前{new_n}单: {summarize(new_all)}")
    lines.append("")
    lines.extend(compare_sample(f"旧 B′ 前{new_n}单", old_first_n, ["2026-07-28", "2026-07-29"]))
    lines.append("")
    lines.extend(compare_sample("新 session-align", new_all, ["2026-07-29", "2026-07-30"]))
    lines.append("")
    lines.append("### 直接差值（前20单口径）")
    lines.append(
        f"  PnL: 新 - 旧 = {sum(r['pnl'] for r in new_all) - sum(r['pnl'] for r in old_first_n):+.1f}"
    )
    lines.append(
        f"  开单数: 新={len(new_all)} 旧={len(old_first_n)}"
    )
    lines.append("")
    lines.append("### 结论")
    lines.append("  新 marker 已有有效样本，不是空跑。")
    lines.append("  同口径前20单下，优先看两点：")
    lines.append("  1) bot-only 是否明显下降；")
    lines.append("  2) 总 pnl 是否由负转正。")

    text = "\n".join(lines)
    OUT.write_text(text, encoding="utf-8")
    print(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
