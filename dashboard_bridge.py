import asyncio
import json
import subprocess
import websockets
import threading
import sys
import os
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlparse

from bot_config import (
    public_config,
    update_env,
    append_audit,
    read_audit,
    write_runtime_config,
    _sanitize_audit_label,
    _sanitize_audit_reason,
)

try:
    from clob_live import post_fak_order, resolve_order_fill
except ImportError:
    post_fak_order = None
    resolve_order_fill = None

try:
    from clob_trades import fetch_user_trades
except ImportError:
    fetch_user_trades = None

# Configuration
WS_HOST = os.getenv("WS_HOST", "0.0.0.0")
WS_PORT = int(os.getenv("WS_PORT", "8080"))
HTTP_BIND = os.getenv("HTTP_BIND", "127.0.0.1")
HTTP_PORT = int(os.getenv("HTTP_PORT", "8081"))
BOT_API_TOKEN = os.getenv("BOT_API_TOKEN", "").strip()
PREFLIGHT_PATH = Path(os.getenv("PREFLIGHT_PATH", "logs/preflight.json"))
CORE_CMD = ["./build/trading-core.exe"] if os.name == "nt" else ["./build/trading-core"]

clients = set()
latest_data = "{}"
_core_ready_printed = False
_WALLET_PERSIST_KEYS = ("realWalletBalance", "cashBalance", "positionsValue", "walletSource")
_SHADOW_LOG_PATH = Path(os.getenv("BRIDGE_LOG_PATH", "logs/bridge.log"))
_telemetry_log_flush_idx = 0


def _flush_new_shadow_lines(obj: dict) -> None:
    """Append new [LIVE LIH SHADOW] telemetry lines to bridge.log for prelive scans."""
    global _telemetry_log_flush_idx
    tlog = obj.get("telemetryLog")
    if not isinstance(tlog, list):
        return
    start = min(_telemetry_log_flush_idx, len(tlog))
    try:
        with _SHADOW_LOG_PATH.open("a", encoding="utf-8") as f:
            for entry in tlog[start:]:
                text = str(entry)
                if "LIVE LIH SHADOW" in text:
                    f.write(text + "\n")
    except OSError:
        pass
    _telemetry_log_flush_idx = len(tlog)


def _persist_wallet_fields(incoming: dict, previous: dict) -> None:
    for key in _WALLET_PERSIST_KEYS:
        if key in previous:
            incoming[key] = previous[key]


def _merge_core_telemetry(line: str) -> str:
    try:
        incoming = json.loads(line)
        previous = json.loads(latest_data or "{}")
        if isinstance(incoming, dict) and isinstance(previous, dict):
            _persist_wallet_fields(incoming, previous)
            return json.dumps(incoming, ensure_ascii=False)
    except json.JSONDecodeError:
        pass
    return line


def _apply_wallet_snapshot(detail: dict) -> bool:
    global latest_data
    if not isinstance(detail, dict) or not detail.get("funder"):
        return False
    if str(detail.get("source") or "") not in ("live", "on_chain", "clob"):
        return False
    total = float(detail.get("total") or 0)
    if total <= 0:
        return False
    cash = float(detail.get("cash") or 0)
    pos = float(detail.get("positions") or 0)
    obj = json.loads(latest_data or "{}")
    if not isinstance(obj, dict):
        obj = {}
    obj["realWalletBalance"] = total
    obj["cashBalance"] = cash
    obj["positionsValue"] = pos
    obj["walletSource"] = str(detail.get("source") or "live")
    obj["balance"] = total
    latest_data = json.dumps(obj, ensure_ascii=False)
    if clients and loop:
        asyncio.run_coroutine_threadsafe(broadcast(latest_data), loop)
    return True


def _env_int(key: str, default: int, *, min_v: int = 1, max_v: int = 300) -> int:
    raw = (os.getenv(key) or "").strip()
    if not raw:
        return default
    try:
        v = int(raw)
        return max(min_v, min(max_v, v))
    except ValueError:
        return default


def _wallet_sync_once() -> None:
    funder = (os.getenv("POLYMARKET_FUNDER") or "").strip()
    if not funder or funder.startswith("#"):
        return
    try:
        proc = subprocess.run(
            [sys.executable, "fetch_balance.py", "--json"],
            cwd=os.getcwd(),
            capture_output=True,
            text=True,
            timeout=45,
            check=False,
        )
        detail = json.loads((proc.stdout or "").strip() or "{}")
        if _apply_wallet_snapshot(detail):
            print(
                f"[WALLET] synced total=${float(detail.get('total') or 0):.2f} "
                f"cash=${float(detail.get('cash') or 0):.2f}",
                file=sys.stderr,
            )
    except Exception as exc:
        print(f"[WALLET] sync failed: {exc}", file=sys.stderr)


def _print_startup_banner() -> None:
    cfg = public_config()
    pre = _read_preflight()
    mode = (pre.get("mode") or "live").upper()
    ok = pre.get("ok", True)
    mark = "✅" if ok else "⚠️"
    print(f"\n{mark} Bridge 就绪 | 模式 {mode} | WS :{WS_PORT} | API :{HTTP_PORT}", file=sys.stderr)
    if pre.get("wallet", {}).get("funder"):
        w = pre["wallet"]
        print(
            f"   钱包 funder={w.get('funder')} signer={w.get('signer')} sigType={w.get('signature_type')}",
            file=sys.stderr,
        )
    if cfg:
        print(
            f"   LIH mode={cfg.get('LIH_LEG1_MODE', 'trigger')}  "
            f"target={cfg.get('LIH_TARGET_COMBINED', '?')}  "
            f"5m={cfg.get('DH_ENABLE_5M', '?')}  15m={cfg.get('DH_ENABLE_15M', '?')}",
            file=sys.stderr,
        )
    print("", file=sys.stderr)


def _maybe_print_core_ready(line: str) -> None:
    global _core_ready_printed
    if _core_ready_printed:
        return
    try:
        d = json.loads(line)
    except json.JSONDecodeError:
        return
    if "strategy" not in d or "balance" not in d:
        return
    _core_ready_printed = True
    dry = d.get("liveLihDryRun", False)
    mode_label = "shadow" if dry else "实盘"
    bal = float(d.get("balance") or 0)
    open_n = d.get("openCount", 0)
    status = d.get("statusReason") or d.get("status", 0)
    assets_5m = [
        a for a, k in (("BTC", "dhEnable5mBtc"), ("ETH", "dhEnable5mEth"), ("SOL", "dhEnable5mSol"))
        if d.get(k, True) and d.get("dhEnable5m", True)
    ]
    assets_15m = [
        a for a, k in (("BTC", "dhEnable15mBtc"), ("ETH", "dhEnable15mEth"))
        if d.get(k, True) and d.get("dhEnable15m", True)
    ]
    fee = "动态" if d.get("useDynamicFees") else "扁平"
    strat = "LIH" if d.get("lihEnabled") else "DH"
    lih_trades = d.get("totalLihTrades", 0)
    dh_trades = d.get("totalDhTrades", 0)
    print(
        f"[CORE 就绪] {mode_label} | {strat} | 余额 ${bal:.2f} | 持仓 {open_n} | 状态 {status}",
        file=sys.stderr,
    )
    print(
        f"            5m[{' '.join(assets_5m) or '关'}]  "
        f"15m[{' '.join(assets_15m) or '关'}]  "
        f"费率={fee}  市场={d.get('marketsScanned', 0)}  "
        f"成交 LIH={lih_trades} DH={dh_trades}",
        file=sys.stderr,
    )


def _read_preflight() -> dict:
    if not PREFLIGHT_PATH.is_file():
        return {}
    try:
        return json.loads(PREFLIGHT_PATH.read_text(encoding="utf-8"))
    except Exception:
        return {}


def _run_preflight() -> None:
    if os.getenv("PREFLIGHT_SKIP", "").strip().lower() in ("1", "true", "yes"):
        return
    script = Path(__file__).resolve().parent / "live_preflight.py"
    if not script.is_file():
        return
    try:
        subprocess.run([sys.executable, str(script)], check=False)
    except Exception as exc:
        print(f"[preflight] skipped: {exc}", file=sys.stderr)


def _json_response(handler: BaseHTTPRequestHandler, status: int, payload: dict) -> None:
    body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
    if status >= 400:
        err = payload.get("error", payload)
        print(f"[HTTP] ERROR {status} {err}", file=sys.stderr)
    handler.send_response(status)
    handler.send_header("Content-Type", "application/json; charset=utf-8")
    handler.send_header("Content-Length", str(len(body)))
    handler.end_headers()
    handler.wfile.write(body)


def _check_api_auth(handler: BaseHTTPRequestHandler) -> bool:
    if not BOT_API_TOKEN:
        return True
    auth = handler.headers.get("Authorization", "")
    token = handler.headers.get("X-Bot-Api-Token", "")
    if auth.startswith("Bearer "):
        token = auth[7:].strip()
    return token == BOT_API_TOKEN


def _read_body(handler: BaseHTTPRequestHandler) -> dict:
    length = int(handler.headers.get("Content-Length", "0") or "0")
    if length <= 0:
        return {}
    raw = handler.rfile.read(length)
    try:
        return json.loads(raw.decode("utf-8"))
    except json.JSONDecodeError as exc:
        raise ValueError(f"Invalid JSON: {exc}") from exc


def _is_localhost(handler: BaseHTTPRequestHandler) -> bool:
    host = handler.headers.get("Host", "")
    if host.startswith("127.0.0.1") or host.startswith("localhost"):
        return True
    peer = handler.client_address[0] if handler.client_address else ""
    return peer in ("127.0.0.1", "::1", "localhost")


class ConfigHTTPHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        print(f"[HTTP] {self.address_string()} {fmt % args}", file=sys.stderr)

    def do_GET(self):
        path = urlparse(self.path).path
        try:
            if path == "/health":
                _json_response(self, 200, {"ok": True})
            elif path == "/api/config":
                _json_response(self, 200, {"config": public_config(), "live": json.loads(latest_data or "{}")})
            elif path == "/api/audit":
                _json_response(self, 200, {"events": read_audit()})
            elif path == "/api/preflight":
                report = _read_preflight()
                if not report:
                    _json_response(self, 404, {"error": "preflight not run yet"})
                else:
                    _json_response(self, 200, {"preflight": report})
            elif path == "/api/clob/trades":
                if fetch_user_trades is None:
                    _json_response(self, 503, {"error": "clob_trades unavailable"})
                    return
                query = urlparse(self.path).query
                limit = 200
                if query:
                    for part in query.split("&"):
                        if part.startswith("limit="):
                            try:
                                limit = int(part.split("=", 1)[1])
                            except ValueError:
                                pass
                trades = fetch_user_trades(limit=limit)
                if trades and isinstance(trades[0], dict) and trades[0].get("error"):
                    _json_response(self, 502, {"error": trades[0]["error"], "trades": []})
                else:
                    _json_response(self, 200, {"trades": trades, "count": len(trades)})
            else:
                _json_response(self, 404, {"error": "not found"})
        except Exception as exc:
            _json_response(self, 500, {"error": str(exc)})

    def do_POST(self):
        path = urlparse(self.path).path
        try:
            if path == "/internal/clob/order":
                if not _is_localhost(self):
                    _json_response(self, 403, {"error": "localhost only"})
                    return
                if post_fak_order is None:
                    _json_response(self, 503, {"error": "clob_live unavailable"})
                    return
                body = _read_body(self)
                token_id = str(body.get("token_id") or "").strip()
                side = str(body.get("side") or "BUY").strip()
                try:
                    price = float(body.get("price"))
                    size_shares = float(body.get("size_shares"))
                except (TypeError, ValueError):
                    _json_response(self, 400, {"error": "price and size_shares required"})
                    return
                if not token_id or price <= 0 or size_shares <= 0:
                    _json_response(self, 400, {"error": "invalid token_id/price/size_shares"})
                    return
                neg_risk = bool(body.get("neg_risk", False))
                result = post_fak_order(token_id, price, size_shares, side, neg_risk=neg_risk)
                # Always 200 so C++ bridge parses order_id even when fill is still 0.
                _json_response(self, 200, result)
                return

            if path == "/internal/clob/resolve":
                if not _is_localhost(self):
                    _json_response(self, 403, {"error": "localhost only"})
                    return
                if resolve_order_fill is None:
                    _json_response(self, 503, {"error": "clob_live unavailable"})
                    return
                body = _read_body(self)
                token_id = str(body.get("token_id") or "").strip()
                side = str(body.get("side") or "BUY").strip()
                order_id = str(body.get("order_id") or "").strip()
                try:
                    price = float(body.get("price"))
                except (TypeError, ValueError):
                    price = 0.0
                submit_ts = body.get("submit_ts")
                try:
                    submit_ts_f = float(submit_ts) if submit_ts is not None else None
                except (TypeError, ValueError):
                    submit_ts_f = None
                if not token_id:
                    _json_response(self, 400, {"error": "token_id required"})
                    return
                result = resolve_order_fill(
                    token_id, side, price, order_id, submit_ts=submit_ts_f
                )
                _json_response(self, 200, result)
                return

            if not _check_api_auth(self):
                _json_response(self, 401, {"error": "unauthorized"})
                return
            body = _read_body(self)
            user = _sanitize_audit_label(str(body.get("user") or "web"))

            if path == "/api/config":
                patch = body.get("patch") or {}
                action = str(body.get("action") or "").lower().strip()
                reason = _sanitize_audit_reason(str(body.get("reason") or ""))
                if not isinstance(patch, dict):
                    _json_response(self, 400, {"error": "patch must be an object"})
                    return
                if not patch and action not in ("pause", "resume", "reset_kill"):
                    _json_response(self, 400, {"error": "patch or action required"})
                    return
                payload: dict = {"user": user}
                applied: dict = {}
                if patch:
                    applied = update_env(patch)
                    payload["patch"] = applied
                    append_audit({"type": "config", "user": user, "patch": applied})
                if action in ("pause", "resume", "reset_kill"):
                    payload["control"] = action
                    if reason:
                        payload["reason"] = reason
                    append_audit({"type": "control", "user": user, "action": action, "reason": reason})
                write_runtime_config(payload)
                _json_response(self, 200, {"ok": True, "applied": applied, "action": action or None})

            elif path == "/api/control":
                action = str(body.get("action") or "").lower()
                if action not in ("pause", "resume", "reset_kill"):
                    _json_response(self, 400, {"error": "action must be pause|resume|reset_kill"})
                    return
                reason = _sanitize_audit_reason(str(body.get("reason") or f"Manual {action} via web")) or f"Manual {action} via web"
                append_audit({"type": "control", "user": user, "action": action, "reason": reason})
                write_runtime_config({"control": action, "reason": reason, "user": user})
                _json_response(self, 200, {"ok": True, "action": action})

            elif path == "/api/mirror":
                mirror_path = Path(os.getenv("LIVE_MIRROR_PATH", "logs/live_mirror.json"))
                mirror_path.parent.mkdir(parents=True, exist_ok=True)
                payload = {
                    "updated_at": body.get("updated_at") or time.time(),
                    "source": body.get("source") or "external",
                    "assets": body.get("assets") or {},
                }
                if not isinstance(payload["assets"], dict):
                    _json_response(self, 400, {"error": "assets must be object"})
                    return
                mirror_path.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")
                append_audit({"type": "mirror", "user": user, "assets": list(payload["assets"].keys())})
                _json_response(self, 200, {"ok": True, "path": str(mirror_path), "assets": len(payload["assets"])})

            else:
                _json_response(self, 404, {"error": "not found"})
        except ValueError as exc:
            _json_response(self, 400, {"error": str(exc)})
        except Exception as exc:
            _json_response(self, 500, {"error": str(exc)})


def run_http_server():
    server = ThreadingHTTPServer((HTTP_BIND, HTTP_PORT), ConfigHTTPHandler)
    print(f"Config API on http://{HTTP_BIND}:{HTTP_PORT}", file=sys.stderr)
    server.serve_forever()


async def broadcast(message):
    if clients:
        await asyncio.gather(*[client.send(message) for client in clients])


async def handler(websocket):
    global latest_data
    clients.add(websocket)
    try:
        await websocket.send(latest_data)
        async for _message in websocket:
            pass
    finally:
        clients.remove(websocket)


def _wallet_sync_loop() -> None:
    """Refresh on-chain wallet total (cash + positions) for dashboard — read-only."""
    interval = _env_int("WALLET_SYNC_INTERVAL_SEC", 2, min_v=1, max_v=120)
    while True:
        _wallet_sync_once()
        time.sleep(interval)


def _live_maintenance_loop() -> None:
    """Prune expired LIH rows from disk; chain reconcile only in real live (not shadow)."""
    while True:
        time.sleep(60)
        try:
            subprocess.run(
                [sys.executable, "scripts/prune_live_lih.py"],
                cwd=os.getcwd(),
                timeout=30,
                check=False,
            )
            live_dry = os.getenv("LIVE_LIH_DRY_RUN", "true").lower() not in (
                "false",
                "0",
                "no",
                "off",
            )
            recon_enabled = os.getenv("LIVE_LIH_RECONCILE_ENABLED", "true").lower() not in (
                "false",
                "0",
                "no",
                "off",
            )
            if live_dry or not recon_enabled:
                continue
            subprocess.run(
                [sys.executable, "scripts/live_lih_reconcile.py", "--merge"],
                cwd=os.getcwd(),
                timeout=45,
                check=False,
            )
            write_runtime_config({"control": "reload_lih_state", "user": "maintenance"})
        except Exception as exc:
            print(f"[MAINT] prune failed: {exc}", file=sys.stderr)


def _mark_core_offline(reason: str = "trading-core stopped") -> None:
    global latest_data
    try:
        obj = json.loads(latest_data or "{}")
        if not isinstance(obj, dict):
            obj = {}
    except json.JSONDecodeError:
        obj = {}
    obj["status"] = 3
    obj["statusReason"] = reason
    latest_data = json.dumps(obj, ensure_ascii=False)
    if clients and loop:
        asyncio.run_coroutine_threadsafe(broadcast(latest_data), loop)


def run_core():
    global latest_data
    env = os.environ.copy()
    venv_bin = os.path.join(os.getcwd(), ".venv", "bin")
    if os.path.isdir(venv_bin):
        env["PATH"] = venv_bin + os.pathsep + env.get("PATH", "")
    process = subprocess.Popen(
        CORE_CMD,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        bufsize=1,
        cwd=os.getcwd(),
        env=env,
    )

    def log_stderr():
        for line in process.stderr:
            print(f"[CORE LOG] {line.strip()}", file=sys.stderr)

    threading.Thread(target=log_stderr, daemon=True).start()

    for line in process.stdout:
        line = line.strip()
        if line.startswith("{") and line.endswith("}"):
            try:
                incoming = json.loads(line)
                if isinstance(incoming, dict):
                    _flush_new_shadow_lines(incoming)
            except json.JSONDecodeError:
                pass
            line = _merge_core_telemetry(line)
            latest_data = line
            _maybe_print_core_ready(line)
            asyncio.run_coroutine_threadsafe(broadcast(line), loop)
        else:
            print(f"[CORE INFO] {line}", file=sys.stderr)

    code = process.wait()
    _mark_core_offline(f"trading-core exited ({code})")


async def main():
    global loop
    loop = asyncio.get_running_loop()

    _run_preflight()
    _print_startup_banner()
    threading.Thread(target=run_http_server, daemon=True).start()
    threading.Thread(target=_wallet_sync_loop, daemon=True).start()
    threading.Thread(target=_live_maintenance_loop, daemon=True).start()

    async with websockets.serve(handler, WS_HOST, WS_PORT):
        print(f"Bridge started on ws://{WS_HOST}:{WS_PORT}", file=sys.stderr)
        threading.Thread(target=run_core, daemon=True).start()
        await asyncio.Future()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("Shutting down bridge...")
