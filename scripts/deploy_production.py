#!/usr/bin/env python3
"""One-click production deploy: push local tree -> VPS bot + Web (bare-metal /opt/polymarket-bot).

Matches current production: git pull, build-lowmem.sh, server_start_bot.sh, server_start_web.sh.

Prerequisites:
  - Root `.deploy.local` with DEPLOY_SSH_PASSWORD="..."
  - Server repo at /opt/polymarket-bot (or run --setup once)
  - Committed changes pushed to origin/main (server pulls from GitHub)

Usage:
  python scripts/deploy_production.py                 # full deploy (bot + web build)
  python scripts/deploy_production.py --web-fast      # bot rebuild + web restart only
  python scripts/deploy_production.py --skip-build    # git pull + restart, no C++ compile
  python scripts/deploy_production.py --bot-only      # no web step
  python scripts/deploy_production.py --bot-only --skip-build  # lightweight VPS (~1GB RAM)
  python scripts/deploy_production.py --setup         # first-time clone + venv + build + web.env hint
"""
from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

import paramiko

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from remote_deploy import (  # noqa: E402
    BUILD_VPS,
    HOST,
    KILL_STALE_BUILD,
    PROJ,
    REPO,
    ROOT as RD_ROOT,
    USER,
    load_password,
    run,
)

# Scripts synced to server before each deploy (paths relative to repo root).
UPLOAD_FILES: list[tuple[str, str]] = [
    ("scripts/deploy_vps_full.sh", f"{PROJ}/scripts/deploy_vps_full.sh"),
    ("scripts/server_start_bot.sh", f"{PROJ}/server_start_bot.sh"),
    ("scripts/server_stop_web.sh", f"{PROJ}/scripts/server_stop_web.sh"),
    ("scripts/server_restart_web.sh", f"{PROJ}/server_restart_web.sh"),
    ("scripts/web_run.sh", f"{PROJ}/scripts/web_run.sh"),
    ("scripts/web_watchdog.sh", f"{PROJ}/scripts/web_watchdog.sh"),
    ("scripts/web_install_watchdog.sh", f"{PROJ}/scripts/web_install_watchdog.sh"),
    ("scripts/block_ip.sh", f"{PROJ}/scripts/block_ip.sh"),
    ("scripts/apply_ip_blacklist.sh", f"{PROJ}/scripts/apply_ip_blacklist.sh"),
    ("build-lowmem.sh", f"{PROJ}/build-lowmem.sh"),
    ("web.env.example", f"{PROJ}/web.env.example"),
]


def git_push_ahead() -> bool:
    """True if local main is ahead of origin/main (unpushed commits)."""
    try:
        subprocess.run(["git", "fetch", "origin", "main"], cwd=ROOT, capture_output=True, check=False)
        local = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
        remote = subprocess.check_output(["git", "rev-parse", "origin/main"], cwd=ROOT, text=True).strip()
        base = subprocess.check_output(
            ["git", "merge-base", local, "origin/main"], cwd=ROOT, text=True
        ).strip()
        return base == remote and local != remote
    except (subprocess.CalledProcessError, FileNotFoundError):
        return False


def upload_scripts(client: paramiko.SSHClient) -> None:
    sftp = client.open_sftp()
    try:
        try:
            sftp.stat(f"{PROJ}/scripts")
        except OSError:
            sftp.mkdir(f"{PROJ}/scripts")
        for local_rel, remote_path in UPLOAD_FILES:
            local = RD_ROOT / local_rel
            if not local.is_file():
                print(f"WARN: skip missing {local_rel}", file=sys.stderr)
                continue
            print(f"Upload {local_rel} -> {remote_path}", file=sys.stderr)
            sftp.put(str(local), remote_path)
    finally:
        sftp.close()


def run_setup(client: paramiko.SSHClient) -> int:
    upload_scripts(client)
    steps = [
        "command -v git && python3 --version",
        f"test -d '{PROJ}/.git' && echo exists || git clone --branch main '{REPO}' '{PROJ}'",
        f"cd '{PROJ}' && git pull origin main",
        f"test -f '{PROJ}/.env' || cp '{PROJ}/.env.example' '{PROJ}/.env'",
        "dnf install -y gcc gcc-c++ make git openssl-devel python3 python3-pip nodejs 2>/dev/null || "
        "(yum install -y gcc gcc-c++ make git openssl-devel python3 python3-pip 2>/dev/null || true)",
        "command -v node && node --version || "
        "(curl -fsSL https://rpm.nodesource.com/setup_20.x | bash - && dnf install -y nodejs)",
        f"cd '{PROJ}' && python3 -m venv .venv && .venv/bin/pip install -r requirements.txt",
        KILL_STALE_BUILD,
        BUILD_VPS,
        f"test -f '{PROJ}/web.env' || cp '{PROJ}/web.env.example' '{PROJ}/web.env'",
        f"grep -q '^NEXTAUTH_URL=' '{PROJ}/web.env' && "
        f"sed -i 's|^NEXTAUTH_URL=.*|NEXTAUTH_URL=http://{HOST}:3001|' '{PROJ}/web.env' || "
        f"echo 'NEXTAUTH_URL=http://{HOST}:3001' >> '{PROJ}/web.env'",
        f"chmod +x '{PROJ}/scripts/deploy_vps_full.sh' '{PROJ}/build-lowmem.sh' && "
        f"WEB_MODE=full bash '{PROJ}/scripts/deploy_vps_full.sh'",
        "firewall-cmd --permanent --add-port=3001/tcp 2>/dev/null || true",
        "firewall-cmd --reload 2>/dev/null || true",
        f"echo 'Edit {PROJ}/web.env AUTH_USERNAME/AUTH_PASSWORD then re-run deploy_production.py'",
    ]
    rc = 0
    for step in steps:
        r = run(client, step, timeout=3600)
        if r != 0 and ("git clone" in step or "build-lowmem" in step):
            return r
        if r != 0:
            rc = r
    return rc


def run_deploy(
    client: paramiko.SSHClient,
    *,
    web_mode: str,
    skip_build: bool,
    skip_git: bool,
    bot_only: bool,
) -> int:
    upload_scripts(client)
    env_parts = [
        f"WEB_MODE={'skip' if bot_only else web_mode}",
        f"SKIP_BUILD={'1' if skip_build else '0'}",
        f"SKIP_GIT={'1' if skip_git else '0'}",
    ]
    cmd = (
        f"chmod +x '{PROJ}/scripts/deploy_vps_full.sh' '{PROJ}/build-lowmem.sh' "
        f"'{PROJ}/server_start_bot.sh' '{PROJ}/server_start_web.sh' && "
        f"{ ' '.join(env_parts) } bash '{PROJ}/scripts/deploy_vps_full.sh'"
    )
    run(client, KILL_STALE_BUILD, timeout=60)
    rc = run(client, cmd, timeout=3600)
    run(client, "ss -tlnp | grep -E ':3001|:8080|:8081' || true", timeout=30)
    run(
        client,
        f"curl -s -o /dev/null -w 'web=%{{http_code}} bot=%{{http_code}}\\n' "
        f"http://127.0.0.1:3001/login http://127.0.0.1:8081/health",
        timeout=30,
    )
    print(f"\nDashboard: http://{HOST}:3001/login", file=sys.stderr)
    return rc


def main() -> int:
    ap = argparse.ArgumentParser(description="One-click VPS production deploy (bot + web)")
    ap.add_argument("--setup", action="store_true", help="First-time server bootstrap")
    ap.add_argument("--web-fast", action="store_true", help="Skip npm build; restart web only")
    ap.add_argument("--skip-build", action="store_true", help="Skip C++ rebuild")
    ap.add_argument("--skip-git", action="store_true", help="Skip git pull on server")
    ap.add_argument("--bot-only", action="store_true", help="Deploy bot only, no web")
    ap.add_argument("--force", action="store_true", help="Deploy even if local commits not pushed")
    args = ap.parse_args()

    if not args.force and not args.skip_git and git_push_ahead():
        print(
            "ERROR: local main is ahead of origin/main — push first, then deploy.\n"
            "  git push origin main\n"
            "  python scripts/deploy_production.py --force   # to skip this check",
            file=sys.stderr,
        )
        return 1

    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    client.connect(HOST, username=USER, password=load_password(), timeout=45)
    try:
        if args.setup:
            return run_setup(client)
        web_mode = "fast" if args.web_fast else "full"
        return run_deploy(
            client,
            web_mode=web_mode,
            skip_build=args.skip_build,
            skip_git=args.skip_git,
            bot_only=args.bot_only,
        )
    finally:
        client.close()


if __name__ == "__main__":
    raise SystemExit(main())
