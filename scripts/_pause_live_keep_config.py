#!/usr/bin/env python3
"""Pause live trading — strategy config stays in .env, no orders."""
import sys
from pathlib import Path
import paramiko

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from remote_deploy import HOST, PROJ, USER, load_password

PATCH = {"LIVE_LIH_DRY_RUN": "true"}

c = paramiko.SSHClient()
c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
c.connect(HOST, username=USER, password=load_password(), timeout=60)
try:
    sftp = c.open_sftp()
    sftp.put(str(Path(__file__).resolve().parents[1] / "bot_config.py"), f"{PROJ}/bot_config.py")
    sftp.close()
    py = (
        "from bot_config import update_env; "
        f"print(update_env({repr(PATCH)}))"
    )
    _, o, _ = c.exec_command(f"cd {PROJ} && .venv/bin/python -c \"{py}\"", timeout=30)
    print("env:", o.read().decode())
    for cmd in [
        "pkill -f trading-core 2>/dev/null; pkill -f start_bot.py 2>/dev/null; sleep 1",
        f"touch {PROJ}/logs/STOP_TRADING",
        f"grep -E '^(LIVE_LIH_DRY_RUN|LIH_LEG1_MODE|LIH_LEG1_TRIGGER|LIH_ONE_SLOT)' {PROJ}/.env",
        "pgrep -af trading-core || echo core_stopped",
    ]:
        _, o, _ = c.exec_command(cmd, timeout=20)
        print(o.read().decode().strip())
finally:
    c.close()
