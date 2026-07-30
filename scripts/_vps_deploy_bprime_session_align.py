#!/usr/bin/env python3
"""Deploy B′ with mm2 session_active gating (session alignment)."""
from __future__ import annotations

import json
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

from remote_deploy import PROJ, run  # noqa: E402
from vps_shadow_autorun import connect, finalize_shadow_deploy  # noqa: E402

MARKER = "bot-early-yes-guard-bprime-sessionalign-20260729"

# Code already deployed by the previous B′ run.
# This deploy only toggles session gating to align bot open windows with m2 session_active.
ENV_KEYS = {
    "LIH_MM2_SESSION_FROM_OBS": "true",
    # Keep B′ guard settings (re-upsert for safety).
    "LIH_MM2_EARLY_YES_GUARD": "true",
    "LIH_MM2_EARLY_TILT_MIN_SPREAD": "0.35",
    "LIH_MM2_EARLY_TILT_MIN_FAV": "0.65",
    "LIVE_LIH_DRY_RUN": "true",
}


def upsert_env(c, key: str, val: str) -> None:
    run(
        c,
        f"grep -q '^{key}=' '{PROJ}/.env' && "
        f"sed -i 's|^{key}=.*|{key}={val}|' '{PROJ}/.env' || "
        f"echo '{key}={val}' >> '{PROJ}/.env'",
        timeout=30,
    )


def main() -> int:
    c = connect()
    try:
        for k, v in ENV_KEYS.items():
            upsert_env(c, k, v)

        info = finalize_shadow_deploy(
            c,
            marker=MARKER,
            reason="B′ session alignment: gate mm2 session_active from obs pack",
            restart=True,
            require_resume=True,
            resume_timeout_sec=150.0,
        )
        print(json.dumps(info, indent=2))
        if not info.get("resumed"):
            return 1

        time.sleep(6)
        hit = run(
            c,
            f"grep -a 'mm2 session off' '{PROJ}/logs/lih_skip.log' 2>/dev/null | tail -5",
            timeout=20,
        )
        print(hit)
        print("OK", MARKER)
        return 0
    finally:
        c.close()


if __name__ == "__main__":
    raise SystemExit(main())

