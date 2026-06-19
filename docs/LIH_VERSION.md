# LIH 策略版本留档

本文档记录 **Leg-In Hedge（LIH）** 实盘栈的功能版本、配置基线与待开发项。  
Git 以 commit hash 为准；语义版本便于口头对齐。

---

## 当前基线：`v0.11.0-dual-mode`

| 项 | 值 |
|---|---|
| **Git** | `fb7e9eb` — `feat: LIH_LEG1_MODE=trend` + `971ad76` 末段 50s override |
| **主仓库** | [`saibo-trader`](https://github.com/TrendHunter/saibo-trader) — `LIH_LEG1_MODE=cheap`（VPS 默认） |
| **Trend 仓库** | [`saibo-trader-trend`](https://github.com/TrendHunter/saibo-trader-trend) — `LIH_LEG1_MODE=trend` |
| **VPS** | `70.34.221.132` — cheap-leg + endgame；**暂停中**（`STOP_TRADING`） |

---

## v0.11.0 新增（双 leg1 模式）

- **`LIH_LEG1_MODE=cheap`**（主仓库 / VPS）：买 ask ≤ `LIH_LEG1_MAX_PRICE`（0.45）+ 可选 `LIH_LEG1_TREND_ALIGN`。
- **`LIH_LEG1_MODE=trend`**（[`saibo-trader-trend`](https://github.com/TrendHunter/saibo-trader-trend)）：Binance 顺势买贵腿，ask ≤ `LIH_LEG1_TREND_MAX_PRICE`（0.65）。
- **开局延迟** `LIH_LEG1_START_DELAY_SEC=7`：窗口开盘后前 7s 不买 leg1（非强制入场）。
- **目标合价** `LIH_TARGET_COMBINED=0.94`。
- **末段起点** `LIH_ENDGAME_SECS=100`（T≤100s 进入 endgame，替代旧 60s）。
- **Override** `LIH_ENDGAME_OVERRIDE_SECS=50`：最后 50s 可突破软顶 1.15。
- **末段重试** `LIH_ENDGAME_OVERRIDE_COOLDOWN=2`：拒单后 2s 再试。

---

## v0.10.0 末段逻辑（仍适用）

- **配平优先**：5/10 份小步买缺腿；gap≥10 用 10 份，否则 5 份。
- **软顶** `LIH_ENDGAME_SOFT_CAP=1.15`。
- **Hold 例外**：持有腿 ask≥0.90 且 Binance 顺势 → 不配平，等结算。
- **恢复配平**：ask 跌回 <0.89 或逆势 → 继续 endgame 配平。

---

## 主要 env（v0.11 推荐实盘基线 — cheap-leg VPS）

```env
LIH_ENABLED=true
LIH_LEG1_MODE=cheap
LIH_LEG1_MAX_PRICE=0.45
LIH_TARGET_COMBINED=0.94
LIH_LEG1_SHARES=10
LIH_LEG1_START_DELAY_SEC=7
LIH_LEG1_TREND_ALIGN=true
LIH_TREND_LOOKBACK_SEC=60
LIH_ENDGAME_SECS=100
LIH_ENDGAME_HOLD_ASK=0.90
LIH_ENDGAME_RESUME_HEDGE_ASK=0.89
LIH_ENDGAME_SOFT_CAP=1.15
LIH_ENDGAME_OVERRIDE_SECS=50
LIH_ENDGAME_OVERRIDE_COOLDOWN=2
LIH_REBALANCE_MODE=flex
LIH_LEG1_MIN_SECONDS_REMAINING=30
LIH_MIN_SECONDS_REMAINING=15
LIH_ONE_SLOT_GLOBAL=true
LIH_MAX_USDC_PER_SLOT=10
```

Trend 仓库将 `LIH_LEG1_MODE=trend`，`LIH_LEG1_TREND_MAX_PRICE=0.65`；其余末段参数同上。

---

## 已知限制

| 限制 | 说明 |
|------|------|
| Pending 无即时重下单 | 挂单跟踪中不 retry；abandon 后下一 tick 再试 |
| 末段仅买缺腿 | 主路径 CompleteHedge；无独立 paired 末段模式 |
| 极端流动性 | 无波动率门控；靠 T-100s 末段 + T-50s override + 2s 重试缓解 |
| VPS git 脏树 | 服务器 `git pull` 可能失败；部署需 SFTP 或清理本地改动 |

---

## 版本历史

| 版本 | Git | 摘要 |
|------|-----|------|
| **v0.11.0-dual-mode** | `fb7e9eb` | `LIH_LEG1_MODE` cheap/trend；7s 延迟；target 0.94；endgame T-100；override 50s |
| **v0.10.0-endgame** | `9b9c706` | 末段分批配平、软顶 1.15、顺势 hold ≥0.90 |
| **v0.9.0-lih-baseline** | `0268601` | Leg1 趋势过滤 + 60s force；启动暂停 + reconcile merge |
| v0.8.x | `6da72c1` | Reconcile merge 不抹对侧腿 |
| v0.8.x | `36c6e72` | 重启暂停、链上 reconcile、CLOB pending |
| v0.7.x | `82585f0` | 同窗口 hedge 匹配、history dedupe |

---

## 相关文档

- [`README.md`](../README.md) — 架构与日常命令（cheap-leg）
- [`README_TREND.md`](../README_TREND.md) — trend leg1 说明
- [`docs/LIVE_LIH_ORDER_FLOW.md`](LIVE_LIH_ORDER_FLOW.md) — 下单链路
- [`.env.example`](../.env.example) — 配置模板
