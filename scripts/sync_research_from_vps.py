#!/usr/bin/env python3
"""Sync VPS research artifacts → local for offline analysis.

Pulls (no secrets written as-is except sanitized LIH_*):
  - logs/shadow_trades.jsonl, shadow_windows.jsonl, shadow_hourly.log, lih_skip.log
  - m2/m3/m4 day packs (latest N)
  - data/mm2_session_active.json
  - sanitized LIH_/DH_/RISK_ env keys
  - optional marker slice under data/pull_<utc>/

Usage:
  python scripts/sync_research_from_vps.py
  python scripts/sync_research_from_vps.py --latest 7 --marker bot-early-yes-holefix-sessionalign-20260730
  python scripts/sync_research_from_vps.py --no-marker-slice
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

from remote_deploy import PROJ  # noqa: E402
from vps_shadow_autorun import connect, ro  # noqa: E402

LOG_FILES = (
    "shadow_trades.jsonl",
    "shadow_windows.jsonl",
    "shadow_hourly.log",
    "lih_skip.log",
)
PACK_FILES = (
    "windows.jsonl",
    "fills.jsonl",
    "sessions.jsonl",
    "gaps.jsonl",
    "_meta.json",
)


def sftp_get(sftp, remote: str, local: Path) -> dict:
    local.parent.mkdir(parents=True, exist_ok=True)
    try:
        st = sftp.stat(remote)
    except OSError as e:
        return {"ok": False, "remote": remote, "error": str(e)}
    sftp.get(remote, str(local))
    return {"ok": True, "remote": remote, "local": str(local), "bytes": int(st.st_size)}


def pull_logs(sftp, out_raw: Path) -> list[dict]:
    got = []
    for name in LOG_FILES:
        got.append(sftp_get(sftp, f"{PROJ}/logs/{name}", out_raw / name))
        print(f"  log {name}: {'OK' if got[-1].get('ok') else 'skip'}")
    return got


def pull_session(sftp) -> dict:
    return sftp_get(
        sftp,
        f"{PROJ}/data/mm2_session_active.json",
        ROOT / "data" / "mm2_session_active.json",
    )


def pull_lih_env(sftp, out: Path) -> dict:
    try:
        with sftp.open(f"{PROJ}/.env", "r") as f:
            raw = f.read().decode("utf-8", "replace")
    except OSError as e:
        return {"ok": False, "error": str(e)}
    deny = ("KEY", "SECRET", "PRIVATE", "PASSWORD", "TOKEN", "PASSPHRASE", "MNEMONIC")
    lines = []
    for ln in raw.splitlines():
        s = ln.strip()
        if not s or s.startswith("#"):
            continue
        if not s.startswith(("LIH_", "DH_ENABLE_", "RISK_", "LIVE_LIH_", "AUTO_REDEEM")):
            continue
        up = s.upper()
        if any(d in up for d in deny):
            continue
        lines.append(s)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return {"ok": True, "path": str(out), "keys": len(lines)}


def list_remote_days(c, account: str) -> list[str]:
    raw = ro(c, f"ls -1 '{PROJ}/{account}/' 2>/dev/null || true", t=30)
    days = []
    for ln in raw.splitlines():
        name = ln.strip()
        if len(name) == 10 and name[4] == "-" and name[7] == "-":
            days.append(name)
    return sorted(days)


def pull_packs(c, sftp, accounts: list[str], latest: int) -> dict:
    out: dict = {}
    for acc in accounts:
        days = list_remote_days(c, acc)
        take = days[-latest:] if latest > 0 else days
        out[acc] = []
        for day in take:
            local_dir = ROOT / acc / day
            local_dir.mkdir(parents=True, exist_ok=True)
            files = []
            for name in PACK_FILES:
                r = sftp_get(sftp, f"{PROJ}/{acc}/{day}/{name}", local_dir / name)
                if r.get("ok"):
                    files.append(name)
            out[acc].append({"day": day, "files": files})
            print(f"  {acc}/{day}: {','.join(files) or 'empty'}")
    return out


def mirror_logs_to_repo_logs(raw: Path) -> None:
    """Also keep copies under logs/ for scripts that expect that path."""
    dest = ROOT / "logs"
    dest.mkdir(parents=True, exist_ok=True)
    for name in LOG_FILES:
        src = raw / name
        if src.exists():
            target = dest / name
            target.write_bytes(src.read_bytes())


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--latest", type=int, default=7, help="tracker days per account")
    ap.add_argument("--accounts", default="m2,m3,m4")
    ap.add_argument(
        "--marker",
        default="bot-early-yes-holefix-sessionalign-20260730",
        help="also run observation slice for this marker",
    )
    ap.add_argument("--no-marker-slice", action="store_true")
    ap.add_argument("--no-packs", action="store_true")
    ap.add_argument("--no-logs", action="store_true")
    args = ap.parse_args()

    stamp = datetime.now(timezone.utc).strftime("%Y%m%d")
    out_dir = ROOT / "data" / f"research_sync_{stamp}"
    raw = out_dir / "raw"
    raw.mkdir(parents=True, exist_ok=True)

    summary: dict = {
        "started_utc": datetime.now(timezone.utc).isoformat(),
        "out_dir": str(out_dir),
        "marker": args.marker,
    }

    # Ensure git code is current
    try:
        rc = subprocess.call(["git", "pull", "--ff-only", "origin", "main"], cwd=str(ROOT))
        summary["git_pull_rc"] = rc
    except Exception as e:  # noqa: BLE001
        summary["git_pull_error"] = str(e)

    c = connect()
    try:
        sftp = c.open_sftp()
        if not args.no_logs:
            print("=== shadow / skip logs ===")
            summary["logs"] = pull_logs(sftp, raw)
            mirror_logs_to_repo_logs(raw)
        print("=== session file ===")
        summary["session"] = pull_session(sftp)
        print("=== sanitized env ===")
        summary["lih_env"] = pull_lih_env(
            sftp, ROOT / "data" / "compare_triple" / "vps_lih_env_sanitized.txt"
        )
        if not args.no_packs:
            print("=== tracker packs ===")
            accounts = [a.strip() for a in args.accounts.split(",") if a.strip()]
            summary["packs"] = pull_packs(c, sftp, accounts, args.latest)
        sftp.close()
    finally:
        c.close()

    if not args.no_marker_slice and args.marker:
        print("=== marker observation slice ===")
        from datetime import timedelta

        today = datetime.now(timezone.utc).date()
        days = f"{today - timedelta(days=1)},{today}"
        slice_out = ROOT / "data" / f"pull_{stamp}_research"
        rc = subprocess.call(
            [
                sys.executable,
                str(ROOT / "scripts" / "_pull_vps_observation_data.py"),
                "--marker",
                args.marker,
                "--days",
                days,
                "--out",
                str(slice_out),
            ],
            cwd=str(ROOT),
        )
        summary["marker_slice_rc"] = rc
        summary["marker_slice_out"] = str(slice_out)

    summary["done_utc"] = datetime.now(timezone.utc).isoformat()
    man = out_dir / "_sync_manifest.json"
    man.write_text(json.dumps(summary, indent=2, ensure_ascii=False), encoding="utf-8")
    print(json.dumps(summary, indent=2, ensure_ascii=False))
    print(f"\nmanifest → {man}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
