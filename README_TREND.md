# Polymarket LIH Bot — Trend-First（顺势买贵腿）

本仓库为 **顺势 leg1** 变体：与 [`saibo-trader`](https://github.com/TrendHunter/saibo-trader) 同源代码，默认 `LIH_LEG1_MODE=trend`。

## 策略逻辑（一局）

```
开盘 +5s → leg1 买 Binance 顺势一侧（ask ≤ 0.65）→ 利润对冲 ≤0.94 → 末段配平 → 结算
```

| 阶段 | 说明 |
|------|------|
| **开局延迟 5s** | 前 5 秒不买，等开盘波动 |
| **Leg1 trend** | YES 当 spot 涨 / NO 当 spot 跌；ask ≤ `LIH_LEG1_TREND_MAX_PRICE`（默认 0.65） |
| **利润对冲** | `heavy_avg + light_ask ≤ 0.94` |
| **末段 T≤100s** | 5/10 份补缺腿；软顶 1.15 |
| **Hold** | 持有腿 ≥0.90 且仍顺势 → 等结算；跌回 &lt;0.89 → 继续对冲 |
| **T≤50s** | 突破软顶关 gap；拒单后 2s 重试 |

## 关键 env

```env
LIH_LEG1_MODE=trend
LIH_LEG1_TREND_MAX_PRICE=0.65
LIH_TARGET_COMBINED=0.94
LIH_LEG1_START_DELAY_SEC=5
LIH_ENDGAME_SECS=100
LIH_ENDGAME_OVERRIDE_SECS=50
LIH_ENDGAME_OVERRIDE_COOLDOWN=2
```

Cheap-leg 模式见主仓库 `LIH_LEG1_MODE=cheap`。

## 部署

与主仓库相同；重启默认 **PAUSED**，Web Resume 后交易。
