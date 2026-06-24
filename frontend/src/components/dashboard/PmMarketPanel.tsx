"use client";

import { useEffect, useMemo, useRef } from "react";
import { GlassCard, CardContent, CardHeader, CardTitle } from "@/components/shared/GlassCard";
import { createChart, ISeriesApi, LineSeries } from "lightweight-charts";
import { LineChart } from "lucide-react";
import type { MarketOpportunity } from "@/hooks/useLiveState";

interface PmMarketPanelProps {
  opportunities: MarketOpportunity[];
  dhSumTarget: number;
  dhMinDiscount: number;
  feeRate: number;
  timestamp: number;
  marketsScanned: number;
  lihEnabled?: boolean;
  lihTargetCombined?: number;
}

interface WindowConfig {
  minutes: number;
  label: string;
  assets: string[];
  tradeable: boolean;
}

const WINDOWS: WindowConfig[] = [
  { minutes: 5, label: "5 分钟", assets: ["btc", "eth", "sol"], tradeable: true },
  { minutes: 15, label: "15 分钟", assets: ["btc", "eth"], tradeable: true },
];

const ASSET_COLORS: Record<string, string> = {
  btc: "#818cf8",
  eth: "#6366f1",
  sol: "#14b8a6",
};

function dhNetDiscount(combined: number, feeRate: number): number {
  if (combined <= 0) return 0;
  return 1.0 - combined - combined * feeRate;
}

function shortQuestion(q: string): string {
  const m = q.match(/(Bitcoin|Ethereum|Solana|BTC|ETH|SOL)[^—-]*/i);
  return m ? m[0].trim() : q.slice(0, 36);
}

function seriesKey(windowMinutes: number, asset: string): string {
  return `${windowMinutes}m-${asset}`;
}

function bestByAsset(
  opportunities: MarketOpportunity[],
  windowMinutes: number,
  assets: string[]
): Map<string, MarketOpportunity> {
  const active = opportunities.filter(
    (o) =>
      (o.windowMinutes ?? 5) === windowMinutes &&
      assets.includes(o.asset) &&
      o.yesPrice > 0 &&
      o.noPrice > 0 &&
      o.combined > 0
  );
  const map = new Map<string, MarketOpportunity>();
  for (const o of active) {
    const prev = map.get(o.asset);
    if (!prev || (o.endDateTs ?? 0) < (prev.endDateTs ?? Infinity)) map.set(o.asset, o);
  }
  return map;
}

function WindowSection({
  config,
  opportunities,
  dhSumTarget,
  dhMinDiscount,
  feeRate,
  timestamp,
  lihMode = false,
  combinedTarget,
}: {
  config: WindowConfig;
  opportunities: MarketOpportunity[];
  dhSumTarget: number;
  dhMinDiscount: number;
  feeRate: number;
  timestamp: number;
  lihMode?: boolean;
  combinedTarget?: number;
}) {
  const chartRef = useRef<HTMLDivElement>(null);
  const seriesRef = useRef<Record<string, ISeriesApi<"Line">>>({});

  const active = useMemo(
    () =>
      opportunities.filter(
        (o) =>
          (o.windowMinutes ?? 5) === config.minutes &&
          config.assets.includes(o.asset) &&
          o.yesPrice > 0 &&
          o.noPrice > 0 &&
          o.combined > 0
      ),
    [opportunities, config.minutes, config.assets]
  );

  const bestMap = useMemo(
    () => bestByAsset(opportunities, config.minutes, config.assets),
    [opportunities, config.minutes, config.assets]
  );

  useEffect(() => {
    if (!chartRef.current || chartRef.current.clientWidth === 0) return;

    const chart = createChart(chartRef.current, {
      layout: {
        background: { color: "transparent" },
        textColor: "rgba(255,255,255,0.3)",
        fontFamily: "var(--font-jetbrains-mono)",
        fontSize: 10,
      },
      grid: {
        vertLines: { color: "rgba(255, 255, 255, 0.02)" },
        horzLines: { color: "rgba(255, 255, 255, 0.02)" },
      },
      rightPriceScale: { borderColor: "rgba(255, 255, 255, 0.05)" },
      timeScale: { borderColor: "rgba(255, 255, 255, 0.05)" },
      width: chartRef.current.clientWidth,
      height: 160,
    });

    for (const asset of config.assets) {
      const key = seriesKey(config.minutes, asset);
      seriesRef.current[key] = chart.addSeries(LineSeries, {
        color: ASSET_COLORS[asset] ?? "#94a3b8",
        lineWidth: 2,
        title: asset.toUpperCase(),
      });
    }

    const onResize = () => {
      if (chartRef.current) chart.applyOptions({ width: chartRef.current.clientWidth });
    };
    window.addEventListener("resize", onResize);
    return () => {
      window.removeEventListener("resize", onResize);
      chart.remove();
      for (const asset of config.assets) {
        delete seriesRef.current[seriesKey(config.minutes, asset)];
      }
    };
  }, [config.minutes, config.assets]);

  useEffect(() => {
    if (!timestamp) return;
    const time = Math.floor(timestamp / 1000) as never;
    for (const [asset, opp] of bestMap) {
      const s = seriesRef.current[seriesKey(config.minutes, asset)];
      if (s && opp.combined > 0) s.update({ time, value: opp.combined });
    }
  }, [timestamp, bestMap, config.minutes]);

  const target = combinedTarget ?? dhSumTarget;

  return (
    <div className="space-y-4 rounded-xl border border-white/5 bg-white/[0.015] p-4">
      <div className="flex items-center justify-between flex-wrap gap-2">
        <h3 className="text-sm font-semibold text-white/80">{config.label}</h3>
        <span className="text-[10px] font-mono text-white/35">
          {config.tradeable ? (lihMode ? "LIH 可交易" : "参与 DH 开仓") : "仅行情展示"}
        </span>
      </div>

      <div className={`grid gap-2 ${config.assets.length === 3 ? "sm:grid-cols-3" : "sm:grid-cols-2"}`}>
        {config.assets.map((asset) => {
          const o = bestMap.get(asset);
          const combined = o?.combined ?? 0;
          const net = combined > 0 ? dhNetDiscount(combined, feeRate) : 0;
          const ok = lihMode
            ? config.tradeable && combined > 0 && combined <= target
            : config.tradeable && combined > 0 && combined <= dhSumTarget && net >= dhMinDiscount;
          return (
            <div key={asset} className="rounded-lg border border-white/5 bg-white/[0.02] px-3 py-2.5">
              <div className="text-[10px] uppercase tracking-widest text-white/35">{asset}</div>
              <div className="font-mono text-lg text-white mt-1">
                {combined > 0 ? combined.toFixed(3) : "—"}
              </div>
              <div className={`text-[10px] font-mono mt-1 ${ok ? "text-emerald-400" : "text-white/40"}`}>
                {combined > 0
                  ? config.tradeable
                    ? `净折扣 ${(net * 100).toFixed(2)}% · ${ok ? "可开仓" : "未达标"}`
                    : `净折扣 ${(net * 100).toFixed(2)}%`
                  : "等待盘口…"}
              </div>
            </div>
          );
        })}
      </div>

      <div ref={chartRef} className="w-full h-[160px]" />

      <div className="overflow-x-auto rounded-lg border border-white/5">
        <table className="w-full text-[11px] font-mono">
          <thead>
            <tr className="text-white/35 border-b border-white/5">
              <th className="text-left py-2 px-3 font-medium">资产</th>
              <th className="text-right py-2 px-3 font-medium">YES</th>
              <th className="text-right py-2 px-3 font-medium">NO</th>
              <th className="text-right py-2 px-3 font-medium">合计</th>
              <th className="text-right py-2 px-3 font-medium">净折扣</th>
              {config.tradeable && <th className="text-right py-2 px-3 font-medium">状态</th>}
            </tr>
          </thead>
          <tbody>
            {active.length === 0 ? (
              <tr>
                <td colSpan={config.tradeable ? 6 : 5} className="py-5 text-center text-white/30">
                  暂无 {config.label} 盘口数据…
                </td>
              </tr>
            ) : (
              active.map((o, i) => {
                const net = dhNetDiscount(o.combined, feeRate);
                const ok = lihMode
                  ? o.combined <= target
                  : o.combined <= dhSumTarget && net >= dhMinDiscount;
                return (
                  <tr
                    key={`${o.asset}-${o.endDateTs ?? i}`}
                    className="border-b border-white/[0.03] text-white/70"
                  >
                    <td className="py-2 px-3">
                      <span className="uppercase text-white/90">{o.asset}</span>
                      <span
                        className="block text-[9px] text-white/30 truncate max-w-[200px]"
                        title={o.question}
                      >
                        {shortQuestion(o.question)}
                      </span>
                    </td>
                    <td className="text-right py-2 px-3">{o.yesPrice.toFixed(3)}</td>
                    <td className="text-right py-2 px-3">{o.noPrice.toFixed(3)}</td>
                    <td className="text-right py-2 px-3">{o.combined.toFixed(3)}</td>
                    <td className="text-right py-2 px-3">{(net * 100).toFixed(2)}%</td>
                    {config.tradeable && (
                      <td className={`text-right py-2 px-3 ${ok ? "text-emerald-400" : "text-white/35"}`}>
                        {ok ? "达标" : "未达标"}
                      </td>
                    )}
                  </tr>
                );
              })
            )}
          </tbody>
        </table>
      </div>
    </div>
  );
}

export function PmMarketPanel({
  opportunities,
  dhSumTarget,
  dhMinDiscount,
  feeRate,
  timestamp,
  marketsScanned,
  lihEnabled = true,
  lihTargetCombined,
}: PmMarketPanelProps) {
  const lihMode = lihEnabled;
  const combinedTarget = lihTargetCombined ?? dhSumTarget;

  return (
    <GlassCard>
      <CardHeader>
        <div className="flex items-center justify-between flex-wrap gap-2">
          <CardTitle className="font-heading text-lg font-semibold tracking-tight text-gradient flex items-center gap-2">
            <LineChart className="h-4 w-4" />
            实时行情
          </CardTitle>
          <div className="text-[11px] font-mono text-muted-foreground">
            扫描 {marketsScanned} 个市场 · {lihMode ? "LIH" : "DH"} 5m+15m · 合价参考 ≤{" "}
            {combinedTarget.toFixed(2)}
          </div>
        </div>
        <p className="text-[11px] text-muted-foreground mt-2 leading-relaxed">
          YES+NO 卖一合价。LIH 利润对冲与末段参考合价 ≤ {combinedTarget.toFixed(2)}。
        </p>
      </CardHeader>
      <CardContent className="space-y-5">
        {WINDOWS.map((config) => (
          <WindowSection
            key={config.minutes}
            config={config}
            opportunities={opportunities}
            dhSumTarget={dhSumTarget}
            dhMinDiscount={dhMinDiscount}
            feeRate={feeRate}
            timestamp={timestamp}
            lihMode={lihMode}
            combinedTarget={combinedTarget}
          />
        ))}
      </CardContent>
    </GlassCard>
  );
}
