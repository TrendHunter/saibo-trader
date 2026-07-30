# 偏热门早进旁路（Fav-Early Bypass）

> 状态：**代码已合入，默认关闭**。先 shadow，再考虑 VPS。  
> 研究来源：`mm2_fav_early_features` / `mm2_fav_early_coverage` / `mm2_jul18_shift_compare`

## 要解决什么

Bot 第一道时钟门：`secs_left > 180` → `mm2 wait entry window`，**不评估盘口**。  
mm2 约 1/4 成交在窗内 &lt;120s，且偏热门侧质量不错。本旁路在时钟门之外开一条窄缝。

## 开关（默认全关）

```bash
LIH_MM2_FAV_EARLY_BYPASS=false   # true 才启用
LIH_MM2_FAV_EARLY_MODE=B        # B 推荐；C 更严
LIH_MM2_FAV_EARLY_FAV_LO=0.52   # 仅 Mode C
LIH_MM2_FAV_EARLY_FAV_HI=0.65
LIH_MM2_FAV_EARLY_MIN_SPREAD=0.15
LIH_MM2_FAV_EARLY_MIN_DTILT=0.10
```

仍要求：`secs_left ≥ ENTRY_MIN`、有现货、非 flat、深度够；**选侧固定为 expensive（偏热门）**。  
命中标签：`fav_early_b` / `fav_early_c`。

## Mode 对照（全量 m2 假想）

| Mode | 条件 | 全期覆盖 | 全期 pnl | 调策略段(7/18–26) |
|------|------|----------|----------|------------------|
| **B** | fav≥0.48 且 (spread≥0.15 ∨ \|Δtilt\|≥0.10) | ~12.5% | +10608 | n=71, **+5068** |
| C | fav∈[0.52,0.65] 且同上 | ~3% | +1750 | n=20, 仅 +132 |

**建议先 shadow 跑 Mode B。** C 在调策略段变薄，不适合当第一刀。

## 7/18 漂移（为何现在做）

| 段 | favorite% | 过早% | 窄门B 覆盖 |
|----|-----------|-------|-----------|
| 7/04–17 | 27% | 25% | 12% |
| 7/18–26 | **42%** | 30% | **19%** |
| 7/18 当日 | **66%** | 41% | 29 窗 / +3881 |

阶段性偏热门早进，不是匀速漂移；旁路对准的是这条缝。

## Shadow 启用步骤（未自动部署）

1. 确认 `LIVE_LIH_DRY_RUN=true`
2. `.env` 设 `LIH_MM2_FAV_EARLY_BYPASS=true`、`MODE=B`
3. 本地或 VPS 重建 core 后重启 bridge
4. 看 telemetry：`fav_early_b` / 仍大量 `wait entry window` 属正常
5. 对照 mm2_only：过早+买热门窗是否开始出现 bot LEG1

## 风险

- 早段噪声：7/20–21 窄门B 曾单日为负  
- 跨窗 `leg1_sec<0` 本旁路不特判（靠当前窗时钟）；勿同时乱开 cheaper_later  
- **不要**在未 shadow 验证前上实盘
