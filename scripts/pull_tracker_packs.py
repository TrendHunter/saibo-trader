#!/usr/bin/env python3
"""Pull m2/m3/m4 tracker day packs from bot VPS to local repo.

Monitoring server pushes packs to VPS /opt/polymarket-bot/{m2,m3,m4}/YYYY-MM-DD/.
This script syncs them down for offline analysis.

  python scripts/pull_tracker_packs.py
  python scripts/pull_tracker_packs.py --accounts m2,m3,m4 --days 2026-07-09
  python scripts/pull_tracker_packs.py --latest 3
"""
from __future__ import annotations

import argparse
import json
import sys
from datetime import datetime, timedelta, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "scripts"))

from remote_deploy import PROJ  # noqa: E402
from vps_shadow_autorun import connect, ro  # noqa: E402

PACK_FILES = (
    "windows.jsonl",
    "fills.jsonl",
    "sessions.jsonl",
    "gaps.jsonl",
    "_meta.json",
)


def list_remote_days(c, account: str) -> list[str]:
    raw = ro(c, f"ls -1 '{PROJ}/{account}/' 2>/dev/null || true")
    days = []
    for ln in raw.splitlines():
        name = ln.strip()
        if len(name) == 10 and name[4] == "-" and name[7] == "-":
            days.append(name)
    return sorted(days)


def pull_pack(c, account: str, day: str) -> dict:
    remote_dir = f"{PROJ}/{account}/{day}"
    sz = ro(c, f"stat -c%s '{remote_dir}/windows.jsonl' 2>/dev/null || echo 0").strip()
    try:
        size = int(sz.split()[-1])
    except ValueError:
        size = 0
    if size <= 0:
        return {"account": account, "day": day, "ok": False, "reason": "no windows.jsonl on VPS"}

    local = ROOT / account / day
    local.mkdir(parents=True, exist_ok=True)
    sftp = c.open_sftp()
    pulled: list[str] = []
    for name in PACK_FILES:
        try:
            sftp.get(f"{remote_dir}/{name}", str(local / name))
            pulled.append(name)
        except OSError:
            pass
    sftp.close()
    return {"account": account, "day": day, "ok": True, "files": pulled, "local": str(local)}


def main() -> int:
    ap = argparse.ArgumentParser(description="Pull tracker packs from bot VPS")
    ap.add_argument("--accounts", default="m2,m3,m4", help="comma-separated: m2,m3,m4")
    ap.add_argument("--days", nargs="*", help="UTC days YYYY-MM-DD")
    ap.add_argument("--latest", type=int, default=0, help="pull N most recent days per account")
    ap.add_argument("--all", action="store_true", help="pull every day present on VPS")
    args = ap.parse_args()

    accounts = [a.strip() for a in args.accounts.split(",") if a.strip()]
    c = connect()
    summary: dict = {"accounts": {}}
    try:
        for account in accounts:
            remote_days = list_remote_days(c, account)
            if args.all:
                want = remote_days
            elif args.days:
                want = [d for d in args.days if d in remote_days]
            elif args.latest > 0:
                want = remote_days[-args.latest :]
            else:
                today = datetime.now(timezone.utc).date()
                want = [
                    d
                    for d in remote_days
                    if d >= (today - timedelta(days=2)).isoformat()
                ] or remote_days[-1:]

            rows = []
            for day in want:
                rows.append(pull_pack(c, account, day))
            summary["accounts"][account] = {
                "remote_days": remote_days,
                "pulled": rows,
            }
    finally:
        c.close()

    print(json.dumps(summary, indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
