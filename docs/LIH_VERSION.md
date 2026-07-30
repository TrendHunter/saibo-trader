# LIH 策略版本留档

本文档记录 **Leg-In Hedge（LIH）** 实盘栈的功能版本、配置基线与待开发项。  
Git 以 commit hash 为准；语义版本便于口头对齐。

---

## 当前基线：`v0.12.2-rest-decision`

| 项 | 值 |
|---|---|
| **决策报价** | `LIH_QUOTE_MODE=rest` — Leg1 + 对冲检测 **仅用 REST**（2s 轮询，对齐 Python） |
| **执行** | 保留 `OrderRouter` 下单前 REST 复核 + hedge skip |
| **Leg1 band** | `LIH_LEG1_TRIGGER_MIN=0.70` … `MAX=0.75` |
| **风控试跑** | `RISK_MAX_POSITION_FRACTION=0.99`（≈关闭仓位比例 cap；并发等仍生效） |
| **末段** | `LIH_MIN_SECONDS_REMAINING=0`，`LIH_ENDGAME_SECS=0` nosoft |

---

## v0.12.1 留档：`trigger-band`（WS 决策，0 hedge）

band 0.70–0.75 挡住 0.99，但 C++ 仍 0 hedge（max(WS,REST) 对冲过严）。

---

## v0.12.0 留档：`trigger-nosoft`（无上限，已 superseded）

| 项 | 值 |
|---|---|
| **主策略** | trigger ≥0.70，**无上限** → 可能买 0.99 |
| **Shadow 8h** | Python REST nosoft，$50→$99.73（+49.73） |
| **问题** | C++ WS 进场 vs REST 对冲不一致；0 对冲 / 单腿赌 |

### 2026-06-24 30min Py vs C++ 对比（v0.12.0 配置）

| 轨道 | leg1 | 配对/hedge | PnL | 备注 |
|------|------|------------|-----|------|
| **Python REST** | 10 | 9 paired | **+$4.90** | 7× profit 路径 |
| **C++ WS** | 12 | **0 hedge** | **+$17.40** | 全单腿方向赌，8 赢 2 亏 — 非 LIH 配对 |

结论：参数名一致 ≠ 行为一致；**报价源与执行二次校验**是主因。v0.12.1 先加 trigger  band，仍不行再上 REST 决策。

---

## v0.11.0（cheap-leg + trend，仍可用）

- **`LIH_LEG1_MODE=cheap`**：买 ask ≤ `LIH_LEG1_MAX_PRICE`（0.45）。
- **`LIH_LEG1_MODE=trend`**：Binance 顺势买贵腿 ≤ 0.65。
- 末段软顶 **1.15** + T≤50s override；开局延迟 5s。
- VPS 早期实盘盈利（如 0.44+0.50 对冲 +$0.51）多属此模式。

---

## 主要 env（v0.12.1 — trigger band + nosoft）

```env
LIH_ENABLED=true
LIH_LEG1_MODE=trigger
LIH_LEG1_TRIGGER_MIN=0.70
LIH_LEG1_TRIGGER_MAX=0.75
LIH_TARGET_COMBINED=0.94
LIH_LEG1_SHARES=10
LIH_LEG1_START_DELAY_SEC=0
LIH_ALLOW_OVER_TARGET=false
LIH_ENDGAME_SECS=0
LIH_ENDGAME_SOFT_CAP=0.94
LIH_ENDGAME_OVERRIDE_SECS=0
LIH_ENDGAME_LADDER_ENABLED=false
LIH_ENDGAME_MINIMIZE_GAP=true
LIH_MAX_ENTRY_MARGINAL=0
LIVE_LIH_DRY_RUN=true
RISK_MAX_CONCURRENT_POSITIONS=2
```

---

## 已知限制

| 限制 | 说明 |
|------|------|
| C++ Leg1 用 max(WS,REST) | 与 Python REST-only 不一致；计划 `LIH_QUOTE_MODE=rest` |
| Hedge 执行二次 REST | 检测通过仍可能 `hedge skip` |
| Pending 无即时重下单 | abandon 后下一 tick 再试 |
| Web 未暴露 trigger band | `LIH_LEG1_TRIGGER_*` 需改 `.env` |

---

## 版本历史

| 版本 | 摘要 |
|------|------|
| **v0.12.1-trigger-band** | trigger 0.70–0.75；Py/C++ 30m 对比；待 REST 对齐 |
| **v0.12.0-trigger-nosoft** | trigger≥0.70 无顶；8h Py +$49；C++ 0 对冲 |
| **v0.11.0-dual-mode** | cheap/trend；endgame 1.15 |
| **v0.10.0-endgame** | 末段配平、软顶 1.15 |
| **v0.9.0-lih-baseline** | Leg1 趋势 + force balance |

---

## 相关文档

- [`README.md`](../README.md)
- [`docs/LIH_STRATEGY_ROADMAP.md`](LIH_STRATEGY_ROADMAP.md) — 五方案设计
- [`docs/LIH_EXPERIMENT_LOG.md`](LIH_EXPERIMENT_LOG.md) — **shadow 盈亏记录 / 勿重复清单**
- [`scripts/README.md`](../scripts/README.md)
- [`.env.example`](../.env.example)
