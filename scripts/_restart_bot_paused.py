#!/usr/bin/env python3
from __future__ import annotations

import sys
from pathlib import Path

import paramiko

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from remote_deploy import HOST, PROJ, USER, load_password, run  # noqa: E402


def main() -> int:
    c = paramiko.SSHClient()
    c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    c.connect(HOST, username=USER, password=load_password(), timeout=60)
    try:
        run(c, f"touch '{PROJ}/logs/STOP_TRADING' && bash '{PROJ}/server_start_bot.sh'", timeout=90)
        run(c, "sleep 10", timeout=20)
        run(c, "pgrep -af 'start_bot|trading-core' | grep -v pgrep || echo DOWN", timeout=20)
        run(c, "curl -s http://127.0.0.1:8081/health", timeout=15)
        run(c, (
            f"curl -s http://127.0.0.1:8081/api/config | "
            f"{PROJ}/.venv/bin/python -c "
            "\"import sys,json; l=json.load(sys.stdin).get('live',{}); "
            "print('status', l.get('status'), 'reason', l.get('statusReason'))\""
        ), timeout=20)
    finally:
        c.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
