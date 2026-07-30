#!/usr/bin/env python3
"""One-off remote deploy helper. Reads .deploy.local (gitignored). Do not commit secrets."""

from __future__ import annotations

import os
import re
import sys
import time
from pathlib import Path

import paramiko

ROOT = Path(__file__).resolve().parents[1]
DEPLOY_LOCAL = ROOT / ".deploy.local"
HOST = "70.34.221.132"
USER = "root"
REPO = "https://github.com/TrendHunter/saibo-trader.git"
PROJ = "/opt/polymarket-bot"

# VPS ~1GB RAM: kill stale compiles, use build-lowmem.sh (no LTO, ninja -j1).
KILL_STALE_BUILD = (
    "pkill -9 -f build-lowmem.sh 2>/dev/null || true; "
    "pkill -9 -f build.sh 2>/dev/null || true; "
    "pkill -9 -f 'cmake --build' 2>/dev/null || true; "
    "pkill -9 -f ninja 2>/dev/null || true; "
    "pkill -9 -f lto1 2>/dev/null || true; "
    "pkill -9 -f cc1plus 2>/dev/null || true; "
    "sleep 1"
)
BUILD_VPS = f"cd '{PROJ}' && chmod +x build.sh build-lowmem.sh && bash build-lowmem.sh"
BUILD_LOCAL = f"cd '{PROJ}' && chmod +x build.sh && ./build.sh"

STRATEGY_SYNC_PREFIXES = (
    "RISK_",
    "LIH_",
    "DH_ENABLE_",
    "DH_BOOK_",
    "BINANCE_",
)
STRATEGY_SYNC_EXACT = ("MIN_ORDER_SIZE", "FEE_RATE", "LIVE_MIRROR_PATH")
SERVER_ENV_PROTECT = (
    "POLYMARKET_PRIVATE_KEY",
    "POLYMARKET_FUNDER",
    "POLYMARKET_SIGNER",
    "POLYMARKET_CHAIN_ID",
    "POLYMARKET_HOST",
    "POLY_API_KEY",
    "POLY_API_SECRET",
    "POLY_API_PASSPHRASE",
    "POLYMARKET_CLOB_API_KEY",
    "POLYMARKET_CLOB_SECRET",
    "POLYMARKET_CLOB_PASSPHRASE",
)
SERVER_ENV_FORCE = {
    "LIVE_LIH_DRY_RUN": "true",
    "LIVE_DH_DRY_RUN": "true",
}


def parse_env_file(path: Path) -> dict[str, str]:
    out: dict[str, str] = {}
    if not path.is_file():
        return out
    for line in path.read_text(encoding="utf-8").splitlines():
        s = line.strip()
        if not s or s.startswith("#") or "=" not in s:
            continue
        k, v = s.split("=", 1)
        out[k.strip()] = v.strip()
    return out


def strategy_keys_from_local(local: dict[str, str]) -> dict[str, str]:
    picked: dict[str, str] = {}
    for k, v in local.items():
        if k in SERVER_ENV_PROTECT:
            continue
        if k in STRATEGY_SYNC_EXACT or any(k.startswith(p) for p in STRATEGY_SYNC_PREFIXES):
            picked[k] = v
    picked.update(SERVER_ENV_FORCE)
    return picked


def patch_env_remote_cmds(remote_env: str, updates: dict[str, str]) -> list[str]:
    cmds: list[str] = []
    for k, v in sorted(updates.items()):
        esc = v.replace("|", "\\|")
        cmds.append(
            f"grep -q '^{k}=' '{remote_env}' && "
            f"sed -i 's|^{k}=.*|{k}={esc}|' '{remote_env}' || "
            f"echo '{k}={esc}' >> '{remote_env}'"
        )
    return cmds


def load_password() -> str:
    if not DEPLOY_LOCAL.is_file():
        raise SystemExit(f"Missing {DEPLOY_LOCAL}")
    text = DEPLOY_LOCAL.read_text(encoding="utf-8")
    m = re.search(r'DEPLOY_SSH_PASSWORD\s*=\s*["\']([^"\']+)["\']', text)
    if m:
        return m.group(1)
    return text.strip()


def sftp_put_tree(sftp: paramiko.SFTPClient, local_dir: Path, remote_dir: str) -> None:
    """Upload directory tree, preserving relative paths."""
    local_dir = local_dir.resolve()
    for path in local_dir.rglob("*"):
        if not path.is_file():
            continue
        rel = path.relative_to(local_dir).as_posix()
        remote_path = f"{remote_dir.rstrip('/')}/{rel}"
        remote_parent = remote_path.rsplit("/", 1)[0]
        parts: list[str] = []
        for part in remote_parent.split("/"):
            if not part:
                continue
            parts.append(part)
            seg = "/" + "/".join(parts)
            try:
                sftp.stat(seg)
            except OSError:
                try:
                    sftp.mkdir(seg)
                except OSError:
                    pass
        print(f"Upload {path.relative_to(ROOT)} -> {remote_path}", file=sys.stderr)
        sftp.put(str(path), remote_path)


def run(client: paramiko.SSHClient, cmd: str, timeout: int = 600) -> int:
    print(f"\n>>> {cmd}\n")
    stdin, stdout, stderr = client.exec_command(cmd, timeout=timeout, get_pty=True)
    channel = stdout.channel
    buf_out: list[str] = []
    buf_err: list[str] = []
    deadline = time.monotonic() + timeout
    while True:
        if channel.recv_ready():
            buf_out.append(channel.recv(65535).decode(errors="replace"))
        if channel.recv_stderr_ready():
            buf_err.append(channel.recv_stderr(65535).decode(errors="replace"))
        if channel.exit_status_ready():
            while channel.recv_ready():
                buf_out.append(channel.recv(65535).decode(errors="replace"))
            while channel.recv_stderr_ready():
                buf_err.append(channel.recv_stderr(65535).decode(errors="replace"))
            break
        if time.monotonic() > deadline:
            print("WARN: command still running on server (local read timeout)", file=sys.stderr)
            return -1
        time.sleep(0.5)
    out = "".join(buf_out)
    err = "".join(buf_err)
    if out:
        print(out, end="" if out.endswith("\n") else "\n")
    if err:
        print(err, end="" if err.endswith("\n") else "\n", file=sys.stderr)
    return channel.recv_exit_status()


def main() -> int:
    mode = sys.argv[1] if len(sys.argv) > 1 else "probe"
    mode = {
        "deploy-lih-paper": "deploy-lih-shadow",
        "server-paper": "server-shadow",
        "production": "deploy-full",
    }.get(mode, mode)

    if mode == "deploy-full":
        import subprocess

        cmd = [sys.executable, str(ROOT / "scripts/deploy_production.py"), *sys.argv[2:]]
        return subprocess.call(cmd)

    pw = load_password()
    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    client.connect(HOST, username=USER, password=pw, timeout=30)

    try:
        if mode == "probe":
            for cmd in [
                "hostname; uname -a",
                "ls -la /opt/polycopy 2>/dev/null || ls -la /opt",
                "cd /opt/polycopy 2>/dev/null && git log -1 --oneline && git remote -v | head -2 || echo no_git",
                "cd /opt/polycopy 2>/dev/null && ls -la start_bot.py dashboard_bridge.py build/trading-core 2>&1 | head -5",
                "pgrep -af 'start_bot|dashboard_bridge|trading-core|python' || true",
                "find /opt /root /home -maxdepth 5 \\( -name start_bot.py -o -name trading-core -o -name DumpHedgeDetector.cpp \\) 2>/dev/null | head -20",
                "ls -la /opt/polymarket-bot 2>/dev/null || echo no_polymarket_bot",
                "docker ps -a --format 'table {{.Names}}\t{{.Status}}\t{{.Image}}' 2>/dev/null || true",
                "ss -tlnp | grep -E ':3001|:8080|:8081' || true",
                "curl -s -o /dev/null -w 'bot_api=%{http_code}\n' http://127.0.0.1:8081/health 2>/dev/null || true",
                "curl -s -o /dev/null -w 'web=%{http_code}\n' http://127.0.0.1:3001/login 2>/dev/null || true",
                f"tail -n 8 '{PROJ}/logs/frontend.log' 2>/dev/null || true",
            ]:
                run(client, cmd, timeout=60)
            return 0

        if mode == "setup":
            steps = [
                "command -v git && python3 --version",
                f"test -d '{PROJ}/.git' && echo exists || git clone --branch main '{REPO}' '{PROJ}'",
                f"cd '{PROJ}' && git pull origin main",
                f"test -f '{PROJ}/.env' || cp '{PROJ}/.env.example' '{PROJ}/.env'",
                "dnf install -y gcc gcc-c++ make git openssl-devel python3 python3-pip 2>/dev/null || "
                "yum install -y gcc gcc-c++ make git openssl-devel python3 python3-pip",
                BUILD_VPS,
                f"cd '{PROJ}' && python3 -m venv .venv && .venv/bin/pip install -r requirements.txt",
                "pkill -f 'start_bot.py' || true; pkill -f 'dashboard_bridge.py' || true; pkill -f 'trading-core' || true; sleep 2",
                f"cd '{PROJ}' && mkdir -p logs && nohup .venv/bin/python start_bot.py >> logs/bridge.log 2>&1 &",
                "sleep 8",
                "pgrep -af 'start_bot|trading-core' || true",
                f"tail -n 25 '{PROJ}/logs/bot.log' 2>/dev/null || tail -n 25 '{PROJ}/logs/bridge.log' 2>/dev/null || true",
            ]
            rc = 0
            for step in steps:
                r = run(client, step, timeout=1800)
                if r != 0 and ("git clone" in step or "build.sh" in step or "build-lowmem" in step):
                    return r
                if r != 0:
                    rc = r
            return rc

        if mode == "start":
            start_sh = ROOT / "scripts" / "server_start_bot.sh"
            remote_sh = f"{PROJ}/server_start_bot.sh"
            sftp = client.open_sftp()
            sftp.put(str(start_sh), remote_sh)
            sftp.close()
            steps = [
                f"ls -la '{PROJ}/build/trading-core'",
                f"cd '{PROJ}' && .venv/bin/pip install -q -r requirements.txt",
                f"chmod +x '{remote_sh}' && bash '{remote_sh}'",
                "sleep 8",
                "pgrep -af 'start_bot|trading-core' || true",
                f"tail -n 40 '{PROJ}/logs/bridge.log' 2>/dev/null || true",
                f"tail -n 40 '{PROJ}/logs/bot.log' 2>/dev/null || true",
            ]
            for step in steps:
                run(client, step, timeout=180)
            return 0

        if mode == "disable-mongo":
            script = ROOT / "scripts" / "server_disable_mongo.sh"
            remote = f"{PROJ}/server_disable_mongo.sh"
            sftp = client.open_sftp()
            sftp.put(str(script), remote)
            sftp.close()
            run(client, f"chmod +x '{remote}' && bash '{remote}'", timeout=120)
            return 0

        if mode == "cleanup":
            cleanup_sh = ROOT / "scripts" / "server_disk_cleanup.sh"
            remote = f"{PROJ}/server_disk_cleanup.sh"
            purge = os.environ.get("PURGE_MONGO_DATA", "0")
            sftp = client.open_sftp()
            sftp.put(str(cleanup_sh), remote)
            sftp.close()
            steps = [
                f"chmod +x '{remote}'",
                f"PURGE_MONGO_DATA={purge} bash '{remote}'",
                "du -sh /var/log/mongodb /var/lib/mongo /opt/polycopy/backups 2>/dev/null || true",
            ]
            for step in steps:
                run(client, step, timeout=300)
            return 0

        if mode == "server-shadow":
            # Revert server to shadow only (does NOT touch local .env).
            bot_sh = ROOT / "scripts" / "server_start_bot.sh"
            remote_bot = f"{PROJ}/server_start_bot.sh"
            sftp = client.open_sftp()
            sftp.put(str(bot_sh), remote_bot)
            sftp.close()
            steps = [
                f"grep -q '^LIVE_LIH_DRY_RUN=' '{PROJ}/.env' && "
                f"sed -i 's/^LIVE_LIH_DRY_RUN=.*/LIVE_LIH_DRY_RUN=true/' '{PROJ}/.env' || "
                f"echo 'LIVE_LIH_DRY_RUN=true' >> '{PROJ}/.env'",
                f"grep -E '^(RISK_MAX)' '{PROJ}/.env' | head -5",
                f"chmod +x '{remote_bot}' && bash '{remote_bot}'",
                "sleep 8",
                "pgrep -af 'start_bot|trading-core' || true",
                f"tail -n 12 '{PROJ}/logs/bridge.log' 2>/dev/null || true",
            ]
            for step in steps:
                run(client, step, timeout=180)
            return 0

        if mode == "go-live-small":
            # Server-only small live: patch server .env (does NOT upload local .env).
            patch_py = f"""import shutil
from pathlib import Path
P = Path("{PROJ}/.env")
updates = {{
        "LIH_ENABLED": "true",
    "RISK_MAX_POSITION_FRACTION": "0.20",
    "RISK_MAX_CONCURRENT_POSITIONS": "1",
    "MIN_ORDER_SIZE": "5.0",
    "LIH_LEG1_SHARES": "10",
    "LIH_MAX_MATCHED_SHARES": "50",
    "DH_ENABLE_5M": "true",
    "DH_ENABLE_15M": "false",
    "DH_ENABLE_5M_BTC": "true",
    "DH_ENABLE_5M_ETH": "false",
    "DH_ENABLE_5M_SOL": "false",
    "DH_ENABLE_15M_BTC": "false",
    "DH_ENABLE_15M_ETH": "false",
    "AUTO_REDEEM": "true",
    "LIVE_LIH_DRY_RUN": "true",
    "LIVE_STARTING_BALANCE": "21.077149",
}}
shutil.copy2(P, str(P) + ".pre-live-small.bak")
lines = P.read_text(encoding="utf-8").splitlines()
done = set()
out = []
for line in lines:
    s = line.strip()
    if s and not s.startswith("#") and "=" in line:
        k = line.split("=", 1)[0].strip()
        if k in updates:
            out.append(f"{{k}}={{updates[k]}}")
            done.add(k)
            continue
    out.append(line)
for k, v in updates.items():
    if k not in done:
        out.append(f"{{k}}={{v}}")
P.write_text("\\n".join(out) + "\\n", encoding="utf-8")
print("patched", len(updates), "keys")
"""
            bot_sh = ROOT / "scripts" / "server_start_bot.sh"
            remote_bot = f"{PROJ}/server_start_bot.sh"
            sftp = client.open_sftp()
            sftp.put(str(bot_sh), remote_bot)
            with sftp.file(f"{PROJ}/_patch_live_small.py", "w") as f:
                f.write(patch_py)
            sftp.close()
            steps = [
                f"test -f '{PROJ}/logs/live_state.json' && "
                f"cp '{PROJ}/logs/live_state.json' '{PROJ}/logs/live_state.json.pre-live.bak' || true",
                f"cd '{PROJ}' && .venv/bin/python _patch_live_small.py",
                f"grep -E '^(LIH_ENABLED|RISK_|MIN_ORDER|DH_ENABLE|AUTO_REDEEM)' '{PROJ}/.env'",
                f"cd '{PROJ}' && .venv/bin/python derive_and_update_keys.py",
                f"grep -E '^POLY_API' '{PROJ}/.env' | sed "
                "'s/POLY_API_SECRET=.*/POLY_API_SECRET=***MASKED***/;"
                "s/POLY_PASSPHRASE=.*/POLY_PASSPHRASE=***MASKED***/;"
                "s/POLY_API_KEY=.*/POLY_API_KEY=***SET***/'",
                f"cd '{PROJ}' && .venv/bin/python fetch_balance.py",
                f"cd '{PROJ}' && .venv/bin/python start_bot.py --preflight-only",
                f"chmod +x '{remote_bot}' && bash '{remote_bot}'",
                "sleep 10",
                "pgrep -af 'start_bot|trading-core' || true",
                f"tail -n 30 '{PROJ}/logs/bot.log' 2>/dev/null || tail -n 30 '{PROJ}/logs/bridge.log'",
            ]
            rc = 0
            for step in steps:
                r = run(client, step, timeout=300)
                if r != 0 and "derive_and_update_keys" in step:
                    rc = r
                if r != 0 and "preflight-only" in step:
                    return r
            return rc

        if mode == "shadow-dh":
            # Pull latest, enable LIVE_DH_DRY_RUN, rebuild core, restart (no real orders).
            bot_sh = ROOT / "scripts" / "server_start_bot.sh"
            remote_bot = f"{PROJ}/server_start_bot.sh"
            sftp = client.open_sftp()
            sftp.put(str(bot_sh), remote_bot)
            sftp.close()
            steps = [
                f"cd '{PROJ}' && git stash push -m deploy-sync || true",
                f"cd '{PROJ}' && git pull origin main",
                f"grep -q '^LIVE_DH_DRY_RUN=' '{PROJ}/.env' && "
                f"sed -i 's/^LIVE_DH_DRY_RUN=.*/LIVE_DH_DRY_RUN=true/' '{PROJ}/.env' || "
                f"echo 'LIVE_DH_DRY_RUN=true' >> '{PROJ}/.env'",
                f"grep -E '^(LIVE_DH_DRY_RUN|RISK_MAX)' '{PROJ}/.env' | head -6",
                BUILD_VPS,
                f"chmod +x '{remote_bot}' && bash '{remote_bot}'",
                "sleep 10",
                "pgrep -af 'start_bot|trading-core' || true",
                f"grep -E 'LIVE DH|SHADOW|Dry-run|Mode: LIVE' '{PROJ}/bot.log' | tail -20",
            ]
            rc = 0
            for step in steps:
                r = run(client, step, timeout=3600)
                if r != 0 and "git pull" in step:
                    rc = r
                if r != 0 and ("build.sh" in step or "build-lowmem" in step):
                    return r
            return rc

        if mode == "shadow-dh-upload":
            # Upload changed C++ sources only — no git pull/push. LIVE_DH_DRY_RUN stays on.
            upload_files = [
                "trading-core/CMakeLists.txt",
                "trading-core/src/state/StateStore.h",
                "trading-core/src/state/StateStore.cpp",
                "trading-core/src/feeds/PolymarketFeed.cpp",
                "trading-core/src/feeds/GammaClient.h",
                "trading-core/src/feeds/GammaClient.cpp",
                "trading-core/src/signals/DumpHedgeDetector.h",
                "trading-core/src/signals/DumpHedgeDetector.cpp",
                "trading-core/src/signals/LegInHedgeDetector.h",
                "trading-core/src/signals/LegInHedgeDetector.cpp",
                "trading-core/src/exec/OrderRouter.h",
                "trading-core/src/exec/OrderRouter.cpp",
                "trading-core/src/risk/RiskManager.h",
                "trading-core/src/risk/RiskManager.cpp",
                "trading-core/src/main.cpp",
            ]
            bot_sh = ROOT / "scripts" / "server_start_bot.sh"
            remote_bot = f"{PROJ}/server_start_bot.sh"
            sftp = client.open_sftp()
            for rel in upload_files:
                local = ROOT / rel
                remote = f"{PROJ}/{rel.replace(chr(92), '/')}"
                print(f"Upload {rel} -> {remote}", file=sys.stderr)
                sftp.put(str(local), remote)
            sftp.put(str(bot_sh), remote_bot)
            sftp.close()
            env_patch = (
                f"grep -q '^LIVE_DH_DRY_RUN=' '{PROJ}/.env' && "
                f"sed -i 's/^LIVE_DH_DRY_RUN=.*/LIVE_DH_DRY_RUN=true/' '{PROJ}/.env' || "
                f"echo 'LIVE_DH_DRY_RUN=true' >> '{PROJ}/.env'; "
                f"grep -q '^DH_BOOK_AWARE_DETECT=' '{PROJ}/.env' && "
                f"sed -i 's/^DH_BOOK_AWARE_DETECT=.*/DH_BOOK_AWARE_DETECT=true/' '{PROJ}/.env' || "
                f"echo 'DH_BOOK_AWARE_DETECT=true' >> '{PROJ}/.env'"
            )
            steps = [
                f"cp '{PROJ}/build/trading-core' '{PROJ}/build/trading-core.bak-$(date +%s)' 2>/dev/null || true",
                env_patch,
                f"grep -E '^(LIVE_DH_DRY_RUN|DH_BOOK_AWARE_DETECT)' '{PROJ}/.env' | head -6",
                BUILD_VPS,
                f"chmod +x '{remote_bot}' && bash '{remote_bot}'",
                "sleep 10",
                "pgrep -af 'start_bot|trading-core' || true",
                f"grep -E 'Book-aware|LIVE DH|Dry-run|DUMP-HEDGE DETECTED' '{PROJ}/bot.log' | tail -25",
            ]
            rc = 0
            for step in steps:
                r = run(client, step, timeout=3600)
                if r != 0 and ("build.sh" in step or "build-lowmem" in step):
                    return r
            return rc

        if mode in ("shadow-lih-upload", "shadow-lih", "deploy-lih-shadow"):
            # Upload LIH-primary stack.
            upload_files = [
                "trading-core/CMakeLists.txt",
                "trading-core/conanfile.txt",
                "trading-core/src/state/StateStore.h",
                "trading-core/src/state/StateStore.cpp",
                "trading-core/src/state/PaperStateStore.cpp",
                "trading-core/src/feeds/PolymarketFeed.cpp",
                "trading-core/src/feeds/GammaClient.h",
                "trading-core/src/feeds/GammaClient.cpp",
                "trading-core/src/signals/LegInHedgeDetector.h",
                "trading-core/src/signals/LegInHedgeDetector.cpp",
                "trading-core/src/exec/OrderRouter.h",
                "trading-core/src/exec/OrderRouter.cpp",
                "trading-core/src/risk/RiskManager.h",
                "trading-core/src/risk/RiskManager.cpp",
                "trading-core/src/main.cpp",
                "trading-core/src/telemetry/ShadowWindowRecorder.h",
                "trading-core/src/telemetry/ShadowWindowRecorder.cpp",
                "bot_config.py",
                "dashboard_bridge.py",
                "start_bot.py",
                "live_preflight.py",
                "status_bot.py",
                "cli_dashboard.py",
                "requirements.txt",
            ]
            upload_dirs: list[tuple[str, str]] = []
            if mode == "deploy-lih-shadow":
                upload_dirs = [
                    ("frontend/src", f"{PROJ}/frontend/src"),
                ]
            bot_sh = ROOT / "scripts" / "server_start_bot.sh"
            remote_bot = f"{PROJ}/server_start_bot.sh"
            sftp = client.open_sftp()
            for rel in upload_files:
                local = ROOT / rel
                remote = f"{PROJ}/{rel.replace(chr(92), '/')}"
                print(f"Upload {rel} -> {remote}", file=sys.stderr)
                sftp.put(str(local), remote)
            sftp.put(str(bot_sh), remote_bot)
            for local_rel, remote_dir in upload_dirs:
                sftp_put_tree(sftp, ROOT / local_rel, remote_dir)
            if mode == "deploy-lih-shadow":
                web_sh = ROOT / "scripts" / "server_start_web.sh"
                restart_sh = ROOT / "scripts" / "server_restart_web.sh"
                sftp.put(str(web_sh), f"{PROJ}/server_start_web.sh")
                sftp.put(str(restart_sh), f"{PROJ}/server_restart_web.sh")
            sftp.close()
            env_keys = {
                "LIH_ENABLED": "true",
                                "LIVE_LIH_DRY_RUN": "true",
                "LIVE_DH_DRY_RUN": "true",
            }
            env_patch_lines = []
            for k, v in env_keys.items():
                env_patch_lines.append(
                    f"grep -q '^{k}=' '{PROJ}/.env' && "
                    f"sed -i 's/^{k}=.*/{k}={v}/' '{PROJ}/.env' || "
                    f"echo '{k}={v}' >> '{PROJ}/.env'"
                )
            env_patch = "; ".join(env_patch_lines)
            steps = [
                f"cp '{PROJ}/build/trading-core' '{PROJ}/build/trading-core.bak-$(date +%s)' 2>/dev/null || true",
                env_patch,
                f"grep -E '^(LIH_ENABLED|LIVE_LIH|LIVE_DH|RISK_MAX)' '{PROJ}/.env' | head -10",
                f"cd '{PROJ}' && .venv/bin/pip install -q -r requirements.txt",
                BUILD_VPS,
                f"chmod +x '{remote_bot}' && bash '{remote_bot}'",
                "sleep 10",
                "pgrep -af 'start_bot|trading-core' || true",
                f"grep -E 'LIH|Leg-In|leg_in|LIH dry-run|SHADOW' '{PROJ}/logs/bridge.log' 2>/dev/null | tail -20 || "
                f"tail -20 '{PROJ}/logs/bridge.log' 2>/dev/null || true",
            ]
            if mode == "deploy-lih-shadow":
                steps.extend([
                    f"export NEXTAUTH_URL=http://{HOST}:3001 && "
                    f"chmod +x '{PROJ}/server_start_web.sh' && bash '{PROJ}/server_start_web.sh'",
                    "sleep 8",
                    f"curl -s -o /dev/null -w 'web=%{{http_code}}\\n' http://127.0.0.1:3001/login",
                    f"curl -s -o /dev/null -w 'bot_api=%{{http_code}}\\n' http://127.0.0.1:8081/health",
                ])
            rc = 0
            for step in steps:
                r = run(client, step, timeout=3600)
                if r != 0 and ("build.sh" in step or "build-lowmem" in step):
                    return r
            return rc

        if mode == "sync-env":
            local_env = ROOT / ".env"
            if not local_env.is_file():
                print(f"ERROR: missing {local_env}", file=sys.stderr)
                return 1
            remote_env = f"{PROJ}/.env"
            sftp = client.open_sftp()
            try:
                sftp.rename(remote_env, f"{remote_env}.bak")
            except OSError:
                pass
            sftp.put(str(local_env), remote_env)
            sftp.close()
            keys = [
                line.split("=", 1)[0].strip()
                for line in local_env.read_text(encoding="utf-8").splitlines()
                if line.strip() and not line.strip().startswith("#") and "=" in line
            ]
            print(f"Uploaded .env ({len(keys)} keys)", file=sys.stderr)
            bot_sh = ROOT / "scripts" / "server_start_bot.sh"
            remote_bot = f"{PROJ}/server_start_bot.sh"
            sftp = client.open_sftp()
            sftp.put(str(bot_sh), remote_bot)
            sftp.close()
            run(client, f"chmod +x '{remote_bot}' && bash '{remote_bot}'", timeout=120)
            run(
                client,
                f"grep -E '^(LIH_ENABLED|LIH_|DH_SUM|RISK_)' '{remote_env}' | head -12",
                timeout=30,
            )
            run(client, "pgrep -af 'start_bot|trading-core' || true", timeout=15)
            return 0

        if mode == "sync-strategy-env":
            local_env = ROOT / ".env"
            if not local_env.is_file():
                print(f"ERROR: missing {local_env}", file=sys.stderr)
                return 1
            updates = strategy_keys_from_local(parse_env_file(local_env))
            remote_env = f"{PROJ}/.env"
            bot_sh = ROOT / "scripts" / "server_start_bot.sh"
            remote_bot = f"{PROJ}/server_start_bot.sh"
            sftp = client.open_sftp()
            sftp.put(str(bot_sh), remote_bot)
            sftp.close()
            for step in patch_env_remote_cmds(remote_env, updates):
                run(client, step, timeout=30)
            run(
                client,
                f"grep -E '^(LIH_|RISK_MAX|LIVE_LIH)' '{remote_env}' | head -20",
                timeout=30,
            )
            run(client, f"chmod +x '{remote_bot}' && bash '{remote_bot}'", timeout=120)
            run(client, "pgrep -af 'start_bot|trading-core' || true", timeout=15)
            print(f"Synced {len(updates)} strategy keys from local .env", file=sys.stderr)
            return 0

        if mode == "fix-bot-py":
            bot_files = [
                ("bot_config.py", f"{PROJ}/bot_config.py"),
                ("dashboard_bridge.py", f"{PROJ}/dashboard_bridge.py"),
            ]
            sftp = client.open_sftp()
            for local_rel, remote_path in bot_files:
                print(f"Upload {local_rel} -> {remote_path}", file=sys.stderr)
                sftp.put(str(ROOT / local_rel), remote_path)
            sftp.close()
            run(client, f"bash '{PROJ}/server_start_bot.sh'", timeout=120)
            run(
                client,
                f"cd '{PROJ}' && python3 -c \"from bot_config import update_env; update_env({{'RISK_MAX_POSITION_FRACTION': '0.2'}}); print('bot_config ok')\"",
                timeout=30,
            )
            run(client, "pgrep -af 'start_bot|trading-core' || true", timeout=15)
            return 0

        if mode == "fix-web-pages":
            page_files = [
                ("frontend/src/app/strategies/page.tsx", f"{PROJ}/frontend/src/app/strategies/page.tsx"),
                ("frontend/src/app/risk/page.tsx", f"{PROJ}/frontend/src/app/risk/page.tsx"),
            ]
            sftp = client.open_sftp()
            for local_rel, remote_path in page_files:
                print(f"Upload {local_rel} -> {remote_path}", file=sys.stderr)
                sftp.put(str(ROOT / local_rel), remote_path)
            sftp.close()
            run(
                client,
                f"cd '{PROJ}/frontend' && npm run build && pkill -f next-server 2>/dev/null || true; "
                f"sleep 2; setsid -f -- npm run start >> '{PROJ}/logs/frontend.log' 2>&1",
                timeout=1800,
            )
            return 0

        if mode == "set-auth":
            import secrets

            user = sys.argv[2] if len(sys.argv) > 2 else "zhan"
            password = sys.argv[3] if len(sys.argv) > 3 else "qilai1314"
            secret = secrets.token_hex(32)
            web_env = (
                f"AUTH_USERNAME={user}\n"
                f"AUTH_PASSWORD={password}\n"
                f"NEXTAUTH_URL=http://{HOST}:3001\n"
                f"NEXTAUTH_SECRET={secret}\n"
                f"AUTH_TRUST_HOST=true\n"
            )
            auth_files = [
                ("frontend/src/lib/inputSecurity.ts", f"{PROJ}/frontend/src/lib/inputSecurity.ts"),
                ("frontend/src/lib/ipGuard.ts", f"{PROJ}/frontend/src/lib/ipGuard.ts"),
                (
                    "frontend/src/app/api/auth/[...nextauth]/route.ts",
                    f"{PROJ}/frontend/src/app/api/auth/[...nextauth]/route.ts",
                ),
                ("frontend/src/app/api/auth/register/route.ts", f"{PROJ}/frontend/src/app/api/auth/register/route.ts"),
                (
                    "frontend/src/app/api/auth/forgot-password/route.ts",
                    f"{PROJ}/frontend/src/app/api/auth/forgot-password/route.ts",
                ),
                (
                    "frontend/src/app/api/auth/reset-password/route.ts",
                    f"{PROJ}/frontend/src/app/api/auth/reset-password/route.ts",
                ),
                ("frontend/src/app/api/bot/config/route.ts", f"{PROJ}/frontend/src/app/api/bot/config/route.ts"),
                ("frontend/prisma/seed.ts", f"{PROJ}/frontend/prisma/seed.ts"),
                ("frontend/src/app/login/page.tsx", f"{PROJ}/frontend/src/app/login/page.tsx"),
                ("frontend/src/proxy.ts", f"{PROJ}/frontend/src/proxy.ts"),
                ("scripts/server_start_web.sh", f"{PROJ}/server_start_web.sh"),
                ("scripts/server_restart_web.sh", f"{PROJ}/server_restart_web.sh"),
                ("scripts/block_ip.sh", f"{PROJ}/scripts/block_ip.sh"),
                ("scripts/apply_ip_blacklist.sh", f"{PROJ}/scripts/apply_ip_blacklist.sh"),
            ]
            sftp = client.open_sftp()
            for local_rel, remote_path in auth_files:
                local_path = ROOT / local_rel
                remote_parent = remote_path.rsplit("/", 1)[0]
                parts: list[str] = []
                for part in remote_parent.split("/"):
                    if not part:
                        continue
                    parts.append(part)
                    seg = "/" + "/".join(parts)
                    try:
                        sftp.stat(seg)
                    except OSError:
                        try:
                            sftp.mkdir(seg)
                        except OSError:
                            pass
                print(f"Upload {local_rel} -> {remote_path}", file=sys.stderr)
                sftp.put(str(local_path), remote_path)
            with sftp.file(f"{PROJ}/web.env", "w") as f:
                f.write(web_env)
            sftp.close()
            run(client, f"rm -f '{PROJ}/frontend/src/middleware.ts'", timeout=15)
            run(
                client,
                f"export NEXTAUTH_URL=http://{HOST}:3001 && "
                f"chmod +x '{PROJ}/server_start_web.sh' && bash '{PROJ}/server_start_web.sh'",
                timeout=1800,
            )
            run(
                client,
                f"curl -s -o /dev/null -w 'web=%{{http_code}}\\n' http://127.0.0.1:3001/login",
                timeout=30,
            )
            bot_files = [
                ("bot_config.py", f"{PROJ}/bot_config.py"),
                ("dashboard_bridge.py", f"{PROJ}/dashboard_bridge.py"),
            ]
            sftp = client.open_sftp()
            for local_rel, remote_path in bot_files:
                print(f"Upload {local_rel} -> {remote_path}", file=sys.stderr)
                sftp.put(str(ROOT / local_rel), remote_path)
            sftp.close()
            run(client, f"bash '{PROJ}/server_start_bot.sh'", timeout=120)
            print(f"Web login set to {user} / (password updated), auth hardened", file=sys.stderr)
            return 0

        if mode == "web":
            web_sh = ROOT / "scripts" / "server_start_web.sh"
            remote_web = f"{PROJ}/server_start_web.sh"
            sftp = client.open_sftp()
            sftp.put(str(web_sh), remote_web)
            sftp.close()
            steps = [
                "command -v node && node --version || "
                "(curl -fsSL https://rpm.nodesource.com/setup_20.x | bash - && dnf install -y nodejs)",
                f"chmod +x '{remote_web}'",
                f"export NEXTAUTH_URL=http://{HOST}:3001 && bash '{remote_web}'",
                "firewall-cmd --permanent --add-port=3001/tcp 2>/dev/null || true",
                "firewall-cmd --reload 2>/dev/null || true",
                "ss -tlnp | grep -E ':3001|:8080|:8081' || true",
                f"tail -n 25 '{PROJ}/logs/frontend.log' 2>/dev/null || true",
            ]
            for step in steps:
                run(client, step, timeout=1800)
            return 0

        if mode == "deploy":
            proj = PROJ
            _, stdout, _ = client.exec_command(f"test -d '{proj}/.git' && echo ok", timeout=15)
            if "ok" not in stdout.read().decode():
                print(f"ERROR: {proj} missing — run: python scripts/remote_deploy.py setup", file=sys.stderr)
                return 1
            bot_sh = ROOT / "scripts" / "server_start_bot.sh"
            web_sh = ROOT / "scripts" / "server_start_web.sh"
            remote_bot = f"{proj}/server_start_bot.sh"
            remote_web = f"{proj}/server_start_web.sh"
            sftp = client.open_sftp()
            sftp.put(str(bot_sh), remote_bot)
            sftp.put(str(web_sh), remote_web)
            sftp.close()
            steps = [
                f"cd '{proj}' && git pull origin main",
                f"cd '{proj}' && .venv/bin/pip install -q -r requirements.txt",
                f"chmod +x '{remote_bot}' '{remote_web}' && bash '{remote_bot}'",
                "command -v node && node --version || "
                "(curl -fsSL https://rpm.nodesource.com/setup_20.x | bash - && dnf install -y nodejs)",
                f"export NEXTAUTH_URL=http://{HOST}:3001 && bash '{remote_web}'",
                "firewall-cmd --permanent --add-port=3001/tcp 2>/dev/null || true",
                "firewall-cmd --reload 2>/dev/null || true",
                "ss -tlnp | grep -E ':3001|:8080|:8081' || true",
                "pgrep -af 'start_bot|trading-core|next-server' || true",
            ]
            rc = 0
            for step in steps:
                r = run(client, step, timeout=1800)
                if r != 0 and "git pull" in step:
                    rc = r
            return rc

        if mode == "prelaunch-fix":
            import secrets

            upload_files = [
                "trading-core/src/state/StateStore.h",
                "trading-core/src/risk/RiskManager.h",
                "trading-core/src/risk/RiskManager.cpp",
                "trading-core/src/signals/LegInHedgeDetector.cpp",
                "trading-core/src/main.cpp",
                "bot_config.py",
                "dashboard_bridge.py",
                "start_bot.py",
                "frontend/src/lib/botApi.ts",
                "frontend/src/app/risk/page.tsx",
            ]
            script_files = [
                ("scripts/server_start_bot.sh", f"{PROJ}/server_start_bot.sh"),
                ("scripts/server_start_web.sh", f"{PROJ}/server_start_web.sh"),
            ]
            sftp = client.open_sftp()
            for rel in upload_files:
                local = ROOT / rel
                remote = f"{PROJ}/{rel.replace(chr(92), '/')}"
                print(f"Upload {rel} -> {remote}", file=sys.stderr)
                sftp.put(str(local), remote)
            for local_rel, remote_path in script_files:
                print(f"Upload {local_rel} -> {remote_path}", file=sys.stderr)
                sftp.put(str(ROOT / local_rel), remote_path)
            sftp.close()
            token = secrets.token_hex(24)
            env_patch = (
                f"grep -q '^HTTP_BIND=' '{PROJ}/.env' && "
                f"sed -i 's/^HTTP_BIND=.*/HTTP_BIND=127.0.0.1/' '{PROJ}/.env' || "
                f"echo 'HTTP_BIND=127.0.0.1' >> '{PROJ}/.env'; "
                f"grep -q '^DH_ENABLE_15M=' '{PROJ}/.env' && "
                f"sed -i 's/^DH_ENABLE_15M=.*/DH_ENABLE_15M=false/' '{PROJ}/.env' || "
                f"echo 'DH_ENABLE_15M=false' >> '{PROJ}/.env'; "
                f"grep -q '^BOT_API_TOKEN=' '{PROJ}/.env' || "
                f"echo 'BOT_API_TOKEN={token}' >> '{PROJ}/.env'"
            )
            steps = [
                env_patch,
                f"grep -E '^(HTTP_BIND|DH_ENABLE_15M|BOT_API_TOKEN|LIVE_LIH)' '{PROJ}/.env' | head -8",
                BUILD_VPS,
                f"chmod +x '{PROJ}/server_start_bot.sh' && bash '{PROJ}/server_start_bot.sh'",
                "sleep 8",
                "ss -tlnp | grep -E ':8080|:8081' || true",
                "curl -s -o /dev/null -w 'bot_local=%{http_code}\n' http://127.0.0.1:8081/health",
                f"export NEXTAUTH_URL=http://{HOST}:3001 && bash '{PROJ}/server_start_web.sh'",
                "sleep 10",
                f"curl -s -o /dev/null -w 'web=%{{http_code}}\\n' http://127.0.0.1:3001/login",
                "pgrep -af 'start_bot|trading-core|next-server' || true",
            ]
            for step in steps:
                r = run(client, step, timeout=1800)
                if r != 0 and ("build.sh" in step or "build-lowmem" in step):
                    return r
            return 0

        if mode == "step1-shadow":
            # Step 1 go-live: shadow only (LIVE_LIH_DRY_RUN=true).
            sftp = client.open_sftp()
            for rel in ("polymarket_fees.py", "live_preflight.py"):
                print(f"Upload {rel} -> {PROJ}/{rel}", file=sys.stderr)
                sftp.put(str(ROOT / rel), f"{PROJ}/{rel}")
            bot_sh = ROOT / "scripts" / "server_start_bot.sh"
            sftp.put(str(bot_sh), f"{PROJ}/server_start_bot.sh")
            sftp.close()
            env_patch = (
                f"grep -q '^LIVE_LIH_DRY_RUN=' '{PROJ}/.env' && "
                f"sed -i 's/^LIVE_LIH_DRY_RUN=.*/LIVE_LIH_DRY_RUN=true/' '{PROJ}/.env' || "
                f"echo 'LIVE_LIH_DRY_RUN=true' >> '{PROJ}/.env'"
            )
            steps = [
                env_patch,
                f"grep -E '^(LIVE_LIH_DRY_RUN|LIH_ENABLED|RISK_MAX|POLYMARKET_FUNDER)' '{PROJ}/.env'",
                f"cd '{PROJ}' && .venv/bin/python fetch_balance.py",
                f"cd '{PROJ}' && .venv/bin/python start_bot.py --preflight-only",
                f"chmod +x '{PROJ}/server_start_bot.sh' && bash '{PROJ}/server_start_bot.sh'",
                "sleep 15",
                "pgrep -af 'start_bot|trading-core' || true",
                f"grep -E 'Starting Core|LIH dry-run|Mode: LIVE|MARKETS REFRESHED|fee_curve' "
                f"'{PROJ}/logs/bridge.log' | tail -15",
                f"grep -c 'LIVE LIH SHADOW' '{PROJ}/logs/bridge.log' 2>/dev/null; echo shadow_lines_above",
                f"grep 'LIVE LIH SHADOW' '{PROJ}/logs/bridge.log' 2>/dev/null | tail -5",
                f"tail -12 '{PROJ}/logs/bridge.log'",
            ]
            for step in steps:
                r = run(client, step, timeout=300)
                if r != 0 and "preflight-only" in step:
                    print("WARN: preflight returned non-zero — check output", file=sys.stderr)
            return 0

        if mode == "fix-clob-payload":
            sftp = client.open_sftp()
            uploads = [
                (ROOT / "trading-core" / "src" / "exec" / "OrderRouter.cpp", f"{PROJ}/trading-core/src/exec/OrderRouter.cpp"),
                (ROOT / "trading-core" / "src" / "exec" / "OrderRouter.h", f"{PROJ}/trading-core/src/exec/OrderRouter.h"),
                (ROOT / "trading-core" / "src" / "main.cpp", f"{PROJ}/trading-core/src/main.cpp"),
                (ROOT / "clob_live.py", f"{PROJ}/clob_live.py"),
                (ROOT / "fetch_balance.py", f"{PROJ}/fetch_balance.py"),
                (ROOT / "dashboard_bridge.py", f"{PROJ}/dashboard_bridge.py"),
            ]
            for local, remote in uploads:
                print(f"Upload {local.name} -> {remote}", file=sys.stderr)
                sftp.put(str(local), remote)
            sftp.close()
            steps = [
                BUILD_VPS,
                f"bash '{PROJ}/server_start_bot.sh'",
                "sleep 15",
                f"grep -E 'Python CLOB bridge|LIH dry-run|LIVE EXEC|Bridge fill|Invalid order' '{PROJ}/bot.log' | tail -15",
            ]
            for step in steps:
                run(client, step, timeout=900)
            return 0

        if mode == "go-live-real":
            upload_files = ["prelive_lih_check.py", "live_preflight.py", "start_bot.py"]
            sftp = client.open_sftp()
            for rel in upload_files:
                print(f"Upload {rel} -> {PROJ}/{rel}", file=sys.stderr)
                sftp.put(str(ROOT / rel), f"{PROJ}/{rel}")
            sftp.close()
            env_patch = (
                f"grep -q '^LIVE_LIH_DRY_RUN=' '{PROJ}/.env' && "
                f"sed -i 's/^LIVE_LIH_DRY_RUN=.*/LIVE_LIH_DRY_RUN=false/' '{PROJ}/.env' || "
                f"echo 'LIVE_LIH_DRY_RUN=false' >> '{PROJ}/.env'; "
                f"sed -i 's/^DH_ENABLE_15M=.*/DH_ENABLE_15M=false/' '{PROJ}/.env'; "
                f"grep -q '^POLYMARKET_SIGNATURE_TYPE=' '{PROJ}/.env' && "
                f"sed -i 's/^POLYMARKET_SIGNATURE_TYPE=.*/POLYMARKET_SIGNATURE_TYPE=3/' '{PROJ}/.env' || "
                f"echo 'POLYMARKET_SIGNATURE_TYPE=3' >> '{PROJ}/.env'"
            )
            steps = [
                env_patch,
                f"grep -E '^(LIVE_LIH_DRY_RUN|LIH_ENABLED|LIH_MAX_USDC|RISK_MAX|LIVE_TRADES_BASELINE)' '{PROJ}/.env'",
                f"cd '{PROJ}' && .venv/bin/python fetch_balance.py",
                f"cd '{PROJ}' && .venv/bin/python prelive_lih_check.py --allow-live --since-baseline",
                f"cd '{PROJ}' && .venv/bin/python start_bot.py --preflight-only",
                f"bash '{PROJ}/server_start_bot.sh'",
                "sleep 18",
                "pgrep -af 'start_bot|trading-core' || true",
                f"grep -E 'Starting Core|LIH dry-run|Mode: LIVE|dry_run' "
                f"'{PROJ}/bot.log' '{PROJ}/logs/bridge.log' 2>/dev/null | tail -10",
                f"tail -20 '{PROJ}/bot.log' 2>/dev/null || tail -20 '{PROJ}/logs/bridge.log'",
            ]
            for step in steps:
                r = run(client, step, timeout=300)
                if r != 0 and ("preflight-only" in step or "prelive_lih_check" in step):
                    print("FATAL: preflight/prelive failed — aborting go-live", file=sys.stderr)
                    # Revert to shadow on failure
                    run(
                        client,
                        f"grep -q '^LIVE_LIH_DRY_RUN=' '{PROJ}/.env' && "
                        f"sed -i 's/^LIVE_LIH_DRY_RUN=.*/LIVE_LIH_DRY_RUN=true/' '{PROJ}/.env'",
                        timeout=30,
                    )
                    run(client, f"bash '{PROJ}/server_start_bot.sh'", timeout=120)
                    return 1
            return 0

        if mode == "no-new-entries":
            """Block new LIH leg1 after current position closes; rebalance on open positions still allowed."""
            steps = [
                f"grep -q '^RISK_MAX_CONCURRENT_POSITIONS=' '{PROJ}/.env' && "
                f"sed -i 's/^RISK_MAX_CONCURRENT_POSITIONS=.*/RISK_MAX_CONCURRENT_POSITIONS=0/' '{PROJ}/.env' || "
                f"echo 'RISK_MAX_CONCURRENT_POSITIONS=0' >> '{PROJ}/.env'",
                f"printf '%s' '{{\"patch\":{{\"RISK_MAX_CONCURRENT_POSITIONS\":\"0\"}},"
                f"\"user\":\"no-new-entries\"}}' > '{PROJ}/logs/runtime_config.json'",
                f"grep '^RISK_MAX_CONCURRENT_POSITIONS=' '{PROJ}/.env'",
                "sleep 4",
                f"curl -s http://127.0.0.1:8081/api/config | python3 -c "
                "\"import sys,json; l=json.load(sys.stdin).get('live',{}); "
                "print('openCount', l.get('openCount')); "
                "print('riskMaxConcurrent', l.get('riskMaxConcurrentPositions'))\"",
            ]
            for step in steps:
                run(client, step, timeout=120)
            return 0

        if mode == "set-risk-20":
            patch = (
                f"cd '{PROJ}' && .venv/bin/python -c "
                "\"from bot_config import update_env, write_runtime_config; "
                "p=update_env({'RISK_MAX_POSITION_FRACTION': '0.20'}); "
                "write_runtime_config({'patch': p, 'user': 'deploy'}); "
                "print('applied', p)\""
            )
            steps = [
                patch,
                f"grep '^RISK_MAX_POSITION_FRACTION=' '{PROJ}/.env'",
                "sleep 3",
                f"grep 'CONFIG RISK_MAX_POSITION' '{PROJ}/bot.log' | tail -2",
                f"curl -s http://127.0.0.1:8081/api/config | python3 -c "
                "\"import sys,json; l=json.load(sys.stdin).get('live',{}); "
                "print('riskMaxPositionFraction', l.get('riskMaxPositionFraction')); "
                "print('balance', l.get('balance'))\"",
            ]
            for step in steps:
                run(client, step, timeout=60)
            return 0

        if mode == "cleanup-memory":
            web_sh = ROOT / "scripts" / "server_restart_web.sh"
            sftp = client.open_sftp()
            sftp.put(str(web_sh), f"{PROJ}/server_restart_web.sh")
            sftp.put(str(ROOT / "scripts" / "server_start_web.sh"), f"{PROJ}/server_start_web.sh")
            sftp.close()
            steps = [
                "free -h",
                "pgrep -c next-server || echo 0",
                "pkill -9 -f next-server 2>/dev/null || true",
                "sleep 2",
                "pgrep -c next-server || echo 0",
                f"chmod +x '{PROJ}/server_restart_web.sh' && bash '{PROJ}/server_restart_web.sh'",
                "sleep 4",
                "free -h",
                "pgrep -c next-server || echo 0",
                "pgrep -af 'next-server|start_bot|trading-core' || true",
                f"curl -s -o /dev/null -w 'web=%{{http_code}}\\n' http://127.0.0.1:3001/login",
            ]
            for step in steps:
                run(client, step, timeout=120)
            return 0

        if mode == "harden-startup":
            upload_files = [
                "start_bot.py",
                "scripts/server_start_bot.sh",
                "dashboard_bridge.py",
                "clob_trades.py",
                "scripts/prune_live_lih.py",
                "scripts/live_lih_reconcile.py",
                "trading-core/src/risk/RiskManager.h",
                "trading-core/src/risk/RiskManager.cpp",
                "trading-core/src/signals/LegInHedgeDetector.cpp",
                "trading-core/src/main.cpp",
            ]
            sftp = client.open_sftp()
            for rel in upload_files:
                local = ROOT / rel
                remote = f"{PROJ}/{rel.replace(chr(92), '/')}"
                remote_parent = remote.rsplit("/", 1)[0]
                parts: list[str] = []
                for part in remote_parent.split("/"):
                    if not part:
                        continue
                    parts.append(part)
                    seg = "/" + "/".join(parts)
                    try:
                        sftp.stat(seg)
                    except OSError:
                        try:
                            sftp.mkdir(seg)
                        except OSError:
                            pass
                print(f"Upload {rel} -> {remote}", file=sys.stderr)
                sftp.put(str(local), remote)
            sftp.close()
            env_patch = (
                f"grep -q '^START_SKIP_PRELIVE=' '{PROJ}/.env' && "
                f"sed -i 's/^START_SKIP_PRELIVE=.*/START_SKIP_PRELIVE=1/' '{PROJ}/.env' || "
                f"echo 'START_SKIP_PRELIVE=1' >> '{PROJ}/.env'"
            )
            steps = [
                env_patch,
                "pkill -f trading-core || true",
                "pkill -f start_bot.py || true",
                "sleep 2",
                f"cd '{PROJ}' && .venv/bin/python scripts/prune_live_lih.py",
                BUILD_VPS,
                f"chmod +x '{PROJ}/server_start_bot.sh' && bash '{PROJ}/server_start_bot.sh'",
                "sleep 15",
                "pgrep -af 'start_bot|trading-core' || true",
                f"curl -s http://127.0.0.1:8081/api/config | python3 -c "
                "'import sys,json; l=json.load(sys.stdin).get(\"live\",{}); "
                "print(\"openCount\", l.get(\"openCount\")); "
                "ops=l.get(\"openPositions\") or []; "
                "[print(p.get(\"asset\"), p.get(\"heldSide\"), p.get(\"yesSize\"), p.get(\"noSize\")) for p in ops]'",
            ]
            for step in steps:
                r = run(client, step, timeout=1800)
                if r != 0 and ("build.sh" in step or "build-lowmem" in step):
                    return r
            return 0

        if mode == "conservative-live":
            upload_files = [
                "clob_live.py",
                "bot_config.py",
                "trading-core/src/exec/OrderRouter.h",
                "trading-core/src/exec/OrderRouter.cpp",
                "trading-core/src/risk/RiskManager.h",
                "trading-core/src/risk/RiskManager.cpp",
                "trading-core/src/signals/LegInHedgeDetector.cpp",
                "trading-core/src/state/StateStore.cpp",
                "trading-core/src/main.cpp",
            ]
            sftp = client.open_sftp()
            for rel in upload_files:
                local = ROOT / rel
                remote = f"{PROJ}/{rel.replace(chr(92), '/')}"
                remote_parent = remote.rsplit("/", 1)[0]
                parts: list[str] = []
                for part in remote_parent.split("/"):
                    if not part:
                        continue
                    parts.append(part)
                    seg = "/" + "/".join(parts)
                    try:
                        sftp.stat(seg)
                    except OSError:
                        try:
                            sftp.mkdir(seg)
                        except OSError:
                            pass
                print(f"Upload {rel} -> {remote}", file=sys.stderr)
                sftp.put(str(local), remote)
            sftp.close()
            env_lines = [
                ("RISK_MAX_CONCURRENT_POSITIONS", "1"),
                ("LIH_ONE_SLOT_GLOBAL", "true"),
                ("LIH_SESSION_MAX_LEGS", "2"),
                ("LIH_MIN_BALANCE_USDC", "10"),
                ("LIH_MAX_USDC_PER_SLOT", "10"),
                ("LIH_LEG1_MIN_SECONDS_REMAINING", "30"),
                ("LIH_MIN_SECONDS_REMAINING", "15"),
                ("START_SKIP_PRELIVE", "1"),
            ]
            env_cmds = []
            for key, val in env_lines:
                env_cmds.append(
                    f"grep -q '^{key}=' '{PROJ}/.env' && "
                    f"sed -i 's/^{key}=.*/{key}={val}/' '{PROJ}/.env' || "
                    f"echo '{key}={val}' >> '{PROJ}/.env'"
                )
            runtime_patch = (
                f"printf '%s' '{{\"patch\":{{"
                f"\"RISK_MAX_CONCURRENT_POSITIONS\":\"1\","
                f"\"LIH_ONE_SLOT_GLOBAL\":\"true\","
                f"\"LIH_SESSION_MAX_LEGS\":\"2\","
                f"\"LIH_MIN_BALANCE_USDC\":\"10\","
                f"\"LIH_MAX_USDC_PER_SLOT\":\"10\""
                f"}},\"user\":\"deploy-conservative\"}}' "
                f"| curl -s -X POST http://127.0.0.1:8081/api/config "
                f"-H 'Content-Type: application/json' -d @- || true"
            )
            steps = env_cmds + [
                runtime_patch,
                "pkill -f trading-core || true",
                "pkill -f start_bot.py || true",
                "sleep 2",
                f"cd '{PROJ}' && .venv/bin/python scripts/live_lih_reconcile.py",
                BUILD_VPS,
                f"chmod +x '{PROJ}/server_start_bot.sh' && bash '{PROJ}/server_start_bot.sh'",
                "sleep 15",
                "pgrep -af 'start_bot|trading-core' || true",
                f"grep -E 'LIH|session|one.slot|LEG1|open=' '{PROJ}/bot.log' | tail -20",
                f"curl -s http://127.0.0.1:8081/api/config | python3 -c "
                "\"import sys,json; l=json.load(sys.stdin).get('live',{}); "
                "print('openCount', l.get('openCount'), "
                "'session', l.get('lihSessionLegsUsed'), '/', l.get('lihSessionMaxLegs')); "
                "ops=l.get('openPositions') or []; "
                "[print(p.get('asset'), p.get('heldSide'), p.get('yesSize'), p.get('noSize')) for p in ops]\"",
            ]
            for step in steps:
                r = run(client, step, timeout=1800)
                if r != 0 and ("build.sh" in step or "build-lowmem" in step):
                    return r
            return 0

        if mode == "fix-fill-reconcile":
            upload_files = [
                "trading-core/src/risk/RiskManager.cpp",
                "trading-core/src/exec/OrderRouter.cpp",
                "clob_live.py",
                "clob_trades.py",
                "scripts/live_lih_reconcile.py",
                "scripts/server_start_bot.sh",
                "bot_config.py",
            ]
            sftp = client.open_sftp()
            for rel in upload_files:
                local = ROOT / rel
                remote = f"{PROJ}/{rel.replace(chr(92), '/')}"
                remote_parent = remote.rsplit("/", 1)[0]
                parts: list[str] = []
                for part in remote_parent.split("/"):
                    if not part:
                        continue
                    parts.append(part)
                    seg = "/" + "/".join(parts)
                    try:
                        sftp.stat(seg)
                    except OSError:
                        try:
                            sftp.mkdir(seg)
                        except OSError:
                            pass
                print(f"Upload {rel} -> {remote}", file=sys.stderr)
                sftp.put(str(local), remote)
            sftp.close()
            steps = [
                f"cd '{PROJ}' && .venv/bin/python scripts/live_lih_reconcile.py",
                BUILD_VPS,
                f"chmod +x '{PROJ}/server_start_bot.sh' && bash '{PROJ}/server_start_bot.sh'",
                "sleep 15",
                f"grep -E 'Live LIH state|open_lih|LEG1|rebalance|Bridge fill' '{PROJ}/bot.log' | tail -25",
                f"curl -s http://127.0.0.1:8081/api/config | python3 -c "
                "\"import sys,json; l=json.load(sys.stdin).get('live',{}); "
                "print('openCount', l.get('openCount')); "
                "ops=l.get('openPositions') or []; "
                "[print(p.get('asset'), p.get('heldSide'), p.get('yesSize'), p.get('noSize')) for p in ops]\"",
            ]
            for step in steps:
                r = run(client, step, timeout=1800)
                if r != 0 and ("build.sh" in step or "build-lowmem" in step):
                    return r
            return 0

        if mode == "fix-critical":
            # Duplicate-order guards, live state persist, BOT_API_TOKEN sync, CLOB trade history.
            upload_files = [
                "trading-core/src/risk/RiskManager.h",
                "trading-core/src/risk/RiskManager.cpp",
                "trading-core/src/signals/LegInHedgeDetector.cpp",
                "trading-core/src/exec/OrderRouter.cpp",
                "trading-core/src/state/PaperStateStore.h",
                "trading-core/src/state/PaperStateStore.cpp",
                "trading-core/src/main.cpp",
                "dashboard_bridge.py",
                "clob_live.py",
                "clob_trades.py",
                "frontend/src/lib/botApi.ts",
                "frontend/src/lib/clobTrades.ts",
                "frontend/src/app/api/clob/trades/route.ts",
                "frontend/src/app/history/page.tsx",
            ]
            script_files = [
                ("scripts/server_start_web.sh", f"{PROJ}/server_start_web.sh"),
                ("scripts/server_restart_web.sh", f"{PROJ}/server_restart_web.sh"),
                ("scripts/server_start_bot.sh", f"{PROJ}/server_start_bot.sh"),
            ]
            sftp = client.open_sftp()

            def _ensure_remote_dir(path: str) -> None:
                remote_parent = path.rsplit("/", 1)[0]
                parts: list[str] = []
                for part in remote_parent.split("/"):
                    if not part:
                        continue
                    parts.append(part)
                    seg = "/" + "/".join(parts)
                    try:
                        sftp.stat(seg)
                    except OSError:
                        try:
                            sftp.mkdir(seg)
                        except OSError:
                            pass

            for rel in upload_files:
                local = ROOT / rel
                remote = f"{PROJ}/{rel.replace(chr(92), '/')}"
                _ensure_remote_dir(remote)
                print(f"Upload {rel} -> {remote}", file=sys.stderr)
                sftp.put(str(local), remote)
            for local_rel, remote_path in script_files:
                print(f"Upload {local_rel} -> {remote_path}", file=sys.stderr)
                sftp.put(str(ROOT / local_rel), remote_path)
            sftp.close()
            env_patch = (
                f"grep -q '^LIVE_LIH_DRY_RUN=' '{PROJ}/.env' && "
                f"sed -i 's/^LIVE_LIH_DRY_RUN=.*/LIVE_LIH_DRY_RUN=true/' '{PROJ}/.env' || "
                f"echo 'LIVE_LIH_DRY_RUN=true' >> '{PROJ}/.env'; "
                f"grep -q '^LIVE_STATE_PERSIST=' '{PROJ}/.env' || "
                f"echo 'LIVE_STATE_PERSIST=true' >> '{PROJ}/.env'; "
                f"grep -q '^LIVE_STATE_PATH=' '{PROJ}/.env' || "
                f"echo 'LIVE_STATE_PATH=logs/live_state.json' >> '{PROJ}/.env'; "
                f"TOKEN=$(grep '^BOT_API_TOKEN=' '{PROJ}/.env' | cut -d= -f2-); "
                f"if [ -n \"$TOKEN\" ] && [ -f '{PROJ}/web.env' ]; then "
                f"grep -q '^BOT_API_TOKEN=' '{PROJ}/web.env' || echo \"BOT_API_TOKEN=$TOKEN\" >> '{PROJ}/web.env'; "
                f"fi"
            )
            steps = [
                env_patch,
                f"grep -E '^(LIVE_LIH|BOT_API_TOKEN|LIVE_STATE)' '{PROJ}/.env' | head -10",
                f"grep BOT_API_TOKEN '{PROJ}/web.env' 2>/dev/null | head -1 || echo 'web.env: no BOT_API_TOKEN yet'",
                BUILD_VPS,
                f"chmod +x '{PROJ}/server_start_bot.sh' && bash '{PROJ}/server_start_bot.sh'",
                "sleep 10",
                "curl -s http://127.0.0.1:8081/api/clob/trades?limit=3 | python3 -c "
                "\"import sys,json; d=json.load(sys.stdin); print('clob_trades', d.get('count',0))\"",
                f"export NEXTAUTH_URL=http://{HOST}:3001 && bash '{PROJ}/server_start_web.sh'",
                "sleep 12",
                f"TOKEN=$(grep '^BOT_API_TOKEN=' '{PROJ}/.env' | cut -d= -f2-); "
                f"curl -s -o /dev/null -w 'config_post=%{{http_code}}\\n' "
                f"-H \"X-Bot-Api-Token: $TOKEN\" -H 'Content-Type: application/json' "
                f"-d '{{\"patch\":{{\"RISK_MAX_CONCURRENT_POSITIONS\":\"3\"}},\"user\":\"deploy-test\"}}' "
                f"http://127.0.0.1:8081/api/config",
                f"curl -s -o /dev/null -w 'web=%{{http_code}}\\n' http://127.0.0.1:3001/login",
                f"grep -E 'Live LIH state|in-flight|LIH dry-run' '{PROJ}/logs/bridge.log' | tail -8",
            ]
            for step in steps:
                r = run(client, step, timeout=1800)
                if r != 0 and ("build.sh" in step or "build-lowmem" in step):
                    return r
            return 0

        if mode == "split-cooldown":
            upload_files = [
                "trading-core/src/signals/LegInHedgeDetector.h",
                "trading-core/src/signals/LegInHedgeDetector.cpp",
                "trading-core/src/main.cpp",
                "bot_config.py",
                "frontend/src/app/strategies/page.tsx",
            ]
            sftp = client.open_sftp()
            for rel in upload_files:
                local = ROOT / rel
                remote = f"{PROJ}/{rel.replace(chr(92), '/')}"
                print(f"Upload {rel} -> {remote}", file=sys.stderr)
                sftp.put(str(local), remote)
            sftp.close()
            env_patch = (
                f"grep -q '^LIH_LEG1_COOLDOWN_SECONDS=' '{PROJ}/.env' || "
                f"echo 'LIH_LEG1_COOLDOWN_SECONDS=20' >> '{PROJ}/.env'; "
                f"grep -q '^LIH_REBALANCE_COOLDOWN_SECONDS=' '{PROJ}/.env' || "
                f"echo 'LIH_REBALANCE_COOLDOWN_SECONDS=5' >> '{PROJ}/.env'; "
                f"sed -i 's/^LIH_LEG1_COOLDOWN_SECONDS=.*/LIH_LEG1_COOLDOWN_SECONDS=20/' '{PROJ}/.env'; "
                f"sed -i 's/^LIH_REBALANCE_COOLDOWN_SECONDS=.*/LIH_REBALANCE_COOLDOWN_SECONDS=5/' '{PROJ}/.env'"
            )
            steps = [
                env_patch,
                f"grep -E 'LIH_.*COOLDOWN' '{PROJ}/.env'",
                BUILD_VPS,
                f"bash '{PROJ}/server_start_bot.sh'",
                "sleep 8",
                f"grep 'leg1_cd=' '{PROJ}/logs/bridge.log' | tail -3",
                f"export NEXTAUTH_URL=http://{HOST}:3001 && "
                f"cd '{PROJ}/frontend' && npm run build && "
                f"pkill -f next-server 2>/dev/null || true; sleep 2; "
                f"setsid -f -- npm run start >> '{PROJ}/logs/frontend.log' 2>&1",
            ]
            for step in steps:
                r = run(client, step, timeout=1800)
                if r != 0 and ("build.sh" in step or "build-lowmem" in step):
                    return r
            return 0

        if mode == "leg2-lock":
            upload_files = [
                "trading-core/src/risk/RiskManager.h",
                "trading-core/src/risk/RiskManager.cpp",
                "trading-core/src/signals/LegInHedgeDetector.cpp",
                "trading-core/src/exec/OrderRouter.cpp",
            ]
            sftp = client.open_sftp()
            for rel in upload_files:
                local = ROOT / rel
                remote = f"{PROJ}/{rel.replace(chr(92), '/')}"
                print(f"Upload {rel} -> {remote}", file=sys.stderr)
                sftp.put(str(local), remote)
            sftp.close()
            steps = [
                f"grep -q '^LIH_COOLDOWN_SECONDS=' '{PROJ}/.env' && "
                f"sed -i 's/^LIH_COOLDOWN_SECONDS=.*/LIH_COOLDOWN_SECONDS=20/' '{PROJ}/.env' || "
                f"echo 'LIH_COOLDOWN_SECONDS=20' >> '{PROJ}/.env'",
                BUILD_VPS,
                f"bash '{PROJ}/server_start_bot.sh'",
                "sleep 8",
                f"grep -E 'LIH_COOLDOWN|LIH dry-run' '{PROJ}/.env' '{PROJ}/logs/bridge.log' 2>/dev/null | tail -5",
            ]
            for step in steps:
                r = run(client, step, timeout=1800)
                if r != 0 and ("build.sh" in step or "build-lowmem" in step):
                    return r
            return 0

        if mode == "clear-shadow":
            upload_files = ["live_preflight.py", "start_bot.py", "prelive_lih_check.py"]
            sftp = client.open_sftp()
            for rel in upload_files:
                print(f"Upload {rel} -> {PROJ}/{rel}", file=sys.stderr)
                sftp.put(str(ROOT / rel), f"{PROJ}/{rel}")
            sftp.close()
            steps = [
                f"cd '{PROJ}' && .venv/bin/python -c "
                "\"from bot_config import clear_live_trades_history; "
                "print('cleared', clear_live_trades_history())\"",
                f"grep -E '^(LIVE_TRADES_BASELINE_TS|LIVE_LIH_DRY_RUN)' '{PROJ}/.env'",
                f"ls -la '{PROJ}/logs/live_state.json' 2>/dev/null || echo 'live_state removed'",
                f"ls -la '{PROJ}/logs/live_state.json.bak.*' 2>/dev/null | tail -1 || true",
                f"bash '{PROJ}/server_start_bot.sh'",
                "sleep 12",
                "pgrep -af trading-core || echo NO_CORE",
                f"grep 'LIH dry-run' '{PROJ}/bot.log' 2>/dev/null | tail -2",
                f"tail -8 '{PROJ}/logs/bridge.log'",
                "curl -s http://127.0.0.1:8081/api/config 2>/dev/null | python3 -c "
                "\"import sys,json; l=json.load(sys.stdin).get('live',{}); "
                "print('bot_balance', l.get('balance'), 'open_positions', l.get('openPositions'), "
                "'lih_positions', len(l.get('lihPositions') or []))\"",
                f"cd '{PROJ}' && .venv/bin/python prelive_lih_check.py --since-hours 1 2>&1 | tail -15",
            ]
            for step in steps:
                run(client, step, timeout=180)
            return 0

        if mode == "clear-live-history":
            upload_files = [
                "trading-core/src/state/StateStore.h",
                "trading-core/src/state/StateStore.cpp",
                "trading-core/src/main.cpp",
                "bot_config.py",
                "clob_trades.py",
                "dashboard_bridge.py",
                "frontend/src/lib/clobTrades.ts",
                "frontend/src/hooks/useLiveState.ts",
                "frontend/src/app/history/page.tsx",
            ]
            script_files = [
                ("scripts/server_start_bot.sh", f"{PROJ}/server_start_bot.sh"),
                ("scripts/server_start_web.sh", f"{PROJ}/server_start_web.sh"),
            ]
            sftp = client.open_sftp()

            def _ensure_dir(path: str) -> None:
                remote_parent = path.rsplit("/", 1)[0]
                parts: list[str] = []
                for part in remote_parent.split("/"):
                    if not part:
                        continue
                    parts.append(part)
                    seg = "/" + "/".join(parts)
                    try:
                        sftp.stat(seg)
                    except OSError:
                        try:
                            sftp.mkdir(seg)
                        except OSError:
                            pass

            for rel in upload_files:
                local = ROOT / rel
                remote = f"{PROJ}/{rel.replace(chr(92), '/')}"
                _ensure_dir(remote)
                print(f"Upload {rel} -> {remote}", file=sys.stderr)
                sftp.put(str(local), remote)
            for local_rel, remote_path in script_files:
                print(f"Upload {local_rel} -> {remote_path}", file=sys.stderr)
                sftp.put(str(ROOT / local_rel), remote_path)
            sftp.close()
            steps = [
                f"cd '{PROJ}' && .venv/bin/python -c "
                "\"from bot_config import clear_live_trades_history; "
                "r=clear_live_trades_history(); print('cleared', r)\"",
                f"grep '^LIVE_TRADES_BASELINE_TS=' '{PROJ}/.env'",
                f"ls -la '{PROJ}/logs/live_state.json' 2>/dev/null || echo 'live_state removed'",
                BUILD_VPS,
                f"bash '{PROJ}/server_start_bot.sh'",
                "sleep 10",
                f"export NEXTAUTH_URL=http://{HOST}:3001 && bash '{PROJ}/server_start_web.sh'",
                "sleep 10",
                "curl -s 'http://127.0.0.1:8081/api/clob/trades?limit=5' | python3 -c "
                "\"import sys,json; d=json.load(sys.stdin); print('clob_count', d.get('count',0))\"",
                f"curl -s http://127.0.0.1:8081/api/config | python3 -c "
                "\"import sys,json; l=json.load(sys.stdin).get('live',{}); "
                "print('tradesBaselineTs', l.get('tradesBaselineTs')); "
                "print('tradeHistory_len', len(l.get('tradeHistory') or [])); "
                "print('totalLihTrades', l.get('totalLihTrades'))\"",
            ]
            for step in steps:
                r = run(client, step, timeout=1800)
                if r != 0 and ("build.sh" in step or "build-lowmem" in step):
                    return r
            return 0

        if mode == "optimize-web":
            files = [
                ("scripts/web_run.sh", f"{PROJ}/scripts/web_run.sh"),
                ("scripts/web_watchdog.sh", f"{PROJ}/scripts/web_watchdog.sh"),
                ("scripts/web_install_watchdog.sh", f"{PROJ}/scripts/web_install_watchdog.sh"),
                ("scripts/server_restart_web.sh", f"{PROJ}/server_restart_web.sh"),
                ("scripts/server_start_web.sh", f"{PROJ}/server_start_web.sh"),
                ("frontend/next.config.ts", f"{PROJ}/frontend/next.config.ts"),
            ]
            sftp = client.open_sftp()
            for local_rel, remote in files:
                print(f"Upload {local_rel} -> {remote}", file=sys.stderr)
                sftp.put(str(ROOT / local_rel), remote)
            sftp.close()
            steps = [
                "free -h",
                f"chmod +x '{PROJ}/scripts/web_run.sh' '{PROJ}/scripts/web_watchdog.sh' "
                f"'{PROJ}/scripts/web_install_watchdog.sh' '{PROJ}/server_restart_web.sh' '{PROJ}/server_start_web.sh'",
                f"export NEXTAUTH_URL=http://{HOST}:3001 && bash '{PROJ}/server_start_web.sh'",
                "sleep 8",
                f"bash '{PROJ}/scripts/web_install_watchdog.sh'",
                "pgrep -af 'next-server|standalone/server.js|node server.js' | head -5",
                "ss -tlnp | grep ':3001' || true",
                f"curl -s -o /dev/null -w 'login=%{{http_code}}\\n' http://127.0.0.1:3001/login",
                f"ls -la '{PROJ}/frontend/.next/standalone/server.js' 2>/dev/null || echo 'no standalone'",
                "free -h",
                f"tail -5 '{PROJ}/logs/frontend.log'",
            ]
            for step in steps:
                r = run(client, step, timeout=1800)
                if r != 0 and "server_start_web" in step:
                    return r
            return 0

        if mode == "prelive-slot-cap":
            # Shadow=live state parity, per-slot USDC cap, prelive_lih_check gate.
            upload_files = [
                "trading-core/src/risk/RiskManager.h",
                "trading-core/src/risk/RiskManager.cpp",
                "trading-core/src/exec/OrderRouter.cpp",
                "trading-core/src/signals/LegInHedgeDetector.cpp",
                "trading-core/src/main.cpp",
                "prelive_lih_check.py",
                "live_preflight.py",
                "start_bot.py",
                "bot_config.py",
            ]
            sftp = client.open_sftp()
            for rel in upload_files:
                local = ROOT / rel
                remote = f"{PROJ}/{rel.replace(chr(92), '/')}"
                remote_parent = remote.rsplit("/", 1)[0]
                parts: list[str] = []
                for part in remote_parent.split("/"):
                    if not part:
                        continue
                    parts.append(part)
                    seg = "/" + "/".join(parts)
                    try:
                        sftp.stat(seg)
                    except OSError:
                        try:
                            sftp.mkdir(seg)
                        except OSError:
                            pass
                print(f"Upload {rel} -> {remote}", file=sys.stderr)
                sftp.put(str(local), remote)
            sftp.close()
            env_patch = (
                f"grep -q '^LIVE_LIH_DRY_RUN=' '{PROJ}/.env' && "
                f"sed -i 's/^LIVE_LIH_DRY_RUN=.*/LIVE_LIH_DRY_RUN=true/' '{PROJ}/.env' || "
                f"echo 'LIVE_LIH_DRY_RUN=true' >> '{PROJ}/.env'; "
                f"grep -q '^LIH_MAX_USDC_PER_SLOT=' '{PROJ}/.env' || "
                f"echo 'LIH_MAX_USDC_PER_SLOT=0' >> '{PROJ}/.env'; "
                f"grep -q '^PRELIVE_LOG_HOURS=' '{PROJ}/.env' || "
                f"echo 'PRELIVE_LOG_HOURS=24' >> '{PROJ}/.env'; "
                f"grep -q '^LIVE_STATE_PERSIST=' '{PROJ}/.env' || "
                f"echo 'LIVE_STATE_PERSIST=true' >> '{PROJ}/.env'"
            )
            steps = [
                env_patch,
                f"grep -E '^(LIVE_LIH|LIH_MAX_USDC|PRELIVE_LOG|LIH_LEG1_COOLDOWN|RISK_MAX)' '{PROJ}/.env' | head -12",
                BUILD_VPS,
                f"bash '{PROJ}/server_start_bot.sh'",
                "sleep 12",
                f"cd '{PROJ}' && .venv/bin/python start_bot.py --preflight-only",
                f"cd '{PROJ}' && .venv/bin/python prelive_lih_check.py --since-hours 24",
                f"grep -E 'slot_cap=|LIH SHADOW|LIH SHADOW\\]|in-flight|slot budget' "
                f"'{PROJ}/logs/bridge.log' '{PROJ}/bot.log' 2>/dev/null | tail -12",
                f"grep 'LIH config' '{PROJ}/logs/bridge.log' 2>/dev/null | tail -2",
            ]
            for step in steps:
                r = run(client, step, timeout=1800)
                if r != 0 and ("build.sh" in step or "build-lowmem" in step):
                    return r
            return 0

        if mode == "fix-web":
            restart_sh = ROOT / "scripts" / "server_restart_web.sh"
            start_sh = ROOT / "scripts" / "server_start_web.sh"
            sftp = client.open_sftp()
            sftp.put(str(restart_sh), f"{PROJ}/server_restart_web.sh")
            sftp.put(str(start_sh), f"{PROJ}/server_start_web.sh")
            sftp.close()
            steps = [
                "free -h",
                "pgrep -af 'next-server|node.*3001' || echo 'no next process'",
                "ss -tlnp | grep ':3001' || echo 'port 3001 not listening'",
                f"tail -30 '{PROJ}/logs/frontend.log' 2>/dev/null || echo 'no frontend.log'",
                f"curl -s -o /dev/null -w 'local_login=%{{http_code}}\\n' http://127.0.0.1:3001/login || true",
                f"chmod +x '{PROJ}/server_restart_web.sh' && "
                f"export NEXTAUTH_URL=http://{HOST}:3001 && bash '{PROJ}/server_restart_web.sh'",
                "sleep 6",
                "pgrep -af 'next-server' || echo 'still no next'",
                "ss -tlnp | grep ':3001' || echo 'still down'",
                f"curl -s -o /dev/null -w 'after_restart=%{{http_code}}\\n' http://127.0.0.1:3001/login",
                f"tail -8 '{PROJ}/logs/frontend.log'",
            ]
            rc = 0
            for step in steps:
                r = run(client, step, timeout=300)
                if r != 0 and "server_restart_web" in step:
                    rc = r
            return rc

        if mode == "leg1-late-window":
            upload_files = [
                "trading-core/src/signals/LegInHedgeDetector.h",
                "trading-core/src/signals/LegInHedgeDetector.cpp",
                "trading-core/src/risk/RiskManager.cpp",
                "trading-core/src/main.cpp",
                "bot_config.py",
            ]
            sftp = client.open_sftp()
            for rel in upload_files:
                local = ROOT / rel
                remote = f"{PROJ}/{rel.replace(chr(92), '/')}"
                remote_parent = remote.rsplit("/", 1)[0]
                parts: list[str] = []
                for part in remote_parent.split("/"):
                    if not part:
                        continue
                    parts.append(part)
                    seg = "/" + "/".join(parts)
                    try:
                        sftp.stat(seg)
                    except OSError:
                        try:
                            sftp.mkdir(seg)
                        except OSError:
                            pass
                print(f"Upload {rel} -> {remote}", file=sys.stderr)
                sftp.put(str(local), remote)
            sftp.close()
            env_lines = [
                ("LIH_LEG1_MIN_SECONDS_REMAINING", "30"),
                ("LIH_MIN_SECONDS_REMAINING", "15"),
            ]
            steps = []
            for key, val in env_lines:
                steps.append(
                    f"grep -q '^{key}=' '{PROJ}/.env' && "
                    f"sed -i 's/^{key}=.*/{key}={val}/' '{PROJ}/.env' || "
                    f"echo '{key}={val}' >> '{PROJ}/.env'"
                )
            steps += [
                "pkill -f trading-core || true",
                "pkill -f start_bot.py || true",
                "sleep 2",
                BUILD_VPS,
                f"bash '{PROJ}/server_start_bot.sh'",
                "sleep 12",
                f"grep 'LIH config' '{PROJ}/bot.log' | tail -1",
            ]
            for step in steps:
                r = run(client, step, timeout=1800)
                if r != 0 and ("build.sh" in step or "build-lowmem" in step):
                    return r
            return 0

        if mode == "fix-build":
            # Upload LIH reliability fixes, compile only — does NOT touch .env or start bot.
            upload_files = [
                "clob_live.py",
                "dashboard_bridge.py",
                "scripts/live_lih_reconcile.py",
                "trading-core/src/exec/OrderRouter.cpp",
                "trading-core/src/exec/OrderRouter.h",
                "trading-core/src/risk/RiskManager.cpp",
                "trading-core/src/risk/RiskManager.h",
                "trading-core/src/main.cpp",
            ]
            sftp = client.open_sftp()
            for rel in upload_files:
                local = ROOT / rel
                remote = f"{PROJ}/{rel.replace(chr(92), '/')}"
                remote_parent = remote.rsplit("/", 1)[0]
                parts: list[str] = []
                for part in remote_parent.split("/"):
                    if not part:
                        continue
                    parts.append(part)
                    seg = "/" + "/".join(parts)
                    try:
                        sftp.stat(seg)
                    except OSError:
                        try:
                            sftp.mkdir(seg)
                        except OSError:
                            pass
                print(f"Upload {rel} -> {remote}", file=sys.stderr)
                sftp.put(str(local), remote)
            sftp.close()
            steps = [
                KILL_STALE_BUILD,
                BUILD_VPS,
                f"test -x '{PROJ}/build/trading-core' && ls -la '{PROJ}/build/trading-core'",
                "pgrep -af 'trading-core|start_bot' || echo BOT_NOT_RUNNING_OK",
                f"test -f '{PROJ}/logs/STOP_TRADING' && echo STOP_FLAG=present || echo STOP_FLAG=missing",
                f"grep -E '^LIH_ENABLED=|^LIH_ONE_SLOT|^RISK_MAX_CONCURRENT' '{PROJ}/.env'",
            ]
            for step in steps:
                r = run(client, step, timeout=1800)
                if r != 0 and ("build.sh" in step or "build-lowmem" in step):
                    return r
            return 0

        if mode == "stop-after-round" or mode == "pause-after-round":
            upload_files = [
                "clob_live.py",
                "dashboard_bridge.py",
                "scripts/live_lih_reconcile.py",
                "trading-core/src/exec/OrderRouter.cpp",
                "trading-core/src/exec/OrderRouter.h",
                "trading-core/src/risk/RiskManager.cpp",
                "trading-core/src/risk/RiskManager.h",
                "trading-core/src/main.cpp",
                "trading-core/src/state/StateStore.h",
            ]
            sftp = client.open_sftp()
            for rel in upload_files:
                local = ROOT / rel
                remote = f"{PROJ}/{rel.replace(chr(92), '/')}"
                remote_parent = remote.rsplit("/", 1)[0]
                parts: list[str] = []
                for part in remote_parent.split("/"):
                    if not part:
                        continue
                    parts.append(part)
                    seg = "/" + "/".join(parts)
                    try:
                        sftp.stat(seg)
                    except OSError:
                        try:
                            sftp.mkdir(seg)
                        except OSError:
                            pass
                print(f"Upload {rel} -> {remote}", file=sys.stderr)
                sftp.put(str(local), remote)
            sftp.close()
            env_fixes = [
                ("RISK_MAX_CONCURRENT_POSITIONS", "1"),
                ("LIH_TARGET_COMBINED", "0.95"),
                ("LIH_PAUSE_AFTER_ROUND", "false"),
                ("LIH_ONE_SLOT_GLOBAL", "true"),
                ("LIH_SESSION_MAX_LEGS", "0"),
            ]
            steps = [
                f"cd '{PROJ}' && .venv/bin/python -c \"import json; p='logs/live_state.json'; d=json.load(open(p)); d['lih_leg1_inflight']=[]; json.dump(d,open(p,'w')); print('cleared inflight')\"",
            ]
            for key, val in env_fixes:
                steps.append(
                    f"grep -q '^{key}=' '{PROJ}/.env' && "
                    f"sed -i 's/^{key}=.*/{key}={val}/' '{PROJ}/.env' || "
                    f"echo '{key}={val}' >> '{PROJ}/.env'"
                )
            steps.extend([
                f"cd '{PROJ}' && .venv/bin/python scripts/live_lih_reconcile.py",
                "pkill -f trading-core || true",
                "pkill -f start_bot.py || true",
                "sleep 2",
                BUILD_VPS,
                f"bash '{PROJ}/server_start_bot.sh'",
                "sleep 15",
                f"printf '%s' '{{\"control\":\"pause\",\"reason\":\"awaiting manual resume after deploy\"}}' "
                f"> '{PROJ}/logs/runtime_config.json'",
                "sleep 3",
                f"curl -s http://127.0.0.1:8081/api/config | python3 -c "
                "\"import sys,json; l=json.load(sys.stdin).get('live',{}); "
                "print('balance', l.get('balance')); print('openCount', l.get('openCount')); "
                "print('target', l.get('lihTargetCombined')); "
                "print('maxConcurrent', l.get('riskMaxConcurrentPositions')); "
                "print('session', l.get('lihSessionLegsUsed'),'/', l.get('lihSessionMaxLegs')); "
                "print('status', l.get('status'), l.get('statusReason','')); "
                "ops=l.get('openPositions') or []; "
                "[print(p.get('asset'), p.get('heldSide'), p.get('yesSize'), p.get('noSize')) for p in ops]\"",
                f"grep 'LIH config' '{PROJ}/bot.log' | tail -1",
            ])
            for step in steps:
                r = run(client, step, timeout=1800)
                if r != 0 and ("build.sh" in step or "build-lowmem" in step):
                    return r
            return 0

        if mode == "live-monitor":
            steps = [
                f"grep -q '^RISK_MAX_CONCURRENT_POSITIONS=' '{PROJ}/.env' && "
                f"sed -i 's/^RISK_MAX_CONCURRENT_POSITIONS=.*/RISK_MAX_CONCURRENT_POSITIONS=1/' '{PROJ}/.env' || "
                f"echo 'RISK_MAX_CONCURRENT_POSITIONS=1' >> '{PROJ}/.env'",
                f"printf '%s' '{{\"control\":\"reload_lih_state\",\"user\":\"deploy-monitor\"}}' "
                f"> '{PROJ}/logs/runtime_config.json'",
                f"cd '{PROJ}' && .venv/bin/python -c \"import json; p='logs/live_state.json'; d=json.load(open(p)); d['lih_leg1_inflight']=[]; json.dump(d,open(p,'w')); print('cleared inflight')\"",
                f"cd '{PROJ}' && .venv/bin/python fetch_balance.py 2>&1",
                "pkill -f trading-core || true",
                "pkill -f start_bot.py || true",
                "sleep 2",
                f"bash '{PROJ}/server_start_bot.sh'",
                "sleep 15",
                "pgrep -af 'start_bot|trading-core' || echo STOPPED",
                f"grep -E 'below LIH_MIN|Bal:|LIH_MIN|session' '{PROJ}/bot.log' | tail -5",
                f"curl -s http://127.0.0.1:8081/api/config | python3 -c "
                "\"import sys,json; l=json.load(sys.stdin).get('live',{}); "
                "print('balance', l.get('balance')); print('openCount', l.get('openCount')); "
                "print('session', l.get('lihSessionLegsUsed'),'/', l.get('lihSessionMaxLegs')); "
                "print('status', l.get('status'), l.get('statusReason',''))\"",
            ]
            for step in steps:
                run(client, step, timeout=120)
            return 0

        print(f"Unknown mode: {mode}", file=sys.stderr)
        return 1
    finally:
        client.close()


if __name__ == "__main__":
    raise SystemExit(main())
