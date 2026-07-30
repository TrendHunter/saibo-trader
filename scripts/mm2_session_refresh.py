#!/usr/bin/env python3
"""Build bot session hour list from mm2 observation packs.

Default behavior avoids using today's partial pack, which can collapse the
session file to only the hours observed so far and incorrectly mark the rest
of the UTC day as inactive.
"""
from __future__ import annotations

import json
import sys
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUT = ROOT / "data" / "mm2_session_active.json"
DEFAULT_M2 = ROOT / "m2"


def latest_obs_day(m2_root: Path) -> Path | None:
    """Newest m2/YYYY-MM-DD/ with windows.jsonl."""
    best: tuple[str, Path] | None = None
    if not m2_root.is_dir():
        return None
    for p in m2_root.iterdir():
        if not p.is_dir():
            continue
        if len(p.name) != 10 or p.name[4] != "-" or p.name[7] != "-":
            continue
        if not (p / "windows.jsonl").is_file():
            continue
        if best is None or p.name > best[0]:
            best = (p.name, p)
    return best[1] if best else None


def list_obs_days(m2_root: Path) -> list[Path]:
    days: list[Path] = []
    if not m2_root.is_dir():
        return days
    for p in sorted(m2_root.iterdir(), key=lambda x: x.name):
        if not p.is_dir():
            continue
        if len(p.name) != 10 or p.name[4] != "-" or p.name[7] != "-":
            continue
        if not (p / "windows.jsonl").is_file():
            continue
        days.append(p)
    return days


def active_hours_from_pack(pack_dir: Path) -> list[int]:
    hours: set[int] = set()
    path = pack_dir / "windows.jsonl"
    for ln in path.read_text(encoding="utf-8").splitlines():
        if not ln.strip().startswith("{"):
            continue
        w = json.loads(ln)
        if not w.get("session_active"):
            continue
        ts = w.get("window_start_ts")
        if ts is None:
            continue
        h = datetime.fromtimestamp(int(float(ts)), tz=timezone.utc).hour
        hours.add(h)
    return sorted(hours)


def auto_pick_pack(m2_root: Path) -> Path | None:
    """Prefer the most recent completed UTC day over today's partial pack."""
    days = list_obs_days(m2_root)
    if not days:
        return None
    today = datetime.now(timezone.utc).strftime("%Y-%m-%d")
    completed = [p for p in days if p.name < today]
    if completed:
        return completed[-1]
    return days[-1]


def refresh(
    m2_root: Path | None = None,
    out_path: Path | None = None,
    prefer_day: str | None = None,
) -> dict:
    m2_root = m2_root or DEFAULT_M2
    out_path = out_path or DEFAULT_OUT

    pack: Path | None = None
    if prefer_day:
        cand = m2_root / prefer_day
        if (cand / "windows.jsonl").is_file():
            pack = cand
    if pack is None:
        pack = auto_pick_pack(m2_root)
    if pack is None:
        raise FileNotFoundError(f"no mm2 pack under {m2_root} (need m2/YYYY-MM-DD/windows.jsonl)")

    hours = active_hours_from_pack(pack)
    if not hours:
        raise ValueError(f"no session_active hours in {pack}")

    meta_path = pack / "_meta.json"
    meta = {}
    if meta_path.is_file():
        meta = json.loads(meta_path.read_text(encoding="utf-8"))

    doc = {
        "schema_version": "mm2-session-hours-v1",
        "day_utc": pack.name,
        "source_pack": str(pack.relative_to(ROOT)).replace("\\", "/"),
        "active_hours_utc": hours,
        "windows_traded": meta.get("windows_traded"),
        "skip_reason_counts": meta.get("skip_reason_counts"),
        "updated_utc": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
    }
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(doc, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    return doc


def main() -> int:
    import argparse

    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--m2-root", type=Path, default=DEFAULT_M2)
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT)
    ap.add_argument("--day", default=None, help="Force YYYY-MM-DD pack")
    args = ap.parse_args()
    try:
        doc = refresh(args.m2_root, args.out, args.day)
    except (FileNotFoundError, ValueError) as exc:
        print(f"mm2_session_refresh: {exc}", file=sys.stderr)
        return 1
    print(
        f"mm2_session_refresh: day={doc['day_utc']} hours={doc['active_hours_utc']} -> {args.out}",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
