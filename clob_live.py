"""Live CLOB order execution via official py-clob-client-v2 (V2 EIP-712)."""

from __future__ import annotations

import json
import math
import os
import re
import time
from functools import lru_cache
from typing import Any

from dotenv import load_dotenv
from py_clob_client_v2.client import ClobClient
from py_clob_client_v2.clob_types import (
    ApiCreds,
    MarketOrderArgsV2,
    OrderArgsV2,
    OrderType,
    PartialCreateOrderOptions,
)
from py_clob_client_v2.constants import POLYGON

load_dotenv()

_SCALE = 1_000_000.0
_MIN_BUY_USDC = 1.0
_MIN_ORDER_SHARES = 5.0
_ORDER_ID_RE = re.compile(r"0x[a-fA-F0-9]{64}")
_TICK = 0.01


def classify_exec(
    *,
    side: str,
    price: float,
    side_ask: float | None,
    side_bid: float | None = None,
    secs_left: float | None = None,
) -> str:
    """Infer exec_class from fill price vs book (mm2-tracker-v2 rules)."""
    px = float(price)
    if side_ask is None:
        if secs_left is not None and secs_left <= 20:
            return "endgame_no_book"
        if px >= 0.92 or px <= 0.08:
            return "endgame_no_book"
        return "no_book"
    ask = float(side_ask)
    if side.upper() == "BUY":
        if px < ask - _TICK:
            return "limit_below_ask"
        if side_bid is not None and px <= float(side_bid) + _TICK:
            return "limit_at_bid"
        if px <= ask + _TICK:
            return "taker_at_ask"
        return "taker_above_ask"
    return "unknown"


def infer_trader_side(post_order_type: str, exec_class: str) -> str:
    if post_order_type.upper() == "FAK":
        return "TAKER"
    if exec_class in ("limit_below_ask", "limit_at_bid"):
        return "MAKER"
    if exec_class in ("taker_at_ask", "taker_above_ask"):
        return "TAKER"
    return ""


def _attach_exec_meta(
    result: dict[str, Any],
    *,
    side: str,
    fill_price: float,
    post_order_type: str,
    side_ask: float | None = None,
    side_bid: float | None = None,
    secs_left: float | None = None,
    intent: str = "",
) -> dict[str, Any]:
    result["post_order_type"] = post_order_type
    if intent:
        result["intent"] = intent
    if side_ask is not None:
        result["side_ask_at_fill"] = side_ask
        result["price_vs_ask_cents"] = round((float(fill_price) - float(side_ask)) * 100.0, 4)
    if side_bid is not None:
        result["side_bid_at_fill"] = side_bid
    ec = classify_exec(
        side=side,
        price=fill_price,
        side_ask=side_ask,
        side_bid=side_bid,
        secs_left=secs_left,
    )
    result["exec_class"] = ec
    ts = infer_trader_side(post_order_type, ec)
    if ts:
        result["trader_side"] = ts
    return result


def _resolve_signature_type(funder: str, signer: str) -> int:
    raw = os.getenv("POLYMARKET_SIGNATURE_TYPE", "").strip()
    if raw:
        return int(raw)
    if funder and signer and funder.lower() != signer.lower():
        return 3
    return 0


@lru_cache(maxsize=1)
def _client() -> ClobClient:
    pk = os.getenv("POLYMARKET_PRIVATE_KEY", "").strip()
    if not pk.startswith("0x"):
        pk = "0x" + pk
    signer = os.getenv("POLYMARKET_SIGNER", "").strip()
    funder = os.getenv("POLYMARKET_FUNDER", "").strip()
    creds = ApiCreds(
        api_key=os.getenv("POLY_API_KEY", "").strip(),
        api_secret=os.getenv("POLY_API_SECRET", "").strip(),
        api_passphrase=os.getenv("POLY_PASSPHRASE", "").strip(),
    )
    sig_type = _resolve_signature_type(funder, signer)
    return ClobClient(
        "https://clob.polymarket.com",
        key=pk,
        chain_id=POLYGON,
        signature_type=sig_type,
        funder=funder,
        creds=creds,
    )


def round_clob_buy(price: float, size_shares: float) -> tuple[float, float]:
    """Polymarket BUY: 2dp USDC (maker) + implied shares (taker max 4dp via market path)."""
    price, usdc, size = buy_usdc_for_shares(price, size_shares)
    return price, size


def buy_usdc_for_shares(price: float, size_shares: float) -> tuple[float, float, float]:
    """Return (price, usdc_2dp, implied_shares) for market-style FAK buys."""
    price = round(float(price), 2)
    if price <= 0 or size_shares <= 0:
        return price, 0.0, 0.0
    size_2dp = math.floor(float(size_shares) * 100 + 1e-9) / 100
    if size_2dp + 1e-9 < _MIN_ORDER_SHARES:
        return price, 0.0, 0.0
    usdc = math.floor(price * size_2dp * 100 + 1e-9) / 100
    if usdc < _MIN_BUY_USDC:
        size_2dp = math.ceil((_MIN_BUY_USDC / price) * 100) / 100
        usdc = math.floor(price * size_2dp * 100 + 1e-9) / 100
    if usdc < _MIN_BUY_USDC:
        return price, 0.0, 0.0
    implied = math.floor((usdc / price) * 10000 + 1e-9) / 10000
    return price, usdc, implied


def round_clob_sell(price: float, size_shares: float) -> tuple[float, float]:
    price = round(float(price), 4)
    size = math.floor(float(size_shares) * 10000 + 1e-9) / 10000
    return price, size if size > 0 else 0.0


def _float_field(obj: dict[str, Any], *keys: str) -> float:
    for key in keys:
        if key not in obj or obj[key] is None:
            continue
        val = obj[key]
        try:
            return float(val)
        except (TypeError, ValueError):
            continue
    return 0.0


def _parse_fill(side: str, resp: dict[str, Any]) -> tuple[float, float]:
    """Return (price, size_shares) from CLOB post_order / get_order response."""
    making = _float_field(resp, "makingAmount", "making_amount")
    taking = _float_field(resp, "takingAmount", "taking_amount")
    if making > 0 and taking > 0:
        if side.upper() == "BUY":
            shares = taking / _SCALE
            price = (making / _SCALE) / shares if shares > 0 else 0.0
        else:
            shares = making / _SCALE
            price = (taking / _SCALE) / shares if shares > 0 else 0.0
        return price, shares

    matched = _float_field(resp, "size_matched", "sizeMatched", "matched_amount", "matchedAmount")
    price = _float_field(resp, "price", "avg_price", "average_price")
    if matched > 0:
        if matched > 1000:
            matched = matched / _SCALE
        return (price if price > 0 else 0.0), matched

    raw_size = _float_field(resp, "size", "original_size", "filled_size", "filledSize")
    if raw_size > 0:
        if raw_size > 1000:
            raw_size = raw_size / _SCALE
        return (price if price > 0 else 0.0), raw_size

    # Do NOT return price with 0 shares — forces poll / activity fallback.
    return 0.0, 0.0


def _extract_order_id(payload: Any) -> str:
    if isinstance(payload, dict):
        for key in ("orderID", "orderId", "order_id", "id"):
            val = payload.get(key)
            if val:
                return str(val)
        err = payload.get("error")
        if isinstance(err, str):
            m = _ORDER_ID_RE.search(err)
            if m:
                return m.group(0)
    text = str(payload)
    m = _ORDER_ID_RE.search(text)
    return m.group(0) if m else ""


def _activity_fill_for_token(
    token_id: str,
    side: str,
    fallback_price: float,
    *,
    since_ts: float | None = None,
    order_id: str = "",
) -> tuple[float, float]:
    """Last-resort: match recent wallet TRADE rows when CLOB poll returns 0 shares."""
    try:
        from clob_trades import fetch_user_trades
    except ImportError:
        return fallback_price, 0.0
    side_u = side.upper()
    oid = (order_id or "").strip().lower()
    cutoff = (since_ts or (time.time() - 120.0)) - 60.0
    tok = str(token_id).strip()
    total_shares = 0.0
    weighted_cost = 0.0
    for row in fetch_user_trades(limit=60):
        if not isinstance(row, dict) or row.get("error"):
            continue
        row_tok = str(row.get("tokenID") or row.get("asset") or "")
        if tok and row_tok and row_tok != tok:
            continue
        if str(row.get("side") or "").upper() != side_u:
            continue
        ts = float(row.get("timestamp") or 0)
        if ts > 0 and ts < cutoff:
            continue
        if oid:
            row_oid = str(row.get("orderID") or row.get("id") or "").lower()
            if row_oid and oid not in row_oid and row_oid not in oid:
                continue
        size = float(row.get("size") or 0)
        price = float(row.get("price") or 0)
        if size <= 0:
            continue
        total_shares += size
        weighted_cost += size * (price if price > 0 else fallback_price)
    if total_shares > 0:
        avg = weighted_cost / total_shares
        return (avg if avg > 0 else fallback_price), total_shares
    return fallback_price, 0.0


def _poll_order_fill(
    client: ClobClient,
    order_id: str,
    side: str,
    fallback_price: float,
    *,
    token_id: str = "",
    submit_ts: float | None = None,
) -> tuple[float, float, str]:
    status = ""
    for attempt in range(40):
        if attempt > 0:
            time.sleep(0.35)
        try:
            raw = client.get_order(order_id)
            resp = raw if isinstance(raw, dict) else {}
            status = str(resp.get("status") or "")
            price, shares = _parse_fill(side, resp)
            if shares > 0:
                return (price if price > 0 else fallback_price), shares, status
            if status in ("unmatched", "cancelled", "expired", "failed"):
                break
        except Exception:
            continue
    if token_id:
        price, shares = _activity_fill_for_token(
            token_id,
            side,
            fallback_price,
            since_ts=submit_ts,
            order_id=order_id,
        )
        if shares > 0:
            return price, shares, "activity"
    return fallback_price, 0.0, status


def _normalize_result(
    *,
    side: str,
    fallback_price: float,
    resp: dict[str, Any] | None,
    order_id: str,
    client: ClobClient,
    token_id: str = "",
    submit_ts: float | None = None,
    post_order_type: str = "FAK",
    side_ask: float | None = None,
    side_bid: float | None = None,
    intent: str = "",
) -> dict[str, Any]:
    if not order_id and resp:
        order_id = _extract_order_id(resp)

    fill_price, fill_shares = (0.0, 0.0)
    status = ""
    if resp:
        fill_price, fill_shares = _parse_fill(side, resp)
        status = str(resp.get("status") or "")

    if fill_shares <= 0 and order_id:
        polled_price, polled_shares, polled_status = _poll_order_fill(
            client,
            order_id,
            side,
            fallback_price,
            token_id=token_id,
            submit_ts=submit_ts,
        )
        if polled_shares > 0:
            fill_price, fill_shares = polled_price, polled_shares
            status = polled_status or status

    if fill_shares <= 0 and token_id:
        act_price, act_shares = _activity_fill_for_token(
            token_id,
            side,
            fallback_price,
            since_ts=(submit_ts or time.time()) - 600.0,
            order_id=order_id,
        )
        if act_shares > 0:
            fill_price, fill_shares = act_price, act_shares
            status = status or "activity"

    if fill_shares <= 0:
        return {
            "success": False,
            "error": status or "0 fill after poll",
            "price": fallback_price,
            "size_shares": 0.0,
            "order_id": order_id,
            "status": status,
            "post_order_type": post_order_type,
        }

    out_price = fill_price if fill_price > 0 else fallback_price
    result = {
        "success": True,
        "price": out_price,
        "size_shares": fill_shares,
        "order_id": order_id,
        "status": status or "matched",
        "error": "",
    }
    return _attach_exec_meta(
        result,
        side=side,
        fill_price=out_price,
        post_order_type=post_order_type,
        side_ask=side_ask,
        side_bid=side_bid,
        intent=intent,
    )


def resolve_order_fill(
    token_id: str,
    side: str,
    fallback_price: float,
    order_id: str = "",
    *,
    submit_ts: float | None = None,
) -> dict[str, Any]:
    """Re-query CLOB + activity after C++ got 0 fill (Data API may lag)."""
    submit_ts = submit_ts or time.time()
    client = _client()
    if order_id:
        price, shares, status = _poll_order_fill(
            client,
            order_id,
            side,
            fallback_price,
            token_id=token_id,
            submit_ts=submit_ts,
        )
        if shares > 0:
            return {
                "success": True,
                "price": price,
                "size_shares": shares,
                "order_id": order_id,
                "status": status or "resolved",
                "error": "",
            }
    price, shares = _activity_fill_for_token(
        token_id,
        side,
        fallback_price,
        since_ts=submit_ts,
        order_id=order_id,
    )
    if shares > 0:
        return {
            "success": True,
            "price": price,
            "size_shares": shares,
            "order_id": order_id,
            "status": "activity",
            "error": "",
        }
    return {
        "success": False,
        "price": fallback_price,
        "size_shares": 0.0,
        "order_id": order_id,
        "status": "unresolved",
        "error": "0 fill after resolve",
    }


def post_fak_order(
    token_id: str,
    price: float,
    size_shares: float,
    side: str,
    *,
    neg_risk: bool = False,
    side_ask: float | None = None,
    side_bid: float | None = None,
    intent: str = "",
) -> dict[str, Any]:
    """Submit a FAK limit order; returns normalized result dict for C++ bridge."""
    side_u = side.upper()
    if side_u not in ("BUY", "SELL"):
        return {"success": False, "error": f"invalid side: {side}"}

    if side_u == "BUY":
        price, usdc, size_shares = buy_usdc_for_shares(price, size_shares)
    else:
        price, size_shares = round_clob_sell(price, size_shares)
        usdc = 0.0
    if side_u == "BUY" and usdc <= 0:
        return {"success": False, "error": "size below exchange minimum after rounding"}
    if side_u == "SELL" and size_shares <= 0:
        return {"success": False, "error": "size below exchange minimum after rounding"}

    submit_ts = time.time()
    try:
        from py_clob_client_v2.clob_types import BalanceAllowanceParams, AssetType

        client = _client()
        funder = os.getenv("POLYMARKET_FUNDER", "").strip()
        signer = os.getenv("POLYMARKET_SIGNER", "").strip()
        sig_type = _resolve_signature_type(funder, signer)
        try:
            client.update_balance_allowance(
                params=BalanceAllowanceParams(asset_type=AssetType.COLLATERAL, signature_type=sig_type)
            )
        except Exception:
            pass
        opts = PartialCreateOrderOptions(neg_risk=True) if neg_risk else None
        if side_u == "BUY":
            # FAK at ask is treated as market buy — limit path yields 4dp USDC and CLOB rejects.
            margs = MarketOrderArgsV2(
                token_id=str(token_id),
                amount=float(usdc),
                side=side_u,
                price=float(price),
                order_type=OrderType.FAK,
            )
            resp = client.create_and_post_market_order(margs, options=opts, order_type=OrderType.FAK)
        else:
            args = OrderArgsV2(
                token_id=str(token_id),
                price=float(price),
                size=float(size_shares),
                side=side_u,
            )
            resp = client.create_and_post_order(args, options=opts, order_type=OrderType.FAK)
    except Exception as exc:
        order_id = _extract_order_id(getattr(exc, "error_message", None) or str(exc))
        if order_id:
            try:
                client = _client()
                return _normalize_result(
                    side=side_u,
                    fallback_price=price,
                    resp=None,
                    order_id=order_id,
                    client=client,
                    token_id=str(token_id),
                    submit_ts=submit_ts,
                    post_order_type="FAK",
                    side_ask=side_ask,
                    side_bid=side_bid,
                    intent=intent,
                )
            except Exception:
                pass
        return {
            "success": False,
            "error": str(exc),
            "order_id": order_id,
            "post_order_type": "FAK",
        }

    if not isinstance(resp, dict):
        return {"success": False, "error": f"unexpected response type: {type(resp).__name__}"}

    order_id = _extract_order_id(resp)
    error_msg = str(resp.get("errorMsg") or resp.get("error") or "")
    status = str(resp.get("status") or "")
    success = bool(resp.get("success", True))

    if not success or error_msg:
        if order_id:
            return _normalize_result(
                side=side_u,
                fallback_price=price,
                resp=resp,
                order_id=order_id,
                client=_client(),
                token_id=str(token_id),
                submit_ts=submit_ts,
                post_order_type="FAK",
                side_ask=side_ask,
                side_bid=side_bid,
                intent=intent,
            )
        return {
            "success": False,
            "error": error_msg or "order rejected",
            "status": status,
            "order_id": order_id,
            "post_order_type": "FAK",
        }

    if status == "unmatched" and order_id:
        return _normalize_result(
            side=side_u,
            fallback_price=price,
            resp=resp,
            order_id=order_id,
            client=_client(),
            token_id=str(token_id),
            submit_ts=submit_ts,
            post_order_type="FAK",
            side_ask=side_ask,
            side_bid=side_bid,
            intent=intent,
        )
    if status == "unmatched":
        return {
            "success": False,
            "error": "FAK unmatched",
            "status": status,
            "order_id": order_id,
            "post_order_type": "FAK",
        }

    return _normalize_result(
        side=side_u,
        fallback_price=price,
        resp=resp,
        order_id=order_id,
        client=_client(),
        token_id=str(token_id),
        submit_ts=submit_ts,
        post_order_type="FAK",
        side_ask=side_ask,
        side_bid=side_bid,
        intent=intent,
    )


def post_gtc_order(
    token_id: str,
    price: float,
    size_shares: float,
    side: str,
    *,
    neg_risk: bool = False,
    side_ask: float | None = None,
    side_bid: float | None = None,
    intent: str = "",
    wait_secs: float = 8.0,
) -> dict[str, Any]:
    """Post GTC limit; poll until partial/full fill, cancel, or timeout."""
    side_u = side.upper()
    if side_u not in ("BUY", "SELL"):
        return {"success": False, "error": f"invalid side: {side}", "post_order_type": "GTC"}
    if side_u == "BUY":
        price, usdc, size_shares = buy_usdc_for_shares(price, size_shares)
    else:
        price, size_shares = round_clob_sell(price, size_shares)
        usdc = 0.0
    if side_u == "BUY" and usdc <= 0:
        return {"success": False, "error": "size below exchange minimum after rounding", "post_order_type": "GTC"}
    if side_u == "SELL" and size_shares <= 0:
        return {"success": False, "error": "size below exchange minimum after rounding", "post_order_type": "GTC"}

    submit_ts = time.time()
    try:
        client = _client()
        opts = PartialCreateOrderOptions(neg_risk=True) if neg_risk else None
        args = OrderArgsV2(
            token_id=str(token_id),
            price=float(price),
            size=float(size_shares),
            side=side_u,
        )
        resp = client.create_and_post_order(args, options=opts, order_type=OrderType.GTC)
    except Exception as exc:
        order_id = _extract_order_id(getattr(exc, "error_message", None) or str(exc))
        if order_id:
            try:
                return _normalize_result(
                    side=side_u,
                    fallback_price=price,
                    resp=None,
                    order_id=order_id,
                    client=_client(),
                    token_id=str(token_id),
                    submit_ts=submit_ts,
                    post_order_type="GTC",
                    side_ask=side_ask,
                    side_bid=side_bid,
                    intent=intent,
                )
            except Exception:
                pass
        return {"success": False, "error": str(exc), "order_id": order_id, "post_order_type": "GTC"}

    if not isinstance(resp, dict):
        return {"success": False, "error": f"unexpected response type: {type(resp).__name__}", "post_order_type": "GTC"}

    order_id = _extract_order_id(resp)
    deadline = time.time() + max(0.5, float(wait_secs))
    while time.time() < deadline:
        time.sleep(0.35)
        try:
            od = client.get_order(order_id)
        except Exception:
            continue
        if not isinstance(od, dict):
            continue
        st = str(od.get("status", "")).lower()
        matched = float(od.get("size_matched", 0) or 0)
        if matched > 0 or st in ("matched", "filled"):
            return _normalize_result(
                side=side_u,
                fallback_price=price,
                resp=od,
                order_id=order_id,
                client=client,
                token_id=str(token_id),
                submit_ts=submit_ts,
                post_order_type="GTC",
                side_ask=side_ask,
                side_bid=side_bid,
                intent=intent,
            )
        if st in ("cancelled", "canceled", "expired", "failed"):
            break
    try:
        if order_id:
            client.cancel(order_id)
    except Exception:
        pass
    try:
        od = client.get_order(order_id) if order_id else None
    except Exception:
        od = None
    if isinstance(od, dict):
        matched = float(od.get("size_matched", 0) or 0)
        if matched > 0:
            return _normalize_result(
                side=side_u,
                fallback_price=price,
                resp=od,
                order_id=order_id,
                client=client,
                token_id=str(token_id),
                submit_ts=submit_ts,
                post_order_type="GTC",
                side_ask=side_ask,
                side_bid=side_bid,
                intent=intent,
            )
    return {
        "success": False,
        "error": "GTC timeout or 0 fill",
        "price": price,
        "size_shares": 0.0,
        "order_id": order_id,
        "status": "timeout",
        "post_order_type": "GTC",
    }
