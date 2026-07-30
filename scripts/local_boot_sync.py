#!/usr/bin/env python3
"""Run when local PC is on: pull bot shadow + tracker packs, refresh mm2 session, push to VPS.

Tracker m2/m3/m4 day packs are pushed to bot VPS by the monitoring server; this script
pulls them down to local m2/ m3/ m4/ for offline analysis.

Usage:
  python scripts/local_boot_sync.py           # pull shadow + tracker + refresh + push session
  python scripts/local_boot_sync.py --no-push # local analysis only
  python scripts/local_boot_sync.py --push-only
  python scripts/local_boot_sync.py --no-tracker-pull
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "scripts"))


def run_py(script: str, *args: str) -> int:
    cmd = [sys.executable, str(ROOT / "scripts" / script), *args]
    print(f">>> {' '.join(cmd)}", file=sys.stderr)
    return subprocess.call(cmd, cwd=str(ROOT))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--no-pull", action="store_true", help="skip VPS shadow incremental pull")
    ap.add_argument("--no-tracker-pull", action="store_true", help="skip m2/m3/m4 pack pull from VPS")
    ap.add_argument("--no-push", action="store_true", help="skip push session/pack to VPS")
    ap.add_argument("--push-only", action="store_true", help="only push (no shadow pull)")
    args = ap.parse_args()

    summary: dict = {"started_utc": datetime.now(timezone.utc).isoformat(), "steps": []}

    if not args.push_only and not args.no_pull:
        rc = run_py("_fetch_shadow_incremental_now.py")
        summary["steps"].append({"pull_shadow": rc})
        if rc != 0:
            print("warn: shadow pull failed (offline?)", file=sys.stderr)

    if not args.push_only and not args.no_tracker_pull:
        rc = run_py("pull_tracker_packs.py", "--latest", "3")
        summary["steps"].append({"pull_tracker_packs": rc})
        if rc != 0:
            print("warn: tracker pack pull failed", file=sys.stderr)

    from mm2_session_refresh import latest_obs_day, refresh  # noqa: E402

    pack = latest_obs_day(ROOT / "m2")
    if pack is None:
        summary["session"] = {"error": "no m2/YYYY-MM-DD pack — drop polycopy export first"}
        print(json.dumps(summary, indent=2))
        return 0 if args.no_push else 1

    try:
        doc = refresh()
        summary["session"] = doc
    except Exception as exc:
        summary["session"] = {"error": str(exc)}
        print(json.dumps(summary, indent=2))
        return 1

    if not args.no_push:
        try:
            from push_mm2_session_to_vps import push_to_vps  # noqa: E402

            summary["vps_push"] = push_to_vps(pack)
        except Exception as exc:
            summary["vps_push"] = {"error": str(exc)}
            print(json.dumps(summary, indent=2))
            return 1

    summary["done_utc"] = datetime.now(timezone.utc).isoformat()
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
