"""Read/write whitelisted .env keys and append audit events."""

from __future__ import annotations

import json
import os
import re
import time
from pathlib import Path
from typing import Any

ENV_PATH = Path(os.getenv("ENV_PATH", ".env"))
AUDIT_PATH = Path(os.getenv("AUDIT_PATH", "logs/audit.jsonl"))
RUNTIME_CONFIG_PATH = Path(os.getenv("RUNTIME_CONFIG_PATH", "logs/runtime_config.json"))

BOOL_KEYS = frozenset({
    "BINANCE_FEED_ENABLED",
    "BOOK_AWARE_DETECT",
    "DH_ENABLE_5M",
    "DH_ENABLE_15M",
    "DH_ENABLE_5M_BTC",
    "DH_ENABLE_5M_ETH",
    "DH_ENABLE_5M_SOL",
    "DH_ENABLE_15M_BTC",
    "DH_ENABLE_15M_ETH",
    "LIH_USE_MIRROR",
    "LIVE_LIH_DRY_RUN",
    "LIH_ALLOW_OVER_TARGET",
    "LIH_ONE_SLOT_GLOBAL",
    "LIH_LEG1_TREND_ALIGN",
    "LIH_ENDGAME_MINIMIZE_GAP",
    "LIH_ENDGAME_LADDER_ENABLED",
    "LIH_HEDGE_FEASIBLE_ENTRY",
    "LIH_VWAP_ENTRY_GATE",
    "LIH_PARALLEL_CLIP_HEDGE",
    "LIH_UNWIND_ENABLED",
    "LIH_OPEN_GAP",
    "LIH_MM2_MODE",
    "LIH_SKIP_PARTIAL_WINDOW_ON_START",
})

ALLOWED_KEYS = {
    "BOOK_AWARE_DETECT",
    "DH_ENABLE_5M",
    "DH_ENABLE_15M",
    "DH_ENABLE_5M_BTC",
    "DH_ENABLE_5M_ETH",
    "DH_ENABLE_5M_SOL",
    "DH_ENABLE_15M_BTC",
    "DH_ENABLE_15M_ETH",
    "LIH_USE_MIRROR",
    "LIVE_LIH_DRY_RUN",
    "SHADOW_BANKROLL_USDC",
    "SHADOW_WINDOW_LOG",
    "SHADOW_WINDOW_ASSET",
    "SHADOW_WINDOW_MINUTES",
    "LIH_LEG1_MAX_PRICE",
    "LIH_LEG1_MODE",
    "LIH_LEG1_TRIGGER_MIN",
    "LIH_LEG1_TRIGGER_MAX",
    "LIH_QUOTE_MODE",
    "LIH_LEG1_START_DELAY_SEC",
    "LIH_LEG1_TREND_ALIGN",
    "LIH_ENDGAME_SOFT_CAP",
    "LIH_ENDGAME_SECS",
    "LIH_ENDGAME_OVERRIDE_SECS",
    "LIH_ENDGAME_OVERRIDE_COOLDOWN",
    "LIH_ENDGAME_MINIMIZE_GAP",
    "LIH_ENDGAME_LADDER_ENABLED",
    "LIH_ENDGAME_LADDER_SECS",
    "LIH_ENDGAME_LADDER_START",
    "LIH_ENDGAME_LADDER_END",
    "LIH_ENDGAME_LADDER_STEP",
    "LIH_MAX_ENTRY_MARGINAL",
    "LIH_TARGET_COMBINED",
    "LIH_MID_SOFT_CAP",
    "LIH_MID_SOFT_START_SECS",
    "LIH_HEDGE_FEASIBLE_ENTRY",
    "LIH_HEDGE_FEASIBLE_CAP",
    "LIH_VWAP_ENTRY_GATE",
    "LIH_VWAP_ENTRY_CAP",
    "LIH_VWAP_DEPTH_RATIO",
    "LIH_MIN_EDGE_USDC",
    "LIH_MIN_EDGE_PER_SHARE",
    "LIH_UNWIND_ENABLED",
    "LIH_UNWIND_SECS",
    "LIH_UNWIND_COOLDOWN",
    "LIH_MIN_SECONDS_REMAINING",
    "LIH_LEG1_MIN_SECONDS_REMAINING",
    "LIH_COOLDOWN_SECONDS",
    "LIH_LEG1_COOLDOWN_SECONDS",
    "LIH_REBALANCE_COOLDOWN_SECONDS",
    "LIH_LEG1_SHARES",
    "LIH_LEG1_CLIP_SHARES",
    "LIH_PARALLEL_CLIP_HEDGE",
    "LIH_PARALLEL_HEDGE_MAX_COMBINED",
    "LIH_EARLY_HEDGE_MAX_COMBINED",
    "LIH_HEAVY_CLIP_SHARES",
    "LIH_HEAVY_MAX_PRICE",
    "LIH_MAX_GAP_SHARES",
    "LIH_GAP_HEDGE_MAX_COMBINED",
    "LIH_OPEN_GAP",
    "LIH_MM2_MODE",
    "LIH_MM2_MIN_SPOT_BPS",
    "LIH_MM2_ENTRY_MAX_SECS_LEFT",
    "LIH_MM2_ENTRY_MIN_SECS_LEFT",
    "LIH_MM2_FAVORITE_MIN",
    "LIH_MM2_HEAVY_DELAY_SEC",
    "LIH_MM2_SCALE_CLIP",
    "LIH_MM2_SCALE_AGAINST_STOP_BPS",
    "LIH_SKIP_PARTIAL_WINDOW_ON_START",
    "LIH_MAX_REBALANCE_SHARES",
    "LIH_MAX_MATCHED_SHARES",
    "LIH_MAX_USDC_PER_SLOT",
    "LIH_FORCE_BALANCE_SECS",
    "LIH_FLEX_DILUTE_RATIO",
    "LIH_ALLOW_OVER_TARGET",
    "LIH_REBALANCE_MODE",
    "LIH_ONE_SLOT_GLOBAL",
    "LIH_SESSION_MAX_LEGS",
    "LIH_MIN_BALANCE_USDC",
    "RISK_MAX_POSITION_FRACTION",
    "RISK_DAILY_LOSS_LIMIT",
    "RISK_TOTAL_DRAWDOWN_KILL",
    "RISK_MAX_CONCURRENT_POSITIONS",
    "FEE_RATE",
    "BINANCE_FEED_ENABLED",
}

PUBLIC_CONFIG_KEYS = sorted(ALLOWED_KEYS)


def _sanitize_audit_label(text: str, *, max_len: int = 64, default: str = "web") -> str:
    s = str(text or default).strip()[:max_len]
    if not re.fullmatch(r"[A-Za-z0-9_.@-]+", s):
        return default
    lowered = s.lower()
    for token in ("union", "select", "insert", "update", "delete", "drop", "alter", "exec", "--", "/*"):
        if token in lowered:
            return default
    return s


def _sanitize_audit_reason(text: str) -> str:
    s = str(text or "").strip()[:200]
    if not s:
        return ""
    lowered = s.lower()
    for token in ("union", "select", "insert", "update", "delete", "drop", "--", "/*", ";"):
        if token in lowered:
            return ""
    return s


def _ensure_parent(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)


def read_env(path: Path | None = None) -> dict[str, str]:
    path = path or ENV_PATH
    result: dict[str, str] = {}
    if not path.exists():
        return result
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, val = line.split("=", 1)
        key = key.strip()
        val = val.strip()
        if len(val) >= 2 and val[0] == val[-1] and val[0] in "\"'":
            val = val[1:-1]
        result[key] = val
    return result


def public_config(path: Path | None = None) -> dict[str, str]:
    env = read_env(path)
    return {k: env[k] for k in PUBLIC_CONFIG_KEYS if k in env}


def _parse_bool(text: str) -> str:
    lowered = text.lower()
    if lowered in ("true", "1", "yes", "on"):
        return "true"
    if lowered in ("false", "0", "no", "off"):
        return "false"
    raise ValueError(f"{text!r} must be true/false")


def _parse_float(text: str, key: str) -> float:
    raw = text.strip()
    lowered = raw.lower()
    if not raw or lowered in ("undefined", "null", "nan"):
        raise ValueError(f"{key} 无效，请刷新页面后重试")
    try:
        num = float(raw)
    except ValueError:
        raise ValueError(f"{key} 必须是有效数字")
    if num != num:  # NaN
        raise ValueError(f"{key} 无效，请刷新页面后重试")
    return num


def _validate_patch(patch: dict[str, Any]) -> dict[str, str]:
    cleaned: dict[str, str] = {}
    for key, value in patch.items():
        if key not in ALLOWED_KEYS:
            raise ValueError(f"Key not allowed: {key}")
        text = str(value).strip()
        if key in BOOL_KEYS:
            text = _parse_bool(text)
        elif key == "RISK_MAX_CONCURRENT_POSITIONS":
            n = int(_parse_float(text, key))
            if n < 0 or n > 50:
                raise ValueError("RISK_MAX_CONCURRENT_POSITIONS must be 0-50 (0 = no new leg1, rebalance only)")
            text = str(n)
        elif key.startswith("RISK_") or key in (
            "LIH_LEG1_MAX_PRICE",
            "LIH_TARGET_COMBINED",
            "LIH_MIN_SECONDS_REMAINING",
            "LIH_LEG1_MIN_SECONDS_REMAINING",
            "LIH_COOLDOWN_SECONDS",
            "LIH_LEG1_COOLDOWN_SECONDS",
            "LIH_REBALANCE_COOLDOWN_SECONDS",
            "LIH_LEG1_SHARES",
            "LIH_LEG1_CLIP_SHARES",
            "LIH_MAX_REBALANCE_SHARES",
            "LIH_MAX_MATCHED_SHARES",
            "LIH_FORCE_BALANCE_SECS",
            "LIH_FLEX_DILUTE_RATIO",
            "LIH_SESSION_MAX_LEGS",
            "LIH_MIN_BALANCE_USDC",
            "SHADOW_BANKROLL_USDC",
            "LIH_ENDGAME_SECS",
            "LIH_ENDGAME_SOFT_CAP",
            "LIH_ENDGAME_OVERRIDE_SECS",
            "LIH_ENDGAME_OVERRIDE_COOLDOWN",
            "LIH_ENDGAME_LADDER_SECS",
            "LIH_ENDGAME_LADDER_START",
            "LIH_ENDGAME_LADDER_END",
            "LIH_ENDGAME_LADDER_STEP",
            "LIH_MID_SOFT_CAP",
            "LIH_MID_SOFT_START_SECS",
            "LIH_HEDGE_FEASIBLE_CAP",
            "LIH_VWAP_ENTRY_CAP",
            "LIH_VWAP_DEPTH_RATIO",
            "LIH_MIN_EDGE_USDC",
            "LIH_MIN_EDGE_PER_SHARE",
            "LIH_UNWIND_SECS",
            "LIH_UNWIND_COOLDOWN",
            "LIH_EARLY_HEDGE_MAX_COMBINED",
            "LIH_PARALLEL_HEDGE_MAX_COMBINED",
            "FEE_RATE",
        ):
            num = _parse_float(text, key)
            if key == "RISK_MAX_POSITION_FRACTION" and not (0.01 <= num <= 1.0):
                raise ValueError("RISK_MAX_POSITION_FRACTION must be 0.01-1.0")
            if key == "RISK_DAILY_LOSS_LIMIT" and not (0.01 <= num <= 1.0):
                raise ValueError("RISK_DAILY_LOSS_LIMIT must be 0.01-1.0")
            if key == "RISK_TOTAL_DRAWDOWN_KILL" and not (0.05 <= num <= 1.0):
                raise ValueError("RISK_TOTAL_DRAWDOWN_KILL must be 0.05-1.0")
            if key == "FEE_RATE" and not (0.0 <= num <= 0.1):
                raise ValueError("FEE_RATE must be 0.0-0.1")
            if key in (
                "LIH_COOLDOWN_SECONDS",
                "LIH_LEG1_COOLDOWN_SECONDS",
                "LIH_REBALANCE_COOLDOWN_SECONDS",
                "LIH_MIN_SECONDS_REMAINING",
                "LIH_LEG1_MIN_SECONDS_REMAINING",
            ) and num < 0:
                raise ValueError(f"{key} must be >= 0")
            if key in ("LIH_LEG1_SHARES", "LIH_LEG1_CLIP_SHARES", "LIH_MAX_REBALANCE_SHARES", "LIH_MAX_MATCHED_SHARES") and num < 0:
                raise ValueError(f"{key} must be >= 0")
            if key == "LIH_MAX_USDC_PER_SLOT" and num < 0:
                raise ValueError("LIH_MAX_USDC_PER_SLOT must be >= 0")
            if key == "LIH_FORCE_BALANCE_SECS" and num < 0:
                raise ValueError(f"{key} must be >= 0")
            if key in (
                "LIH_ENDGAME_SECS",
                "LIH_ENDGAME_OVERRIDE_SECS",
                "LIH_ENDGAME_OVERRIDE_COOLDOWN",
                "LIH_ENDGAME_LADDER_SECS",
            ) and num < 0:
                raise ValueError(f"{key} must be >= 0")
            if key in (
                "LIH_ENDGAME_SOFT_CAP",
                "LIH_ENDGAME_LADDER_START",
                "LIH_ENDGAME_LADDER_END",
                "LIH_ENDGAME_LADDER_STEP",
                "LIH_MID_SOFT_CAP",
                "LIH_HEDGE_FEASIBLE_CAP",
                "LIH_VWAP_ENTRY_CAP",
            ) and not (0.0 < num <= 1.5):
                raise ValueError(f"{key} must be 0-1.5")
            if key == "LIH_VWAP_DEPTH_RATIO" and not (0.0 < num <= 1.0):
                raise ValueError("LIH_VWAP_DEPTH_RATIO must be 0-1")
            if key in ("LIH_MIN_EDGE_USDC", "LIH_MIN_EDGE_PER_SHARE") and num < 0:
                raise ValueError(f"{key} must be >= 0")
            if key in ("LIH_UNWIND_SECS", "LIH_UNWIND_COOLDOWN") and num < 0:
                raise ValueError(f"{key} must be >= 0")
            if key == "LIH_MID_SOFT_START_SECS" and num < 0:
                raise ValueError(f"{key} must be >= 0")
            if key == "LIH_SESSION_MAX_LEGS" and (num < 0 or num > 20):
                raise ValueError("LIH_SESSION_MAX_LEGS must be 0-20 (0 = unlimited)")
            if key == "LIH_MIN_BALANCE_USDC" and num < 0:
                raise ValueError("LIH_MIN_BALANCE_USDC must be >= 0 (0 = off)")
            if key == "SHADOW_BANKROLL_USDC" and num < 0:
                raise ValueError("SHADOW_BANKROLL_USDC must be >= 0 (0 = use wallet)")
            if key == "LIH_FLEX_DILUTE_RATIO" and not (0.0 < num <= 1.0):
                raise ValueError("LIH_FLEX_DILUTE_RATIO must be 0-1")
            text = str(num)
        elif key == "LIH_REBALANCE_MODE":
            mode = text.lower()
            if mode not in ("flex", "simple"):
                raise ValueError("LIH_REBALANCE_MODE must be flex or simple")
            text = mode
        cleaned[key] = text
    return cleaned


def update_env(patch: dict[str, Any], path: Path | None = None) -> dict[str, str]:
    path = path or ENV_PATH
    cleaned = _validate_patch(patch)
    if not cleaned:
        return {}

    lines: list[str] = []
    seen: set[str] = set()
    if path.exists():
        lines = path.read_text(encoding="utf-8").splitlines()

    out: list[str] = []
    key_pattern = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*)=")
    for line in lines:
        m = key_pattern.match(line.strip())
        if m and m.group(1) in cleaned:
            key = m.group(1)
            out.append(f"{key}={cleaned[key]}")
            seen.add(key)
        else:
            out.append(line)

    for key, val in cleaned.items():
        if key not in seen:
            out.append(f"{key}={val}")

    _ensure_parent(path)
    path.write_text("\n".join(out) + ("\n" if out else ""), encoding="utf-8")
    return cleaned


def append_audit(event: dict[str, Any]) -> None:
    _ensure_parent(AUDIT_PATH)
    payload = {"ts": int(time.time() * 1000), **event}
    with AUDIT_PATH.open("a", encoding="utf-8") as f:
        f.write(json.dumps(payload, ensure_ascii=False) + "\n")


def read_audit(limit: int = 200) -> list[dict[str, Any]]:
    if not AUDIT_PATH.exists():
        return []
    lines = AUDIT_PATH.read_text(encoding="utf-8").splitlines()
    items: list[dict[str, Any]] = []
    for line in lines[-limit:]:
        line = line.strip()
        if not line:
            continue
        try:
            items.append(json.loads(line))
        except json.JSONDecodeError:
            continue
    items.reverse()
    return items


def write_runtime_config(payload: dict[str, Any]) -> None:
    _ensure_parent(RUNTIME_CONFIG_PATH)
    out = dict(payload)
    if "patch" in out and isinstance(out["patch"], dict):
        out["patch"] = {k: str(v) for k, v in out["patch"].items()}
    RUNTIME_CONFIG_PATH.write_text(json.dumps(out, ensure_ascii=False), encoding="utf-8")


def clear_live_trades_history(path: Path | None = None) -> dict[str, str]:
    """Reset live trade history baseline to now and archive persisted live LIH state."""
    path = path or ENV_PATH
    ts = str(int(time.time()))
    lines: list[str] = []
    seen = False
    if path.exists():
        for line in path.read_text(encoding="utf-8").splitlines():
            if line.strip().startswith("LIVE_TRADES_BASELINE_TS="):
                lines.append(f"LIVE_TRADES_BASELINE_TS={ts}")
                seen = True
            else:
                lines.append(line)
    if not seen:
        lines.append(f"LIVE_TRADES_BASELINE_TS={ts}")
    _ensure_parent(path)
    path.write_text("\n".join(lines) + ("\n" if lines else ""), encoding="utf-8")

    live_state = Path(os.getenv("LIVE_STATE_PATH", "logs/live_state.json"))
    if live_state.is_file():
        backup = live_state.with_name(f"live_state.json.bak.{ts}")
        live_state.rename(backup)
    return {"LIVE_TRADES_BASELINE_TS": ts}
