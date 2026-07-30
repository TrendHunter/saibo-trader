#!/usr/bin/env python3
"""Pull VPS shadow observational ledger + trades; export full + current session."""
from __future__ import annotations

import argparse
import csv
import json
import sys
from collections import Counter
from datetime import datetime, timezone
from pathlib import Path

import paramiko

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from remote_deploy import HOST, PROJ, ROOT, USER, load_password  # noqa: E402

DEFAULT_MARKER = "session-v0151-window-once-20260630T080402"


def day_ts_range(day: str) -> tuple[int, int]:
    start = datetime.strptime(day, "%Y-%m-%d").replace(tzinfo=timezone.utc)
    end = start.replace(hour=23, minute=55, second=0)
    return int(start.timestamp()), int(end.timestamp())


def load_jsonl_text(raw: str) -> list[dict]:
    rows: list[dict] = []
    for ln in raw.splitlines():
        ln = ln.strip()
        if not ln or ln.startswith("#") or not ln.startswith("{"):
            continue
        try:
            rows.append(json.loads(ln))
        except json.JSONDecodeError:
            pass
    return rows


def load_jsonl_file(path: Path) -> list[dict]:
    if not path.exists():
        return []
    return load_jsonl_text(path.read_text(encoding="utf-8", errors="replace"))


def dedupe_windows(rows: list[dict]) -> list[dict]:
    def snap_score(w: dict) -> int:
        return sum(1 for k in w if str(k).startswith("snap_"))

    def pick(a: dict | None, b: dict) -> dict:
        if a is None:
            return b
        if bool(b.get("bot_traded")) and not bool(a.get("bot_traded")):
            return b
        if bool(a.get("bot_traded")) and not bool(b.get("bot_traded")):
            return a
        if snap_score(b) > snap_score(a):
            return b
        if snap_score(a) > snap_score(b):
            return a
        return b

    by_ts: dict[str, dict] = {}
    order: list[str] = []
    for w in rows:
        ts = w.get("window_start_ts")
        if ts is None:
            continue
        key = str(int(float(ts)))
        if key not in by_ts:
            order.append(key)
        by_ts[key] = pick(by_ts.get(key), w)
    return [by_ts[k] for k in order]


def segment(raw: str, marker: str) -> tuple[list[dict], list[str]]:
    """Return (events, window_rows) since marker in trades or windows file."""
    events: list[dict] = []
    windows: list[dict] = []
    after = False
    for ln in raw.splitlines():
        ln = ln.strip()
        if not ln:
            continue
        if ln.startswith("#"):
            m = ln[1:].strip()
            if marker in m:
                after, events, windows = True, [], []
                continue
            if after and m.startswith("session-") and marker not in m:
                break
            continue
        if not after:
            continue
        try:
            obj = json.loads(ln)
        except json.JSONDecodeError:
            continue
        if obj.get("event") == "WINDOW" or "window_start_ts" in obj:
            windows.append(obj)
        else:
            events.append(obj)
    return events, windows


def write_jsonl(path: Path, rows: list[dict]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        for row in rows:
            f.write(json.dumps(row, separators=(",", ":")) + "\n")


def write_csv(path: Path, rows: list[dict]) -> None:
    if not rows:
        path.write_text("", encoding="utf-8")
        return
    keys = sorted({k for r in rows for k in r})
    with open(path, "w", newline="", encoding="utf-8-sig") as f:
        w = csv.DictWriter(f, fieldnames=keys, extrasaction="ignore")
        w.writeheader()
        for row in rows:
            w.writerow({k: row.get(k, "") for k in keys})


def export_day(
    out_dir: Path,
    day: str,
    all_wins: list[dict],
    all_tr: list[dict],
) -> dict:
    lo, hi = day_ts_range(day)
    day_dir = out_dir / f"bot_{day}"
    day_dir.mkdir(parents=True, exist_ok=True)

    day_wins = [
        w for w in all_wins if lo <= int(float(w.get("window_start_ts", 0))) <= hi
    ]
    day_tr = [e for e in all_tr if e.get("ts") and lo <= float(e["ts"]) <= hi + 300]

    leg1_ts = {
        str(int(float(e["windowStartTs"])))
        for e in day_tr
        if e.get("event") == "LEG1" and e.get("windowStartTs") is not None
    }
    for w in day_wins:
        key = str(int(float(w["window_start_ts"])))
        if key in leg1_ts:
            w["bot_traded"] = True
            w["skip_reason"] = "traded"

    write_jsonl(day_dir / "windows.jsonl", day_wins)
    write_jsonl(day_dir / "fills.jsonl", day_tr)
    write_csv(day_dir / "windows.csv", day_wins)

    traded = sum(1 for w in day_wins if w.get("bot_traded"))
    snap120 = sum(1 for w in day_wins if w.get("snap_120_yes_ask") is not None)
    meta = {
        "schema_version": "bot-shadow-windows-v1",
        "export_day_utc": day,
        "windows_count": len(day_wins),
        "windows_traded": traded,
        "fills_events": len(day_tr),
        "leg1_count": sum(1 for e in day_tr if e.get("event") == "LEG1"),
        "closed_count": sum(1 for e in day_tr if e.get("event") == "CLOSED"),
        "snap_120_count": snap120,
    }
    (day_dir / "_meta.json").write_text(json.dumps(meta, indent=2), encoding="utf-8")
    return meta


def main() -> int:
    ap = argparse.ArgumentParser(description="Pull VPS shadow + window observational data")
    ap.add_argument("--marker", default=DEFAULT_MARKER, help="Session marker for sample export")
    ap.add_argument("--days", default="2026-06-30,2026-07-01", help="UTC days to slice")
    ap.add_argument(
        "--out",
        default=str(ROOT / "data" / f"pull_{datetime.now(timezone.utc).strftime('%Y%m%d')}"),
    )
    args = ap.parse_args()
    out_dir = Path(args.out)

    c = paramiko.SSHClient()
    c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    c.connect(HOST, username=USER, password=load_password(), timeout=60)
    try:
        sftp = c.open_sftp()
        raw_dir = out_dir / "raw"
        raw_dir.mkdir(parents=True, exist_ok=True)
        remote_files = {
            "shadow_trades.jsonl": f"{PROJ}/logs/shadow_trades.jsonl",
            "shadow_windows.jsonl": f"{PROJ}/logs/shadow_windows.jsonl",
            "lih_skip.log": f"{PROJ}/logs/lih_skip.log",
        }
        sizes: dict[str, int] = {}
        for local_name, remote in remote_files.items():
            dest = raw_dir / local_name
            print(f"fetch {remote} -> {dest}")
            try:
                sftp.get(remote, str(dest))
                sizes[local_name] = dest.stat().st_size
            except OSError as ex:
                print(f"  skip {local_name}: {ex}")
        sftp.close()

        trades_raw = (raw_dir / "shadow_trades.jsonl").read_text(encoding="utf-8", errors="replace")
        wins_raw = (raw_dir / "shadow_windows.jsonl").read_text(encoding="utf-8", errors="replace")

        all_tr = load_jsonl_text(trades_raw)
        all_wins = dedupe_windows(load_jsonl_text(wins_raw))

        # current session sample
        sample_dir = out_dir / "bot_sample"
        sample_dir.mkdir(parents=True, exist_ok=True)
        tr_ev, win_ev = segment(trades_raw, args.marker)
        _, win_from_wins = segment(wins_raw, args.marker)
        if win_from_wins:
            win_ev = dedupe_windows(win_from_wins)
        elif tr_ev:
            # windows may share marker only in windows file; also grab by traded window ts
            wts = {
                int(float(e["windowStartTs"]))
                for e in tr_ev
                if e.get("windowStartTs") is not None
            }
            win_ev = [w for w in all_wins if int(float(w.get("window_start_ts", 0))) in wts]

        write_jsonl(sample_dir / "fills.jsonl", tr_ev)
        write_jsonl(sample_dir / "windows.jsonl", win_ev)
        write_csv(sample_dir / "windows.csv", win_ev)
        sample_meta = {
            "marker": args.marker,
            "fills_events": len(tr_ev),
            "windows_count": len(win_ev),
            "events": dict(Counter(e.get("event", "?") for e in tr_ev)),
            "closed_pnl": sum(float(e.get("pnlUsdc") or 0) for e in tr_ev if e.get("event") == "CLOSED"),
        }
        (sample_dir / "_meta.json").write_text(json.dumps(sample_meta, indent=2), encoding="utf-8")

        day_metas = {}
        for day in [d.strip() for d in args.days.split(",") if d.strip()]:
            day_metas[day] = export_day(out_dir, day, all_wins, all_tr)

        summary = {
            "pulled_utc": datetime.now(timezone.utc).isoformat(),
            "vps_host": HOST,
            "raw_bytes": sizes,
            "all_trades_events": len(all_tr),
            "all_windows_rows": len(all_wins),
            "all_events": dict(Counter(e.get("event", "?") for e in all_tr)),
            "session_sample": sample_meta,
            "day_exports": day_metas,
            "note": "mm2 observer packs (fills/windows) are NOT on bot VPS; use m2-2026-* or tracker zip locally",
        }
        (out_dir / "_pull_summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")

        print(f"\n=== pulled -> {out_dir} ===")
        print(json.dumps(summary, indent=2))
    finally:
        c.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
