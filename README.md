# Polymarket LIH Bot — C++ 交易核心

Polymarket **5m / 15m Up-Down** 市场（BTC / ETH / SOL）自动交易。主策略 **LIH（Leg-In Hedge）**：先建仓一条腿，再对冲到目标合价。

[![C++](https://img.shields.io/badge/C++-20-blue)](https://isocpp.org)
[![Polygon](https://img.shields.io/badge/Network-Polygon-purple)](https://polygon.technology)

> **Dump Hedge 已移除**（2026-06-17）。旧代码见 `backup/source-snapshot-2026-06-17.zip`。

---

## 策略逻辑（一局）— **Trigger 模式**（仓库默认 / Shadow 验证基线）

> 旧 **Cheap-Leg**（≤0.45 + 软顶 1.15）见下文「遗留模式」。顺势买贵腿：`LIH_LEG1_MODE=trend` 或 [`saibo-trader-trend`](https://github.com/TrendHunter/saibo-trader-trend)（[`README_TREND.md`](README_TREND.md)）。

```
任一侧 ask≥0.70 → 买更高 ask 一侧(leg1) → 利润对冲(合价≤0.94) → 末段补缺腿 → 结算/redeem
```

| 阶段 | 条件 | 说明 |
|------|------|------|
| **Leg1** | `LIH_LEG1_MODE=trigger`，任一侧 ask ≥ `LIH_LEG1_TRIGGER_MIN`（**0.70**） | 买 **ask 更高** 的一侧（与 shadow `leg1_trigger` 一致） |
| **份数** | `LIH_LEG1_SHARES=20` | 主仓 leg1 / 利润对冲用此份数（非 `MIN_ORDER_SIZE`） |
| **利润对冲** | `leg1_avg + light_ask ≤ LIH_TARGET_COMBINED`（**0.94**） | 窗口中段：一次性或 flex 利润对冲 |
| **末段 T≤100s** | 仍有 share gap | **5/10 份**分批买缺腿 |
| **末段 T≤90s 阶梯** | `LIH_ENDGAME_LADDER` | 合价上限随时间 **0.95→0.96→0.97**；>0.97 不买 |
| **末段 T 100–90s** | 仅利润价 | 合价 **>0.94 仍不对冲**（敞口保持，等阶梯开窗） |
| **MINIMIZE_GAP** | `LIH_ENDGAME_MINIMIZE_GAP=true` | 末段不因顺势盈利腿 hold |
| **结算** | 市场到期 | `AUTO_REDEEM=true` 链上 redeem |

**Shadow 对照**（`scripts/shadow_first_to_60.py` + `shadow_clob_ws.py`）：$50 共用资金池、BTC+ETH 并行、`leg1_trigger 0.70`、profit≤0.94、无 soft；9h WS 20 份约 +$137（见 `scripts/_analyze_9h_ws20.py`）。实盘与 shadow 差异：风控上限、pending 不重试、Web 暂停门控。

**保守实盘**：`LIH_MIN_BALANCE_USDC`、窗口最后 30s 不开新 leg1；**默认 PAUSED**（`STOP_TRADING` / `LIVE_LIH_DRY_RUN=true`），Web Resume 或改 env 后才下单。单币试跑：设 `DH_ENABLE_5M_ETH=false`、`LIH_ONE_SLOT_GLOBAL=true`，钱包 ~$20 用 10 份、~$50 用 20 份。

**版本留档**：[`docs/LIH_VERSION.md`](docs/LIH_VERSION.md)（当前 `v0.12.0-trigger-nosoft`）。

### 最小下单量（份数 vs 金额）

| 配置 | 作用 |
|------|------|
| **`MIN_ORDER_SIZE=5`** | 仅 **Dump Hedge** 配对整单：每条腿至少 5 份。LIH **不读**此项定 leg1 份数。 |
| **`LIH_LEG1_SHARES`** | Leg1 与主路径利润对冲的份数（默认 **20**）。 |
| **`LIH_ENDGAME_STEP_SHARES_SMALL/LARGE`** | 末段补缺腿：gap 小用 **5 份**，大用 **10 份**（会出现 5 份小单，仅末段）。 |
| **交易所规则** | CLOB 单笔约 **$1 名义**（`price × shares ≥ 1`）；代码里 `leg_meets_minimum` / `kLegMinUsdc=1.0`。不是「全局最小 5 份」。 |

例：leg1 20 份 @0.70 ≈ $14；末段 gap=6 可能下 **5 份** 缺腿单。若利润价（≤0.94）一直等不到，末段会在合价 ≤1.08 时仍分批缩 gap；最后 25s override 继续小步关 gap。

### 配平与敞口（gap）

原则：**先追求利润对冲（≤0.94），配不到则在末段尽量缩小 share gap，避免大敞口裸奔。**

| 时段 | 行为 |
|------|------|
| 窗口中段 | 仅当 `heavy_avg + light_ask ≤ 0.94` 对冲 |
| 末段 T≤100s | gap>0.5 时 5/10 份小步买缺腿 |
| 末段 T≤90s | **阶梯**：随剩余时间放宽合价上限 0.95→0.96→0.97；**>0.97 绝不买** |
| 末段 T 100–90s | 仍只认 ≤0.94（阶梯未开，敞口可能保持约 10s） |
| `MINIMIZE_GAP=true` | 关闭末段 hold |

Shadow **nosoft**（不接受 >0.94）与实盘阶梯（最多 0.97 缩 gap）统计口径不同。

### Leg1 / 对冲锁（不留尾巴）

`RiskManager` 维护 leg1 in-flight 与 rebalance 锁。Round 结束或异常路径主动释放；**`scrub_lih_inflight_locks`** 周期性清理（120s TTL），避免上一局结束后卡死下一窗口。

---

## 遗留模式：Cheap-Leg（`LIH_LEG1_MODE=cheap`）

```
开盘 +5s → leg1 便宜腿(≤0.45)+趋势过滤 → 利润对冲(≤0.94) → 末段配平(软顶1.15) → 结算
```

| 阶段 | 条件 |
|------|------|
| 开局延迟 | `LIH_LEG1_START_DELAY_SEC=5` |
| Leg1 | ask ≤ `LIH_LEG1_MAX_PRICE`（0.45）+ 可选 `LIH_LEG1_TREND_ALIGN` |
| 末段 override | `LIH_ENDGAME_OVERRIDE_SECS=50` 可突破软顶 1.15 |

VPS 曾用此 profile；当前 `.env.example` 已切到 trigger nosoft。Web 控件仍偏向 cheap-leg 字段，trigger 需直接改 `.env`。

---

## 技术栈

| 层级 | 技术 | 作用 |
|------|------|------|
| **交易核心** | C++20 · CMake · Conan · Boost · spdlog · OpenSSL | 行情、LIH 检测、风控、下单编排 |
| **Python 桥接** | asyncio · py-clob-client | `dashboard_bridge.py` WS + HTTP API、`clob_live.py` 实盘下单、reconcile / redeem |
| **Shadow** | `scripts/shadow_first_to_60.py` | WS+REST 簿、无 CLOB 下单、银行roll 统计 |
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

行情：CLOB **WS 主路径** + REST 约 2.5s 兜底（WS 缓存 2s 过期则 REST 刷新，与 shadow 一致）。

---

## 运行模式

| 模式 | 配置 | 说明 |
|------|------|------|
| **暂停 / Shadow** | `STOP_TRADING=true` 或 `LIVE_LIH_DRY_RUN=true` | 默认：验簿、打日志，不下单 |
| **实盘 LIVE** | `STOP_TRADING=false` 且 `LIVE_LIH_DRY_RUN=false` | 真实 CLOB 下单 |

钱包与策略在 **`.env`**；Web 登录在 **`web.env`**（见 [`web.env.example`](web.env.example)）。

---

## mm2 对齐 Shadow（当前实验，2026-07）

VPS shadow 主路径是 **`LIH_LEG1_MODE=mm2`**（对照外部 tracker `m2` 包），不是上面的 trigger 基线。目标：少做 bot 独有的垃圾窗，提高同窗率。

### 术语

| 词 | 含义 |
|----|------|
| **同窗** | 同一 `window_start_ts`，bot 与 m2 **都开了** |
| **bot-only** | bot 开了、m2 没开（多为筛窗过宽：session_off / flat_book / 过早 cheap 等） |
| **m2-only** | m2 开了、bot 没开（漏信号 / 门禁过严 / 数据缺口） |

盈利差经常集中在 **only**：那是「该不该做这窗」的分歧；同窗差才是选边/执行差。

### 已上线（shadow，默认 dry-run）

| 步骤 | 开关 | 作用 |
|------|------|------|
| **session 对齐** | `LIH_MM2_SESSION_FROM_OBS=true` | 只在 m2 `session_active` 小时开；skip：`mm2 session off` |
| **early YES 补洞** | `LIH_MM2_EARLY_YES_GUARD=true` | early 选 YES 须 **YES 本身是 favorite** 且 `yes≥0.65`、`spread≥0.35`（旧版用 `max(yes,no)`，cheap YES 会漏过） |
| **fav-early B** | `LIH_MM2_FAV_EARLY_BYPASS=true` | 时钟门前偏热门旁路；见 [`docs/mm2-fav-early-bypass.md`](docs/mm2-fav-early-bypass.md) |

阈值复用：`LIH_MM2_EARLY_TILT_MIN_SPREAD=0.35`、`LIH_MM2_EARLY_TILT_MIN_FAV=0.65`。Session 小时来自 `m2/YYYY-MM-DD/windows.jsonl` 或 `data/mm2_session_active.json`（`scripts/mm2_session_refresh.py`）。

### Shadow markers（VPS `shadow_trades.jsonl`）

| Marker | 内容 |
|--------|------|
| `bot-fav-early-bypass-b-20260727` | fav-early B |
| `bot-early-yes-guard-bprime-20260728` | 旧 B′（洞未补） |
| `bot-early-yes-guard-bprime-sessionalign-20260729` | + session 对齐（前 20 单约 **+111**，旧同口径约 **-112**） |
| `bot-early-yes-holefix-sessionalign-20260730` | **当前**：session + YES 补洞（攒样本中） |

### 下一步（先观察，不叠刀）

1. 攒当前 marker 样本（建议 ≥20–30 单）再 bot↔m2 同窗对比  
2. 候选下一刀：`flat_book` 对齐、early **NO** 收紧、m2-only 漏窗  
3. 拉包：`python scripts/pull_tracker_packs.py --accounts m2 --latest 3`；观测：`python scripts/_pull_vps_observation_data.py --marker <marker>`  
4. **本地研究全量同步（推荐开机跑）**：`python scripts/sync_research_from_vps.py`  
   → 拉 shadow / lih_skip / m2·m3·m4 日包 / session 文件 / 脱敏 LIH env，并切当前 marker 样本到 `data/pull_*_research/`

分析备忘：`data/compare_triple/bprime_vs_sessionalign_compare.txt`、`early_yes_holefix_deploy_note.txt`（本地，可不入库）。

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
| Shadow 长跑 | `python scripts/shadow_first_to_60.py`（见 `scripts/README.md`） |
| 实盘前检查 | `python scripts/_preflight_live_test.py` |
| 单轮验证 | `python scripts/_watch_test_round.py --enable-live --expect-assets btc` |
| 紧急停开仓 | `python scripts/_emergency_stop_entries.py` |
| 暂停保留配置 | `python scripts/_pause_live_keep_config.py` |
| 链上补录 | `python scripts/live_lih_reconcile.py` |

5m slug：`{asset}-updown-5m-{unix_ts}`，`ts = (now // 300) * 300`。

---

## Docker 部署（可选）

`docker compose up -d --build` → `:3001`。多实例见 [deploy/README.md](deploy/README.md)。**线上当前用 VPS 裸跑，非 Docker。**

---

## 目录速查

| 路径 | 说明 |
|------|------|
| `.env.example` | Bot 策略 / 钱包（trigger nosoft 基线） |
| `web.env.example` | Web 登录 / NEXTAUTH |
| `docs/LIH_VERSION.md` | LIH 版本与 env 基线 |
| `docs/mm2-fav-early-bypass.md` | fav-early 旁路说明 |
| `scripts/README.md` | 运维 / shadow / 一次性脚本索引 |
| `scripts/pull_tracker_packs.py` | 从 VPS 拉 m2/m3/m4 日包 |
| `scripts/mm2_session_refresh.py` | 刷新 `mm2_session_active.json` |
| `server_start_bot.sh` / `server_start_web.sh` | VPS 启动脚本 |
| `build-lowmem.sh` | 低内存编译 |
| `scripts/deploy_production.py` | **一键部署** bot + web → VPS |
| `scripts/deploy_vps_full.sh` | VPS 上全量部署（与生产一致） |
| `.deploy.local.example` | SSH 密码模板（复制为 `.deploy.local`） |
| `scripts/remote_deploy.py` | 本地 → VPS 分模式部署 |

---

## 免责声明

仅供学习与研究。预测市场交易有风险，请先用 shadow 或小资金单轮验证，实盘自负盈亏。
