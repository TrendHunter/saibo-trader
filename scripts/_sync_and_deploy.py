#!/usr/bin/env python3
"""Force-sync VPS to origin/main and run deploy_vps_full (SKIP_GIT)."""
from __future__ import annotations

import sys
from pathlib import Path

import paramiko

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from remote_deploy import HOST, KILL_STALE_BUILD, PROJ, USER, load_password, run  # noqa: E402


def main() -> int:
    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    client.connect(HOST, username=USER, password=load_password(), timeout=60)
    try:
        run(client, KILL_STALE_BUILD, timeout=60)
        sync = (
            f"cd '{PROJ}' && git fetch origin main && git reset --hard origin/main && "
            "git clean -fd "
            "-e .env -e web.env -e logs -e build -e .venv "
            "-e frontend/.next -e frontend/node_modules"
        )
        if run(client, sync, timeout=120) != 0:
            return 1
        deploy = (
            f"chmod +x '{PROJ}/scripts/deploy_vps_full.sh' '{PROJ}/build-lowmem.sh' "
            f"'{PROJ}/server_start_bot.sh' && "
            f"SKIP_GIT=1 WEB_MODE=full bash '{PROJ}/scripts/deploy_vps_full.sh'"
        )
        return run(client, deploy, timeout=3600)
    finally:
        client.close()


if __name__ == "__main__":
    raise SystemExit(main())
