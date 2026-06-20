#!/usr/bin/env python3
"""Rebuild open LIH leg1 state from Polymarket activity when C++ missed fills."""

from __future__ import annotations

import argparse
import json
import os
import sys
import time
from pathlib import Path

from dotenv import load_dotenv

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

load_dotenv(ROOT / ".env")

from clob_trades import fetch_user_trades, fetch_user_positions, parse_market_end_ts  # noqa: E402


def _outcome_side(outcome: str) -> str:
    o = (outcome or "").strip().lower()
    if o in ("yes", "up"):
        return "yes"
    if o in ("no", "down"):
        return "no"
    return ""


def _pair_sec() -> float:
    raw = os.getenv("LIH_RECONCILE_PAIR_SEC", "180").strip()
    try:
        return max(30.0, float(raw))
    except ValueError:
        return 180.0


def _legs_from_trades(trades: list[dict], *, now: float) -> list[dict]:
    """Extract active BUY legs from activity rows."""
    legs: list[dict] = []
    for t in trades:
        if str(t.get("side", "")).upper() != "BUY":
            continue
        title = str(t.get("title") or "")
        end_ts = parse_market_end_ts(title, ref_ts=now)
        if end_ts and now > end_ts + 30:
            continue
        asset = str(t.get("asset") or "").upper()
        if asset == "—":
            asset = ""
        asset = asset.lower() if asset else ""
        window = t.get("windowMinutes")
        if window not in (5, 15):
            window = None
        if not asset or window not in (5, 15):
            from clob_trades import _infer_asset, _infer_window_minutes

            asset = asset or _infer_asset(title)
            window = window or _infer_window_minutes(title)
        asset = (asset or "").lower()
        if not asset or asset == "—" or window not in (5, 15):
            continue
        ts = float(t.get("timestamp") or 0)
        win_min = int(window)
        if ts > 0 and now > ts + win_min * 60 + 120:
            continue
        leg = _outcome_side(str(t.get("outcome") or ""))
        if not leg:
            continue
        shares = float(t.get("size") or 0)
        if shares <= 0:
            continue
        cost = float(t.get("usdcSize") or 0) or (
            float(t.get("price") or 0) * shares
        )
        legs.append(
            {
                "asset": asset,
                "window_minutes": int(window),
                "title": title,
                "end_date_ts": end_ts or 0.0,
                "timestamp": float(t.get("timestamp") or 0),
                "leg": leg,
                "shares": shares,
                "cost": cost,
                "token_id": str(t.get("tokenID") or ""),
            }
        )
    return legs


def _merge_leg_into_row(row: dict, leg: dict) -> None:
    if leg["leg"] == "yes":
        row["yes_shares"] += leg["shares"]
        row["yes_cost"] += leg["cost"]
        if leg["token_id"]:
            row["yes_token_id"] = leg["token_id"]
    else:
        row["no_shares"] += leg["shares"]
        row["no_cost"] += leg["cost"]
        if leg["token_id"]:
            row["no_token_id"] = leg["token_id"]
    ts = leg["timestamp"]
    if ts and (not row["opened_at"] or ts < row["opened_at"]):
        row["opened_at"] = ts
        row["title"] = leg["title"]
        row["end_date_ts"] = leg["end_date_ts"] or row["end_date_ts"]


def _pair_legs_into_rounds(legs: list[dict], pair_sec: float) -> list[dict]:
    """Pair leg1 YES / hedge NO (or vice versa) within time window — not by market title."""
    legs = sorted(legs, key=lambda x: x["timestamp"])
    used: set[int] = set()
    rounds: list[dict] = []

    for i, a in enumerate(legs):
        if i in used:
            continue
        row = {
            "asset": a["asset"],
            "window_minutes": a["window_minutes"],
            "title": a["title"],
            "end_date_ts": a["end_date_ts"] or 0.0,
            "opened_at": a["timestamp"] or 0.0,
            "yes_shares": 0.0,
            "no_shares": 0.0,
            "yes_cost": 0.0,
            "no_cost": 0.0,
            "yes_token_id": "",
            "no_token_id": "",
        }
        _merge_leg_into_row(row, a)
        used.add(i)

        # Prefer opposite leg in same asset+window within pair_sec (LIH leg1→hedge).
        best_j: int | None = None
        best_dt = pair_sec + 1.0
        for j, b in enumerate(legs):
            if j in used:
                continue
            if a["asset"] != b["asset"] or a["window_minutes"] != b["window_minutes"]:
                continue
            if a["leg"] == b["leg"]:
                continue
            if not a["timestamp"] or not b["timestamp"]:
                continue
            dt = abs(a["timestamp"] - b["timestamp"])
            if dt <= pair_sec and dt < best_dt:
                best_j = j
                best_dt = dt
        if best_j is not None:
            _merge_leg_into_row(row, legs[best_j])
            used.add(best_j)

        # Same-side duplicates in same round (shouldn't happen often).
        for j, b in enumerate(legs):
            if j in used:
                continue
            if a["asset"] != b["asset"] or a["window_minutes"] != b["window_minutes"]:
                continue
            if a["leg"] != b["leg"]:
                continue
            if not a["timestamp"] or not b["timestamp"]:
                continue
            if abs(a["timestamp"] - b["timestamp"]) <= pair_sec:
                _merge_leg_into_row(row, b)
                used.add(j)

        rounds.append(row)
    return rounds


def _find_existing_lih_id(row: dict, open_lih: dict, pair_sec: float) -> str | None:
    """Match chain row to in-memory lih_id (avoid duplicate -recon rows)."""
    row_ts = float(row.get("opened_at") or 0)
    row_end = float(row.get("end_date_ts") or 0)
    asset = str(row.get("asset") or "").lower()
    window = int(row.get("window_minutes") or 0)

    best_id: str | None = None
    best_score = 10_000.0
    for lid, existing in open_lih.items():
        if str(existing.get("asset", "")).lower() != asset:
            continue
        if int(existing.get("window_minutes") or 0) != window:
            continue
        ex_ts = float(existing.get("opened_at") or 0)
        ex_end = float(existing.get("end_date_ts") or 0)
        if row_ts and ex_ts and abs(row_ts - ex_ts) <= pair_sec:
            score = abs(row_ts - ex_ts)
            if not str(lid).endswith("-recon"):
                score -= 0.5
            if score < best_score:
                best_score = score
                best_id = lid
            continue
        if row_end and ex_end and abs(row_end - ex_end) < 1.0:
            score = abs(row_end - ex_end) + 1.0
            if not str(lid).endswith("-recon"):
                score -= 0.5
            if score < best_score:
                best_score = score
                best_id = lid
    return best_id


def _drop_stale_recon(open_lih: dict, fresh_ids: set[str]) -> None:
    """Remove old -recon rows superseded by merged chain rounds."""
    for lid in list(open_lih.keys()):
        if str(lid).endswith("-recon") and lid not in fresh_ids:
            del open_lih[lid]


def _token_holdings(positions: list[dict]) -> dict[str, float]:
    """Sum on-chain size per outcome token id."""
    by_token: dict[str, float] = {}
    for p in positions:
        if isinstance(p, dict) and p.get("error"):
            continue
        tok = str(p.get("asset") or p.get("tokenId") or p.get("tokenID") or "")
        size = float(p.get("size") or 0)
        if tok and size > 0:
            by_token[tok] = by_token.get(tok, 0.0) + size
    return by_token


def _align_open_lih_from_positions(open_lih: dict, positions: list[dict]) -> int:
    """Raise yes/no share counts to match chain when bot ledger under-counts fills."""
    by_token = _token_holdings(positions)
    if not by_token:
        return 0
    fixed = 0
    for lid, pos in open_lih.items():
        for leg in ("yes", "no"):
            tok = str(pos.get(f"{leg}_token_id") or "")
            if not tok:
                continue
            chain = by_token.get(tok)
            if chain is None:
                continue
            key = f"{leg}_shares"
            old = float(pos.get(key) or 0)
            if chain <= old + 0.05:
                continue
            pos[key] = chain
            cost_key = f"{leg}_cost"
            old_cost = float(pos.get(cost_key) or 0)
            if old > 0 and old_cost > 0:
                pos[cost_key] = old_cost * (chain / old)
            avg_key = f"{leg}_entry_price"
            sh = float(pos.get(key) or 0)
            if sh > 0:
                pos[avg_key] = float(pos.get(cost_key) or 0) / sh
            print(f"chain_align {lid} {leg.upper()} {old:.2f} -> {chain:.2f} (token …{tok[-8:]})")
            fixed += 1
    return fixed


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--prune-only", action="store_true", help="only drop expired open rows")
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="print reconciled slots without writing live_state.json",
    )
    parser.add_argument(
        "--merge",
        action="store_true",
        help="merge chain slots into existing open_lih_positions instead of replacing",
    )
    parser.add_argument(
        "--positions-only",
        action="store_true",
        help="fast path: align open slot share counts from chain positions API only",
    )
    args = parser.parse_args()

    if args.prune_only:
        sys.path.insert(0, str(ROOT / "scripts"))
        from prune_live_lih import prune_live_state

        prune_live_state()
        return 0

    live_path = Path(os.getenv("LIVE_STATE_PATH", "logs/live_state.json"))

    if args.positions_only:
        if not live_path.is_file():
            print("positions_only: no live_state.json")
            return 0
        try:
            doc = json.loads(live_path.read_text(encoding="utf-8"))
        except json.JSONDecodeError as exc:
            print(f"positions_only: bad json: {exc}", file=sys.stderr)
            return 1
        if not isinstance(doc, dict):
            return 1
        open_lih = dict(doc.get("open_lih_positions") or {})
        if not open_lih:
            print("positions_only: no open slots")
            return 0
        positions = fetch_user_positions(limit=500)
        if positions and isinstance(positions[0], dict) and positions[0].get("error"):
            print("ERROR positions:", positions[0]["error"], file=sys.stderr)
            return 1
        n_align = _align_open_lih_from_positions(open_lih, positions)
        if n_align:
            doc["open_lih_positions"] = open_lih
            doc["saved_at"] = time.time()
            live_path.write_text(json.dumps(doc, ensure_ascii=False, indent=2), encoding="utf-8")
            print(f"positions_only: aligned {n_align} leg(s) -> {live_path}")
        else:
            print("positions_only: chain matches memory (no changes)")
        return 0

    now = time.time()
    pair_sec = _pair_sec()
    trades = fetch_user_trades(limit=80)
    if trades and isinstance(trades[0], dict) and trades[0].get("error"):
        print("ERROR:", trades[0]["error"], file=sys.stderr)
        return 1

    legs = _legs_from_trades(trades, now=now)
    rounds = _pair_legs_into_rounds(legs, pair_sec)

    if not rounds:
        print("No active LIH-style BUY trades to reconcile.")
        return 0

    doc: dict = {
        "version": 1,
        "saved_at": now,
        "current_balance": 0.0,
        "total_lih_trades": 0,
        "lih_pnl": 0.0,
        "closed_lih_positions": [],
        "lih_leg1_inflight": [],
    }
    if live_path.is_file():
        try:
            existing = json.loads(live_path.read_text(encoding="utf-8"))
            if isinstance(existing, dict):
                doc["current_balance"] = float(existing.get("current_balance") or 0)
                doc["total_lih_trades"] = int(existing.get("total_lih_trades") or 0)
                doc["lih_pnl"] = float(existing.get("lih_pnl") or 0)
                if isinstance(existing.get("closed_lih_positions"), list):
                    doc["closed_lih_positions"] = existing["closed_lih_positions"]
        except json.JSONDecodeError:
            pass

    if doc["current_balance"] <= 0:
        try:
            from fetch_balance import fetch_usdc_balance  # type: ignore

            doc["current_balance"] = float(fetch_usdc_balance())
        except Exception:
            pass

    open_lih: dict = {}
    if args.merge and live_path.is_file():
        try:
            existing = json.loads(live_path.read_text(encoding="utf-8"))
            if isinstance(existing.get("open_lih_positions"), dict):
                open_lih = dict(existing["open_lih_positions"])
        except json.JSONDecodeError:
            pass
    elif not args.merge:
        open_lih = {}

    fresh_ids: set[str] = set()
    for row in rounds:
        total_shares = row["yes_shares"] + row["no_shares"]
        if total_shares <= 0:
            continue
        total_cost = row["yes_cost"] + row["no_cost"]
        yes_avg = row["yes_cost"] / row["yes_shares"] if row["yes_shares"] > 0 else 0.0
        no_avg = row["no_cost"] / row["no_shares"] if row["no_shares"] > 0 else 0.0

        lih_id = f"LIH-{row['asset']}-{int(row['opened_at'] * 1000)}-recon"
        if args.merge:
            matched = _find_existing_lih_id(row, open_lih, pair_sec)
            if matched:
                lih_id = matched

        fresh_ids.add(lih_id)
        if args.merge and lih_id in open_lih:
            ex = open_lih[lih_id]
            ex_yes = float(ex.get("yes_shares") or 0)
            ex_no = float(ex.get("no_shares") or 0)
            ex_yc = float(ex.get("yes_cost") or 0)
            ex_nc = float(ex.get("no_cost") or 0)
            # Never wipe a hedged leg: activity rows may only show the latest BUY.
            row_yes = float(row["yes_shares"])
            row_no = float(row["no_shares"])
            row_yc = float(row["yes_cost"])
            row_nc = float(row["no_cost"])
            if row_yes < ex_yes - 1e-6:
                row_yes, row_yc = ex_yes, ex_yc
            if row_no < ex_no - 1e-6:
                row_no, row_nc = ex_no, ex_nc
            if row_yes > ex_yes + 1e-6:
                ex_yes, ex_yc = row_yes, row_yc
            if row_no > ex_no + 1e-6:
                ex_no, ex_nc = row_no, row_nc
            row = dict(row)
            row["yes_shares"] = ex_yes
            row["no_shares"] = ex_no
            row["yes_cost"] = ex_yc
            row["no_cost"] = ex_nc
            row["yes_token_id"] = row.get("yes_token_id") or ex.get("yes_token_id") or ""
            row["no_token_id"] = row.get("no_token_id") or ex.get("no_token_id") or ""
            total_cost = row["yes_cost"] + row["no_cost"]
            yes_avg = row["yes_cost"] / row["yes_shares"] if row["yes_shares"] > 0 else 0.0
            no_avg = row["no_cost"] / row["no_shares"] if row["no_shares"] > 0 else 0.0
        pos = {
            "lih_id": lih_id,
            "asset": row["asset"],
            "market_question": row["title"],
            "yes_token_id": row["yes_token_id"],
            "no_token_id": row["no_token_id"],
            "window_minutes": row["window_minutes"],
            "yes_shares": row["yes_shares"],
            "no_shares": row["no_shares"],
            "yes_cost": row["yes_cost"],
            "no_cost": row["no_cost"],
            "entry_fees": total_cost * 0.018,
            "opened_at": row["opened_at"] or now,
            "end_date_ts": row["end_date_ts"] or 0.0,
            "rebalance_count": int((open_lih.get(lih_id) or {}).get("rebalance_count") or 0)
            if args.merge and lih_id in open_lih
            else 0,
            "is_neg_risk": False,
            "paper_mode": False,
            "is_shadow": False,
            "exit_reason": "",
        }
        open_lih[lih_id] = pos
        held = "YES" if row["yes_shares"] > row["no_shares"] + 1e-6 else (
            "NO" if row["no_shares"] > row["yes_shares"] + 1e-6 else "BOTH"
        )
        print(
            f"reconcile {row['asset']} {row['window_minutes']}m {held} "
            f"Y={row['yes_shares']:.2f}@{yes_avg:.4f} N={row['no_shares']:.2f}@{no_avg:.4f} "
            f"(${total_cost:.2f}) id={lih_id}"
        )

    if args.merge:
        _drop_stale_recon(open_lih, fresh_ids)

    # Drop expired windows so reconcile cannot resurrect settled rounds as open.
    for lid in list(open_lih.keys()):
        pos = open_lih[lid]
        end = float(pos.get("end_date_ts") or 0)
        if end > 0 and now > end + 5:
            print(f"prune expired open {lid} (ended {end:.0f})")
            del open_lih[lid]

    positions = fetch_user_positions(limit=500)
    if positions and isinstance(positions[0], dict) and positions[0].get("error"):
        print("WARN positions:", positions[0]["error"], file=sys.stderr)
    else:
        n_align = _align_open_lih_from_positions(open_lih, positions)
        if n_align:
            print(f"chain_align: updated {n_align} leg(s) from on-chain positions")

    doc["open_lih_positions"] = open_lih
    doc["total_lih_trades"] = max(int(doc.get("total_lih_trades") or 0), len(open_lih))
    if args.dry_run:
        print(f"[dry-run] would write {len(open_lih)} open LIH slot(s) -> {live_path}")
        return 0
    live_path.parent.mkdir(parents=True, exist_ok=True)
    live_path.write_text(json.dumps(doc, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"Wrote {len(open_lih)} active open LIH slot(s) -> {live_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
