#!/usr/bin/env python3
"""Server CLI entry: preflight + config summary + dashboard_bridge (with terminal output)."""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import sys
from pathlib import Path

from dotenv import load_dotenv

from live_preflight import PREFLIGHT_PATH, print_report, run_preflight

PRELIVE_PATH = Path(os.getenv("PRELIVE_LIH_PATH", "logs/prelive_lih.json"))


def _env_bool(key: str, default: bool = True) -> bool:
    raw = os.getenv(key, "").strip().lower()
    if not raw:
        return default
    if raw in ("false", "0", "no", "off"):
        return False
    if raw in ("true", "1", "yes", "on"):
        return True
    return default


def _enabled_assets(prefix: str, assets: tuple[str, ...]) -> str:
    on = [a.upper() for a in assets if _env_bool(f"{prefix}_{a.upper()}", True)]
    return " · ".join(on) if on else "全关"


def print_config_summary() -> None:
    load_dotenv()
    mode = "实盘 LIVE"
    strategy = "LIH 分腿对冲"

    print("=" * 60)
    print(f"  运行配置摘要  |  {mode}  |  {strategy}")
    print("=" * 60)
    print(f"  WS   : ws://{os.getenv('WS_HOST', '0.0.0.0')}:{os.getenv('WS_PORT', '8080')}")
    http_bind = os.getenv("HTTP_BIND", "127.0.0.1")
    print(f"  API  : http://{http_bind}:{os.getenv('HTTP_PORT', '8081')}")
    print(f"  5m   : {'开' if _env_bool('DH_ENABLE_5M') else '关'}  →  {_enabled_assets('DH_ENABLE_5M', ('btc', 'eth', 'sol'))}")
    print(f"  15m  : {'开' if _env_bool('DH_ENABLE_15M') else '关'}  →  {_enabled_assets('DH_ENABLE_15M', ('btc', 'eth'))}")
    dry = _env_bool("LIVE_LIH_DRY_RUN", True)
    print(
        f"  LIH  : mode={os.getenv('LIH_LEG1_MODE', 'trigger')}  "
        f"target={os.getenv('LIH_TARGET_COMBINED', '0.94')}  "
        f"shares={os.getenv('LIH_LEG1_SHARES', '20')}"
    )
    print(f"         live shadow (LIVE_LIH_DRY_RUN)={'开' if dry else '关 — 会真下单'}")
    print(
        f"  风控 : pos={os.getenv('RISK_MAX_POSITION_FRACTION', '0.08')}  "
        f"daily={os.getenv('RISK_DAILY_LOSS_LIMIT', '0.20')}  "
        f"max_pos={os.getenv('RISK_MAX_CONCURRENT_POSITIONS', '3')}  "
        f"slot_cap={os.getenv('LIH_MAX_USDC_PER_SLOT', '0') or 'bal×pos'}"
    )
    funder = os.getenv("POLYMARKET_FUNDER", "")
    signer = os.getenv("POLYMARKET_SIGNER", funder)
    print(f"  钱包 : funder={funder or '(未设置)'}  signer={signer or '(未设置)'}")
    print("=" * 60 + "\n")


def run_preflight_step() -> dict:
    report = run_preflight()
    PREFLIGHT_PATH.parent.mkdir(parents=True, exist_ok=True)
    PREFLIGHT_PATH.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print_report(report)
    return report


def run_prelive_step(*, allow_live: bool = False, min_shadow_leg1: int = 0, live_dry: bool | None = None) -> dict:
    from prelive_lih_check import print_report as print_prelive
    from prelive_lih_check import run_prelive_check

    if live_dry is None:
        load_dotenv()
        live_dry = _env_bool("LIVE_LIH_DRY_RUN", True)

    since_h = float(os.getenv("PRELIVE_LOG_HOURS", "24") or "24")
    report = run_prelive_check(
        require_shadow=not allow_live,
        min_shadow_leg1=min_shadow_leg1,
        since_hours=since_h if live_dry else 0.0,
        since_baseline=not live_dry,
    )
    PRELIVE_PATH.parent.mkdir(parents=True, exist_ok=True)
    PRELIVE_PATH.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print_prelive(report)
    return report


def maybe_reconcile_live_lih() -> None:
    """Rebuild live LIH memory from chain when real live mode starts (not shadow)."""
    load_dotenv()
    if _env_bool("LIVE_LIH_DRY_RUN", True):
        print("[reconcile] skip — shadow mode (LIVE_LIH_DRY_RUN=true)", file=sys.stderr)
        return
    if not _env_bool("LIVE_LIH_RECONCILE_ON_START", False):
        print("[reconcile] skip — LIVE_LIH_RECONCILE_ON_START=false", file=sys.stderr)
        return
    script = Path(__file__).resolve().parent / "scripts" / "live_lih_reconcile.py"
    if not script.is_file():
        print(f"[reconcile] skip — missing {script}", file=sys.stderr)
        return
    import subprocess

    print("[reconcile] syncing open LIH from Polymarket activity …", file=sys.stderr)
    proc = subprocess.run(
        [sys.executable, str(script), "--merge"],
        cwd=str(Path(__file__).resolve().parent),
        capture_output=True,
        text=True,
    )
    if proc.stdout.strip():
        print(proc.stdout.strip(), file=sys.stderr)
    if proc.returncode != 0:
        print(proc.stderr.strip() or f"[reconcile] exit {proc.returncode}", file=sys.stderr)
    else:
        print("[reconcile] done", file=sys.stderr)


def main() -> int:
    parser = argparse.ArgumentParser(description="启动 bot（自检 + 配置摘要 + bridge/core）")
    parser.add_argument("--skip-preflight", action="store_true", help="跳过自检（Docker entrypoint 已跑过时）")
    parser.add_argument("--skip-prelive", action="store_true", help="跳过 LIH prelive（日常重启用）")
    parser.add_argument("--preflight-only", action="store_true", help="只跑自检并退出")
    parser.add_argument("--prelive-only", action="store_true", help="只跑 LIH 上线前安全检查")
    parser.add_argument(
        "--allow-live",
        action="store_true",
        help="prelive 不强制 LIVE_LIH_DRY_RUN=true（真下单前用）",
    )
    parser.add_argument(
        "--min-shadow-leg1",
        type=int,
        default=0,
        help="prelive 要求日志中至少 N 条 LEG1 样本",
    )
    parser.add_argument("--json-only", action="store_true", help="只输出自检 JSON（供脚本用）")
    args = parser.parse_args()

    if args.prelive_only:
        report = run_prelive_step(allow_live=args.allow_live, min_shadow_leg1=args.min_shadow_leg1)
        return 0 if report.get("ok") else 1

    if args.preflight_only or args.json_only:
        report = run_preflight()
        PREFLIGHT_PATH.parent.mkdir(parents=True, exist_ok=True)
        PREFLIGHT_PATH.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
        if args.json_only:
            print(json.dumps(report, ensure_ascii=False))
        else:
            print_report(report)
        if not report.get("ok"):
            return 1
        return 0

    if not args.skip_preflight:
        report = run_preflight_step()
        if not report.get("ok"):
            print("❌ 自检未通过，已中止启动。", file=sys.stderr)
            return 1
    else:
        if PREFLIGHT_PATH.is_file():
            try:
                report = json.loads(PREFLIGHT_PATH.read_text(encoding="utf-8"))
                ok = report.get("ok", True)
                mode = (report.get("mode") or "live").upper()
                print(f"[preflight] 使用已有报告 ({mode}) ok={ok}", file=sys.stderr)
            except Exception:
                pass

    load_dotenv()
    live_dry = _env_bool("LIVE_LIH_DRY_RUN", True)
    skip_prelive = args.skip_prelive or _env_bool("START_SKIP_PRELIVE", False)
    if not skip_prelive:
        allow = args.allow_live or not live_dry
        min_samples = args.min_shadow_leg1 if not live_dry else max(args.min_shadow_leg1, 1)
        prelive = run_prelive_step(
            allow_live=allow,
            min_shadow_leg1=min_samples if not live_dry else args.min_shadow_leg1,
            live_dry=live_dry,
        )
        if not live_dry and not prelive.get("ok"):
            print("❌ LIH 上线前检查未通过，已中止启动。先 shadow 观察或修日志中的重复 LEG1。", file=sys.stderr)
            return 1
        if live_dry and not prelive.get("ok"):
            print("⚠️  LIH prelive 有警告（shadow 模式仍启动，请先处理重复 LEG1）", file=sys.stderr)
    else:
        print("[prelive] 跳过（START_SKIP_PRELIVE / --skip-prelive）", file=sys.stderr)

    maybe_reconcile_live_lih()

    print_config_summary()
    os.environ["PREFLIGHT_SKIP"] = "1"

    print("[启动] dashboard_bridge + trading-core …", file=sys.stderr)
    print("       Ctrl+C 停止 · 日志另见 logs/bot.log\n", file=sys.stderr)

    from dashboard_bridge import main as bridge_main

    try:
        asyncio.run(bridge_main())
    except KeyboardInterrupt:
        print("\n已停止。", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
