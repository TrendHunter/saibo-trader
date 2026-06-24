# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A Polymarket arbitrage bot trading binary "Up or Down" markets (BTC/ETH/SOL/XRP, 5m & 15m windows) against a Binance reference feed. Three tiers, three languages:

1. **`trading-core/`** — C++20 trading engine (Boost.Asio/Beast, OpenSSL, spdlog, secp256k1 via FetchContent). All trading logic lives here.
2. **Python glue at repo root** — `dashboard_bridge.py` (WS server), `cli_dashboard.py` (Rich terminal UI), plus helper scripts the C++ core shells out to.
3. **`frontend/`** — Next.js 16 web dashboard (Prisma + SQLite, NextAuth, Tailwind 4).

The active strategy is **Leg-In Hedge (LIH)** only (`LegInHedgeDetector`). Default: **trigger mode** + endgame ladder. **Dump Hedge removed** (2026-06-17); old tree in `backup/source-snapshot-2026-06-17.zip`. Market toggles still use env prefix `DH_ENABLE_*`. See `README.md` and `docs/LIH_VERSION.md`.

## Commands

```bash
# Build C++ core (Conan + CMake + Ninja; auto-creates .venv with tools if missing)
./build.sh                      # output: build/trading-core; incremental
rm -rf build/CMakeCache.txt build/CMakeFiles   # force clean build

# Run locally (live by default; use LIVE_LIH_DRY_RUN=true for shadow)
cp .env.example .env            # configure wallet + LIH params
./start.sh                      # Windows: ./start_windows.ps1

# Frontend (run from frontend/)
npm run dev
npm run build
npm run lint                    # eslint
npx prisma db push && npx tsx prisma/seed.ts   # SQLite at prisma/dev.db

# Docker (single instance: bot WS on :8080, frontend on :3001, admin/admin)
docker compose up -d --build
docker compose restart bot      # after .env changes
```

There is no test suite. `test_auth.py`, `test_json.py`, `test_sandbox.py` are ad-hoc manual scripts.

## Architecture

### Data flow

```
trading-core (C++) ──stdout JSON lines──> dashboard_bridge.py ──ws://0.0.0.0:8080──> cli_dashboard.py
                                                                                └──> frontend /api/live (BOT_WS_URL, default ws://127.0.0.1:8080)
```

- The core prints its full state as single-line JSON to **stdout**; logs go to stderr. The bridge spawns the core as a subprocess, captures stdout, and broadcasts each JSON line to all WebSocket clients. Anything printed to stdout that isn't `{...}` JSON breaks nothing but is treated as a log line — keep stdout JSON-clean when editing the core.
- Design rule: the C++ core is never exposed to the internet; the frontend only observes via the bridge.

### C++ core (`trading-core/src/`)

- `main.cpp` — orchestrator: parses `.env` itself (`load_env(".env")`, no library), runs the event loop, fetches USDC balance via Polygon RPC, triggers auto-redeem.
- `signals/LegInHedgeDetector` — LIH strategy; `feeds/` — Binance/Polymarket/Gamma; `risk/RiskManager`; `exec/OrderRouter`; `state/StateStore`.
- Adding a `.cpp` file requires listing it in `trading-core/CMakeLists.txt` `SOURCES`.

### C++ ↔ Python coupling

The core invokes Python at runtime via `popen` from its working directory (repo root):

- `fetch_balance.py` — live tradable USDC balance
- `redeem_positions.py` — on-chain CTF redeem for resolved markets (`AUTO_REDEEM=true`)
- `derive_and_update_keys.py` — derives Polymarket L2 API creds (`POLY_API_*`) and **writes them back into `.env`**; run by `start.sh` before the core starts. Live mode refuses to start without them.

So the core must run from the repo root, and changes to these scripts' stdout format can break the core's parsing.

### Frontend (`frontend/`)

- Heed `frontend/AGENTS.md`: this Next.js version has breaking changes — read `node_modules/next/dist/docs/` before writing Next.js code.
- `src/app/api/live/route.ts` proxies the bot WebSocket to the browser. Auth via NextAuth + Prisma (SQLite). Trade history is read from the bot's log files via `src/lib/tradeLog.ts`.

### Configuration

Everything is driven by the root `.env` (see `.env.example`) and `web.env` for the Next.js dashboard. Key bot vars: wallet keys, `RISK_*`, LIH tuning, `DH_ENABLE_*` (market toggles), `AUTO_REDEEM`.

## Docs

- `README.md` — current architecture, LIH flow, ops commands
- `deploy/README.md` — Docker single/multi-instance and bare-metal systemd (Chinese)
- `backup/` — pre-cleanup full source zip
- `README.md` — architecture and ops
- `deploy/LIVE_READINESS.md`, `manual.md` — operations notes
