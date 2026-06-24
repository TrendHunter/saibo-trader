#!/usr/bin/env python3
"""VPS bot-only: stop Next.js web, keep shadow/live bot running."""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

import paramiko

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from remote_deploy import HOST, PROJ, USER, load_password, run  # noqa: E402


def ro(c, cmd, t=60):
    _, o, e = c.exec_command(cmd, timeout=t)
    return (o.read() + e.read()).decode(errors="replace").strip()


def main() -> int:
    ap = argparse.ArgumentParser(description="Stop web on VPS; bot continues")
    ap.add_argument("--upload", action="store_true", help="Upload server_stop_web.sh first")
    args = ap.parse_args()

    c = paramiko.SSHClient()
    c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    c.connect(HOST, username=USER, password=load_password(), timeout=60)
    try:
        if args.upload:
            sftp = c.open_sftp()
            local = Path(__file__).resolve().parents[1] / "scripts" / "server_stop_web.sh"
            remote = f"{PROJ}/scripts/server_stop_web.sh"
            sftp.put(str(local), remote)
            sftp.chmod(remote, 0o755)
            sftp.close()

        run(c, f"bash '{PROJ}/scripts/server_stop_web.sh'", timeout=60)

        print("\n=== MEMORY ===")
        print(ro(c, "free -h | head -3"))

        print("\n=== PROCESSES ===")
        print(ro(c, "pgrep -af 'start_bot|trading-core|next-server' | grep -v pgrep || echo none"))

        print("\n=== BOT ===")
        print(ro(c, "curl -s http://127.0.0.1:8081/health"))
        print(
            ro(
                c,
                f"curl -s http://127.0.0.1:8081/api/config | {PROJ}/.venv/bin/python -c "
                "\"import sys,json; l=json.load(sys.stdin).get('live',{}); "
                "print('status', l.get('status'), 'reason', l.get('statusReason'), "
                "'dry', 'shadow' if l.get('liveLihDryRun') else 'live')\" 2>/dev/null || "
                f"curl -s http://127.0.0.1:8081/api/config | head -c 200",
            )
        )
        print(ro(c, f"test -f '{PROJ}/logs/STOP_TRADING' && echo PAUSED || echo RUNNING"))
        return 0
    finally:
        c.close()


if __name__ == "__main__":
    raise SystemExit(main())
