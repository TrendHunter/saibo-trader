# LIH 策略版本留档

本文档记录 **Leg-In Hedge（LIH）** 实盘栈的功能版本、配置基线与待开发项。  
Git 以 commit hash 为准；语义版本便于口头对齐。

---

## 当前基线：`v0.12.0-trigger-nosoft`

| 项 | 值 |
|---|---|
| **主策略** | `LIH_LEG1_MODE=trigger` — 任一侧 ask≥0.70，买更高 ask 一侧 |
| **对冲** | `LIH_TARGET_COMBINED=0.94` 利润路径；末段 gap-shrink 至 `LIH_ENDGAME_SOFT_CAP=1.08` |
| **份数** | `LIH_LEG1_SHARES=20`；末段 5/10 步进补缺腿 |
| **并行** | `LIH_ONE_SLOT_GLOBAL=false`（BTC+ETH，与 shadow 一致） |
| **行情** | CLOB WS + REST 兜底（WS 2s stale 刷新） |
| **VPS** | `70.34.221.132` — trigger 配置已写入 `.env`；**暂停中**（`STOP_TRADING` / dry-run） |
| **Shadow 参考** | 9h WS nosoft 20sh：`scripts/_analyze_9h_ws20.py` |

---

## v0.12.0 新增（trigger + nosoft）

- **`LIH_LEG1_MODE=trigger`** + `LIH_LEG1_TRIGGER_MIN=0.70`：对齐 shadow `leg1_trigger`。
- **无末段软顶**：`LIH_ENDGAME_SOFT_CAP=0.94`，`LIH_ENDGAME_OVERRIDE_SECS=0`。
- **`LIH_LEG1_START_DELAY_SEC=0`**，`LIH_USE_MIRROR=false`，`LIH_ALLOW_OVER_TARGET=false`。
- **风控**：`RISK_MAX_POSITION_FRACTION=0.35`，`RISK_MAX_CONCURRENT_POSITIONS=2`（shadow 无 cap，实盘需限）。
- **最小单说明**：`MIN_ORDER_SIZE=5` 仅 DH；LIH leg1 用 `LIH_LEG1_SHARES`；交易所 floor ≈ **$1 名义**；末段可出现 **5 份**小单。

---

## v0.11.0（cheap-leg + trend 双模式，仍可用）

- **`LIH_LEG1_MODE=cheap`**：买 ask ≤ `LIH_LEG1_MAX_PRICE`（0.45）。
- **`LIH_LEG1_MODE=trend`**：Binance 顺势买贵腿 ≤ `LIH_LEG1_TREND_MAX_PRICE`（0.65）；见 [`saibo-trader-trend`](https://github.com/TrendHunter/saibo-trader-trend)。
- 末段软顶 **1.15** + T≤50s override；开局延迟 5s。

---

## 主要 env（v0.12 推荐 — trigger + gap-minimize）

```env
LIH_ENABLED=true
LIH_LEG1_MODE=trigger
LIH_LEG1_TRIGGER_MIN=0.70
LIH_TARGET_COMBINED=0.94
LIH_LEG1_SHARES=20
LIH_LEG1_START_DELAY_SEC=0
LIH_ALLOW_OVER_TARGET=false
LIH_ENDGAME_SECS=100
LIH_ENDGAME_SOFT_CAP=0.97
LIH_ENDGAME_OVERRIDE_SECS=0
LIH_ENDGAME_MINIMIZE_GAP=true
LIH_ENDGAME_LADDER_ENABLED=true
LIH_ENDGAME_LADDER_SECS=90
LIH_ENDGAME_LADDER_START=0.95
LIH_ENDGAME_LADDER_END=0.97
LIH_ENDGAME_LADDER_STEP=0.01
LIH_ENDGAME_OVERRIDE_COOLDOWN=2
LIH_ENDGAME_STEP_SHARES_SMALL=5
LIH_ENDGAME_STEP_SHARES_LARGE=10
LIH_ONE_SLOT_GLOBAL=false
LIH_MAX_USDC_PER_SLOT=0
LIH_MIN_BALANCE_USDC=15
LIVE_LIH_DRY_RUN=true
STOP_TRADING=true
```

单币小资金试跑：10 份、`LIH_ONE_SLOT_GLOBAL=true`、只开 `DH_ENABLE_5M_BTC=true`。

---

## 已知限制

| 限制 | 说明 |
|------|------|
| Pending 无即时重下单 | 挂单跟踪中不 retry；abandon 后下一 tick 再试 |
| Web 未暴露 trigger | `LIH_LEG1_TRIGGER_MIN` 等需改 `.env` |
| Shadow 无风控 cap | 实盘 `RiskManager` 仍会限仓 / 并发 |
| VPS git 脏树 | 服务器 `git pull` 可能失败；部署用 `deploy_production.py` 或 SFTP |

---

## 版本历史

| 版本 | 摘要 |
|------|------|
| **v0.12.0-trigger-nosoft** | trigger≥0.70；profit≤0.94 nosoft；20sh；shadow WS 验证 |
| **v0.11.0-dual-mode** | `LIH_LEG1_MODE` cheap/trend；5s 延迟；endgame T-100；override 50s |
| **v0.10.0-endgame** | 末段分批配平、软顶 1.15、顺势 hold ≥0.90 |
| **v0.9.0-lih-baseline** | Leg1 趋势过滤 + 60s force；启动暂停 + reconcile merge |

---

## 相关文档

- [`README.md`](../README.md) — 架构与 trigger 主策略
- [`README_TREND.md`](../README_TREND.md) — trend leg1 说明
- [`scripts/README.md`](../scripts/README.md) — shadow / 运维脚本
- [`docs/LIVE_LIH_ORDER_FLOW.md`](LIVE_LIH_ORDER_FLOW.md) — 下单链路
- [`.env.example`](../.env.example) — 配置模板
