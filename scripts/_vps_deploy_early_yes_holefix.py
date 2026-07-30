#!/usr/bin/env python3
"""Deploy early YES hole-fix on top of session-align (keep B′ + session_from_obs)."""
from __future__ import annotations

import json
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from remote_deploy import BUILD_VPS, KILL_STALE_BUILD, PROJ, run  # noqa: E402
from vps_shadow_autorun import connect, finalize_shadow_deploy, ro  # noqa: E402

MARKER = "bot-early-yes-holefix-sessionalign-20260730"
UPLOAD = [
    "trading-core/src/signals/LegInHedgeDetector.cpp",
    ".env.example",
]

ENV_KEYS = {
    "LIH_MM2_EARLY_YES_GUARD": "true",
    "LIH_MM2_EARLY_TILT_MIN_SPREAD": "0.35",
    "LIH_MM2_EARLY_TILT_MIN_FAV": "0.65",
    "LIH_MM2_SESSION_FROM_OBS": "true",
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
        sftp = c.open_sftp()
        for rel in UPLOAD:
            local = ROOT / rel
            remote = f"{PROJ}/{rel}"
            parent = str(Path(remote).parent).replace("\\", "/")
            ro(c, f"mkdir -p '{parent}'", t=15)
            sftp.put(str(local), remote)
            print("put", rel)
        sftp.close()

        for k, v in ENV_KEYS.items():
            upsert_env(c, k, v)

        print("=== rebuild ===")
        run(c, KILL_STALE_BUILD, timeout=30)
        rc = run(c, BUILD_VPS, timeout=1500)
        ok = ro(c, f"test -x '{PROJ}/build/trading-core' && echo OK || echo FAIL", t=30)
        print(f"build rc={rc} ok={ok.strip()}")
        if ok.strip() != "OK":
            return 1

        info = finalize_shadow_deploy(
            c,
            marker=MARKER,
            reason="early YES hole-fix: YES must be favorite + yes>=0.65 + spread>=0.35",
            restart=True,
            require_resume=True,
            resume_timeout_sec=150.0,
        )
        print(json.dumps(info, indent=2))
        if not info.get("resumed"):
            return 1

        time.sleep(8)
        hit = ro(
            c,
            f"grep -aE 'early yes guard|mm2 session off' '{PROJ}/logs/lih_skip.log' 2>/dev/null | tail -8; "
            f"grep -E '^LIH_MM2_EARLY_YES_GUARD=|^LIH_MM2_SESSION_FROM_OBS=' '{PROJ}/.env'",
            t=30,
        )
        print(hit)
        print("OK", MARKER)
        return 0
    finally:
        c.close()


if __name__ == "__main__":
    raise SystemExit(main())
