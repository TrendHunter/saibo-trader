#!/usr/bin/env python3
"""Push mm2 session JSON + latest observation pack to bot VPS (P4 session gate)."""
from __future__ import annotations

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

from mm2_session_refresh import latest_obs_day, refresh  # noqa: E402
from remote_deploy import PROJ, run  # noqa: E402
from vps_shadow_autorun import connect  # noqa: E402

SESSION_JSON = ROOT / "data" / "mm2_session_active.json"
M2_ROOT = ROOT / "m2"


def push_to_vps(pack_dir: Path | None = None) -> dict:
    doc = refresh()
    pack = pack_dir or latest_obs_day(M2_ROOT)
    if pack is None:
        raise FileNotFoundError("no m2/YYYY-MM-DD pack to push")

    c = connect()
    try:
        sftp = c.open_sftp()
        run(c, f"mkdir -p '{PROJ}/data' '{PROJ}/m2/{pack.name}'", timeout=30)
        sftp.put(str(SESSION_JSON), f"{PROJ}/data/mm2_session_active.json")
        for name in ("windows.jsonl", "_meta.json"):
            local_f = pack / name
            if local_f.is_file():
                sftp.put(str(local_f), f"{PROJ}/m2/{pack.name}/{name}")
        sftp.close()
        run(
            c,
            f"cd '{PROJ}' && .venv/bin/python scripts/mm2_session_refresh.py",
            timeout=60,
        )
    finally:
        c.close()

    return {"session": doc, "pack_pushed": pack.name}


def main() -> int:
    try:
        info = push_to_vps()
    except Exception as exc:
        print(f"push_mm2_session_to_vps: {exc}", file=sys.stderr)
        return 1
    print(json.dumps(info, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
