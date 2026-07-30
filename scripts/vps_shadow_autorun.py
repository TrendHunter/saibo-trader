#!/usr/bin/env python3
"""Shared: resume shadow, clear auto-stop jobs, optional hourly sample cron."""
from __future__ import annotations

import json
import sys
import time
from pathlib import Path

import paramiko

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from remote_deploy import HOST, PROJ, ROOT, USER, load_password, run  # noqa: E402

HOURLY_CRON = (
    "17 * * * * cd /opt/polymarket-bot && "
    ".venv/bin/python scripts/shadow_hourly_sample.py >> logs/shadow_hourly.log 2>&1"
)

# Core sets this on every process start until explicit resume.
STARTUP_PAUSE_HINT = "manual Web resume required"


def ro(c, cmd, t=90):
    _, o, e = c.exec_command(cmd, timeout=t, get_pty=True)
    return (o.read() + e.read()).decode(errors="replace").strip()


def fetch_live(c) -> dict:
    raw = ro(c, "curl -s -m 8 http://127.0.0.1:8081/api/config", t=15)
    try:
        return json.loads(raw).get("live", {}) or {}
    except json.JSONDecodeError:
        return {}


def is_startup_paused(live: dict) -> bool:
    reason = str(live.get("statusReason") or "")
    status = live.get("status")
    if STARTUP_PAUSE_HINT in reason:
        return True
    # status=3 historically used for startup pause in this deploy
    if status == 3 and "startup" in reason.lower():
        return True
    return False


def ensure_shadow_running(c, reason: str = "deploy auto-resume") -> None:
    """Clear STOP flag and drop a resume control file for the core event loop."""
    resume = {"control": "resume", "reason": reason, "user": "autorun"}
    payload = json.dumps(resume, ensure_ascii=False)
    # Write via remote Python (atomic-ish) — avoids shell/printf quoting races.
    py = (
        "import json, pathlib; "
        f"p=pathlib.Path('{PROJ}/logs'); p.mkdir(parents=True, exist_ok=True); "
        f"(p/'STOP_TRADING').unlink(missing_ok=True); "
        f"(p/'runtime_config.json').write_text({payload!r}, encoding='utf-8')"
    )
    ro(c, f"python3 -c {json.dumps(py)}", t=30)


def wait_core_ready(c, *, timeout_sec: float = 90.0) -> bool:
    """Wait until bridge API answers (core/bridge up after restart)."""
    deadline = time.time() + timeout_sec
    while time.time() < deadline:
        live = fetch_live(c)
        if live:
            return True
        # also accept process present
        procs = ro(c, "pgrep -af trading-core | head -1", t=15)
        if "trading-core" in procs and time.time() + 15 > deadline:
            # process up but api slow — keep waiting
            pass
        time.sleep(2)
    return bool(fetch_live(c))


def wait_shadow_resumed(
    c,
    *,
    reason: str = "deploy auto-resume",
    timeout_sec: float = 90.0,
    retry_every_sec: float = 5.0,
) -> dict:
    """Write resume, re-poke until startup pause clears. Returns live snapshot + ok flag."""
    deadline = time.time() + timeout_sec
    attempts = 0
    live: dict = {}
    while time.time() < deadline:
        attempts += 1
        ensure_shadow_running(c, f"{reason} (try {attempts})")
        time.sleep(retry_every_sec)
        live = fetch_live(c)
        if live and not is_startup_paused(live):
            live = dict(live)
            live["_resume_ok"] = True
            live["_resume_attempts"] = attempts
            return live
    live = dict(fetch_live(c) or {})
    live["_resume_ok"] = not is_startup_paused(live)
    live["_resume_attempts"] = attempts
    return live


def clear_scheduled_stops(c) -> str:
    """Remove all at jobs (legacy session-stop schedulers)."""
    return ro(
        c,
        "atq 2>/dev/null | awk '{print $1}' | xargs -r atrm 2>/dev/null; "
        "atq 2>/dev/null || echo 'no-at'",
    )


def install_hourly_sample(c) -> None:
    sftp = c.open_sftp()
    sftp.put(str(ROOT / "scripts/shadow_hourly_sample.py"), f"{PROJ}/scripts/shadow_hourly_sample.py")
    sftp.close()
    ro(c, f"chmod +x '{PROJ}/scripts/shadow_hourly_sample.py'")
    ro(
        c,
        f"(crontab -l 2>/dev/null | grep -v shadow_hourly_sample; echo '{HOURLY_CRON}') | crontab -",
    )


def upload_and_restart(c, upload_bot_config: bool = True) -> None:
    if upload_bot_config:
        sftp = c.open_sftp()
        sftp.put(str(ROOT / "bot_config.py"), f"{PROJ}/bot_config.py")
        sftp.close()
    run(c, f"bash '{PROJ}/server_start_bot.sh'", timeout=120)
    run(c, "sleep 8", timeout=15)


def finalize_shadow_deploy(
    c,
    *,
    marker: str | None = None,
    reason: str = "deploy",
    restart: bool = False,
    require_resume: bool = True,
    resume_timeout_sec: float = 90.0,
) -> dict:
    if restart:
        upload_and_restart(c)
        # Core forces PAUSE at end of init — wait for API before first resume poke,
        # otherwise resume can be consumed by a dying process or written too early.
        ready = wait_core_ready(c, timeout_sec=60)
        if not ready:
            print("WARN: bridge api not ready after restart; still attempting resume")
    clear_scheduled_stops(c)
    live = wait_shadow_resumed(
        c, reason=reason, timeout_sec=resume_timeout_sec, retry_every_sec=5.0
    )
    install_hourly_sample(c)
    if marker:
        # Append marker only after resume attempt so "since marker" aligns with live trading.
        ro(c, f"echo '# {marker}' >> '{PROJ}/logs/shadow_trades.jsonl'")
    run(c, "sleep 2", timeout=8)
    live2 = fetch_live(c)
    if live2:
        live.update(live2)
        live["_resume_ok"] = not is_startup_paused(live2)
    try:
        ro(c, f"cd '{PROJ}' && .venv/bin/python scripts/shadow_hourly_sample.py")
    except Exception as e:
        print("WARN hourly sample:", e)

    out = {
        "status": live.get("status"),
        "statusReason": live.get("statusReason"),
        "dryRun": live.get("liveLihDryRun"),
        "stop": ro(c, f"test -f '{PROJ}/logs/STOP_TRADING' && echo yes || echo no"),
        "resumed": bool(live.get("_resume_ok")),
        "resumeAttempts": live.get("_resume_attempts"),
    }
    if require_resume and not out["resumed"]:
        raise RuntimeError(
            f"shadow still paused after deploy: status={out['status']} "
            f"reason={out['statusReason']!r} stop={out['stop']}"
        )
    return out


def connect():
    c = paramiko.SSHClient()
    c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    c.connect(HOST, username=USER, password=load_password(), timeout=60)
    return c
