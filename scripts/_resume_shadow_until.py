#!/usr/bin/env python3
"""Resume LIH shadow on VPS (LIVE_LIH_DRY_RUN=true, no CLOB orders) until a local wall-clock hour."""
from __future__ import annotations

import argparse
import json
import sys
from datetime import datetime, timedelta
from pathlib import Path
from zoneinfo import ZoneInfo

import paramiko

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from remote_deploy import HOST, PROJ, USER, load_password, run  # noqa: E402


def ro(c, cmd, t=60):
    _, o, e = c.exec_command(cmd, timeout=t)
    return (o.read() + e.read()).decode(errors="replace").strip()


def main() -> int:
    ap = argparse.ArgumentParser(description="Resume shadow until hour (local TZ)")
    ap.add_argument("--until-hour", type=int, default=20, help="Stop at this hour (0-23), default 20:00")
    ap.add_argument("--tz", default="Asia/Shanghai", help="Timezone for --until-hour")
    ap.add_argument("--restart", action="store_true", help="Restart bot via server_start_bot.sh first")
    args = ap.parse_args()

    tz = ZoneInfo(args.tz)
    now = datetime.now(tz)
    stop_at = now.replace(hour=args.until_hour, minute=0, second=0, microsecond=0)
    if stop_at <= now:
        stop_at += timedelta(days=1)
    stop_local = stop_at.strftime("%Y-%m-%d %H:%M %Z")
    stop_utc = stop_at.astimezone(ZoneInfo("UTC")).strftime("%H:%M UTC")

    c = paramiko.SSHClient()
    c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    c.connect(HOST, username=USER, password=load_password(), timeout=60)
    try:
        patch = {"LIVE_LIH_DRY_RUN": "true"}
        py = (
            "from bot_config import update_env; "
            f"print(update_env({repr(patch)}))"
        )
        print("=== ENV ===")
        print(ro(c, f"cd '{PROJ}' && .venv/bin/python -c \"{py}\""))

        if args.restart:
            print("\n=== RESTART BOT ===")
            run(c, f"bash '{PROJ}/server_start_bot.sh'", timeout=90)
            run(c, "sleep 10", timeout=20)

        resume = {
            "patch": {"LIVE_LIH_DRY_RUN": "true"},
            "control": "resume",
            "reason": f"Shadow session until {stop_local}",
            "user": "resume-shadow-until",
        }
        print("\n=== RESUME SHADOW ===")
        ro(c, f"rm -f '{PROJ}/logs/STOP_TRADING'")
        ro(c, f"printf '%s' '{json.dumps(resume, ensure_ascii=False)}' > '{PROJ}/logs/runtime_config.json'")
        run(c, "sleep 6", timeout=15)

        print(ro(c, f"grep -E '^(LIVE_LIH_DRY_RUN|RISK_MAX_CONCURRENT)' '{PROJ}/.env'"))
        print("\n=== STATUS ===")
        print(ro(c, "pgrep -af 'start_bot|trading-core' | grep -v pgrep || echo DOWN"))
        print(
            ro(
                c,
                f"curl -s http://127.0.0.1:8081/api/config | {PROJ}/.venv/bin/python -c "
                "\"import sys,json; l=json.load(sys.stdin).get('live',{}); "
                "print('status', l.get('status'), 'reason', l.get('statusReason'), "
                "'open', l.get('openCount'), 'sess', str(l.get('lihSessionLegsUsed'))+'/'+str(l.get('lihSessionMaxLegs')))\"",
            )
        )
        print(ro(c, f"grep -E 'LIH dry-run|SHADOW|Resume|STOP_TRADING' '{PROJ}/logs/bridge.log' | tail -6"))

        stop_script = f"""#!/bin/bash
set -euo pipefail
cd '{PROJ}'
touch logs/STOP_TRADING
printf '%s' '{{"control":"pause","reason":"Shadow session ended {stop_local}","user":"shadow-until-cron"}}' > logs/runtime_config.json
echo "[shadow-until] paused at $(date -Iseconds)"
"""
        remote_stop = f"{PROJ}/scripts/_shadow_stop_scheduled.sh"
        sftp = c.open_sftp()
        with sftp.file(remote_stop, "w") as f:
            f.write(stop_script)
        sftp.chmod(remote_stop, 0o755)
        sftp.close()

        # One-shot `at` if available, else cron marker for exact minute
        at_spec = stop_at.strftime("%H:%M %Y-%m-%d")
        cron_line = (
            f"{stop_at.minute} {stop_at.hour} {stop_at.day} {stop_at.month} * "
            f"PROJ={PROJ} bash {remote_stop} >> {PROJ}/logs/shadow_until.log 2>&1 "
            f"# shadow-until-{stop_at.strftime('%Y%m%d')}"
        )
        print(f"\n=== SCHEDULE STOP @ {stop_local} ({stop_utc}) ===")
        at_out = ro(
            c,
            f"command -v at >/dev/null && echo 'bash {remote_stop}' | at '{at_spec}' 2>&1 || echo AT_MISSING",
        )
        print(at_out)
        if "AT_MISSING" in at_out:
            ro(
                c,
                f"(crontab -l 2>/dev/null | grep -v 'shadow-until-' ; echo '{cron_line}') | crontab -",
            )
            print("installed one-shot cron:", cron_line)

        print(f"\nShadow 已启动，计划 {stop_local} 自动暂停。")
        print("观察: tail -f logs/bridge.log | grep SHADOW  或 Web 仪表盘")
        return 0
    finally:
        c.close()


if __name__ == "__main__":
    raise SystemExit(main())
