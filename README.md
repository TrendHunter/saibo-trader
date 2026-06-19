# Polymarket LIH Bot — C++ 交易核心

Polymarket **5m / 15m Up-Down** 市场（BTC / ETH / SOL）自动交易。主策略 **LIH（Leg-In Hedge）**：先买便宜边，再对冲到目标合价。

[![C++](https://img.shields.io/badge/C++-20-blue)](https://isocpp.org)
[![Polygon](https://img.shields.io/badge/Network-Polygon-purple)](https://polygon.technology)

> 遗留 **Dump Hedge** 已归档：[`archive/dh-only/`](archive/dh-only/)（设 `LIH_ENABLED=false` 可恢复 DH-only）。

---

## 策略逻辑（一局）— **Cheap-Leg 模式**（VPS 默认）

> 顺势买贵腿模式：[`saibo-trader-trend`](https://github.com/TrendHunter/saibo-trader-trend) 或 `LIH_LEG1_MODE=trend`（见 [`README_TREND.md`](README_TREND.md)）。

```
开盘 +7s → leg1 便宜腿(≤0.45)+趋势过滤 → 利润对冲(≤0.94) → 末段配平 → 结算/redeem
```

| 阶段 | 条件 | 说明 |
|------|------|------|
| **开局延迟** | 开盘后 `LIH_LEG1_START_DELAY_SEC=7` | 前 7 秒 **不买**，等波动；7 秒后可买，**非强制** |
| **Leg1** | ask ≤ `LIH_LEG1_MAX_PRICE`（0.45）+ `LIH_LEG1_TREND_ALIGN` | 买更便宜一侧；逆势则跳过 |
| **利润对冲** | `heavy_avg + light_ask ≤ LIH_TARGET_COMBINED`（**0.94**） | 买对面配平 |
| **末段 T≤100s** | 有 gap | 5/10 份分批补缺腿；合价软顶 **1.15** |
| **末段 hold** | 持有腿 ask **≥0.90** 且 Binance **顺势** | 不配平，等结算；跌回 **<0.89** 或逆势 → 继续对冲 |
| **末段 T≤50s** | override | 可 **突破 1.15** 关 gap；拒单后 **2s** 再试 |
| **结算** | 市场到期 | `AUTO_REDEEM=true` 链上 redeem |

保守实盘：单槽、`LIH_ONE_SLOT_GLOBAL`、余额 &lt; $10 不开 leg1、窗口最后 30s 不开新 leg1；**重启默认 PAUSED**，Web Resume 才交易。

**Leg1 模式**：`LIH_LEG1_MODE=cheap`（本仓库 VPS 默认）。顺势买贵腿见 [`saibo-trader-trend`](https://github.com/TrendHunter/saibo-trader-trend) 或本地设 `LIH_LEG1_MODE=trend`（详见 [`README_TREND.md`](README_TREND.md)）。

**版本留档**：见 [`docs/LIH_VERSION.md`](docs/LIH_VERSION.md)（当前 `v0.11.0-dual-mode`）。

### Leg1 / 对冲锁（不留尾巴）

`RiskManager` 维护 leg1 in-flight 与 rebalance 锁。Round 结束或异常路径主动释放；**`scrub_lih_inflight_locks`** 周期性清理（120s TTL），避免上一局结束后卡死下一窗口。

---

## 技术栈

| 层级 | 技术 | 作用 |
|------|------|------|
| **交易核心** | C++20 · CMake · Conan · Boost · spdlog · OpenSSL | 行情、LIH 检测、风控、下单编排 |
| **Python 桥接** | asyncio · py-clob-client | `dashboard_bridge.py` WS + HTTP API、`clob_live.py` 实盘下单、reconcile / redeem |
| **Web 仪表盘** | Next.js 16 · Prisma/SQLite · NextAuth | 实时持仓/余额、暂停恢复、风控参数、历史 |
| **部署** | VPS 裸跑 · Docker · `remote_deploy.py` | 当前生产为 VPS 裸跑 |

**设计原则**：C++ 核心与 bot HTTP API（`:8081`）仅监听本机；公网只暴露 Next.js（`:3001`）。

---

## 架构

```
浏览器 → Next.js :3001 (web.env)
              │  服务端 proxy
              ▼
dashboard_bridge.py  WS :8080  HTTP :8081  (.env)
    │  spawn
    ▼
trading-core (C++)
    ├── LegInHedgeDetector / RiskManager / OrderRouter
    ├── clob_live.py → Polymarket CLOB
    └── redeem_positions.py → AUTO_REDEEM
```

---

## 运行模式

| 模式 | 配置 | 说明 |
|------|------|------|
| **实盘 LIVE** | `LIVE_LIH_DRY_RUN=false` | 真实 CLOB 下单（默认） |
| **Shadow** | `LIVE_LIH_DRY_RUN=true` | 只验簿、打日志，不下单 |

钱包与策略在 **`.env`**；Web 登录在 **`web.env`**（见 [`web.env.example`](web.env.example)）。

---

## VPS 裸跑部署（当前生产，与服务器一致）

默认路径 **`/opt/polymarket-bot`**。

### 1. Bot

```bash
cd /opt/polymarket-bot
git pull
cp .env.example .env          # 填 POLYMARKET_PRIVATE_KEY / FUNDER / SIGNER
python3 -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt
python3 derive_and_update_keys.py
bash build-lowmem.sh
bash server_start_bot.sh
```

### 2. Web 仪表盘

```bash
cp web.env.example web.env
# AUTH_USERNAME / AUTH_PASSWORD / NEXTAUTH_URL=http://<公网IP>:3001
bash server_start_web.sh       # 首次或改 frontend 后
bash server_restart_web.sh       # 日常快速重启
```

| 端口 | 服务 | 暴露 |
|------|------|------|
| 8080 / 8081 | Bot WS / API | 仅 127.0.0.1 |
| 3001 | Next.js | 公网（建议 HTTPS） |

### 3. 一键部署（推荐）

本地推代码后，一条命令同步到 VPS（`git pull` + `build-lowmem.sh` + Bot + Web），与当前生产环境一致：

```bash
# 1) 配置 SSH（仅首次）
cp .deploy.local.example .deploy.local   # 填入 DEPLOY_SSH_PASSWORD

# 2) 提交并推送
git push origin main

# 3) 一键部署 bot + web
python scripts/deploy_production.py

# 等价别名
python scripts/remote_deploy.py production
python scripts/remote_deploy.py deploy-full
```

| 选项 | 说明 |
|------|------|
| `--web-fast` | 不重编 frontend，仅 `server_restart_web.sh` |
| `--skip-build` | 跳过 C++ 编译，只 pull + 重启 |
| `--bot-only` | 只部署 bot |
| `--setup` | 首次克隆仓库、装依赖、建 web.env 模板 |
| `--force` | 本地未 push 也强制部署 |

服务器上也可手动跑：`bash scripts/deploy_vps_full.sh`（在 `/opt/polymarket-bot`）。

### 4. 分步推送（旧方式）

```bash
python scripts/remote_deploy.py          # bot + 编译
python scripts/remote_deploy.py web        # Web 全量
python scripts/_restart_bot_only.py        # 仅重启 bot
```

C++ 改动必须在 VPS 上 **`build-lowmem.sh`** 后重启才生效。

---

## 本地开发

```bash
cp .env.example .env
./build.sh
python start_bot.py
```

低内存 VPS：`bash build-lowmem.sh`。Windows：`./start_windows.ps1`。

---

## 运维命令

| 任务 | 命令 |
|------|------|
| VPS 部署 bot | `python scripts/remote_deploy.py` |
| VPS 部署 Web | `python scripts/remote_deploy.py web` |
| 实盘前检查 | `python scripts/_preflight_live_test.py` |
| 单轮验证 | `python scripts/_watch_test_round.py --enable-live --expect-assets btc` |
| 连开 N 局 | `python scripts/_watch_test_round.py --enable-live --rounds 2 --max-wait 1200` |
| 紧急停开仓 | `python scripts/_emergency_stop_entries.py` |
| 链上补录 | `python scripts/live_lih_reconcile.py` |

5m slug：`{asset}-updown-5m-{unix_ts}`，`ts = (now // 300) * 300`。

---

## Docker 部署（可选）

`docker compose up -d --build` → `:3001`。多实例见 [deploy/README.md](deploy/README.md)。**线上当前用 VPS 裸跑，非 Docker。**

---

## 目录速查

| 路径 | 说明 |
|------|------|
| `.env.example` | Bot 策略 / 钱包 |
| `web.env.example` | Web 登录 / NEXTAUTH |
| `server_start_bot.sh` / `server_start_web.sh` | VPS 启动脚本 |
| `build-lowmem.sh` | 低内存编译 |
| `scripts/deploy_production.py` | **一键部署** bot + web → VPS |
| `scripts/deploy_vps_full.sh` | VPS 上全量部署（与生产一致） |
| `.deploy.local.example` | SSH 密码模板（复制为 `.deploy.local`） |
| `scripts/remote_deploy.py` | 本地 → VPS 分模式部署 |

---

## 免责声明

仅供学习与研究。预测市场交易有风险，请先用 shadow 或小资金单轮验证，实盘自负盈亏。
