"use client";

import { useEffect, useState } from "react";
import { GlassCard, CardContent, CardHeader, CardTitle } from "@/components/shared/GlassCard";
import type { LiveState, OpenPosition } from "@/hooks/useLiveState";
import {
  cumulativeClosedTrades,
  isLihPrimary,
  strategyRealizedPnl,
  strategyShortLabel,
} from "@/lib/strategyMode";
import { Briefcase, Radio, Zap, Clock, TrendingUp, TrendingDown } from "lucide-react";
import { classifyTradeLog, dedupeTradeTelemetry } from "@/lib/tradeLog";

function logStyle(line: string) {
  const { color, label } = classifyTradeLog(line);
  return { color, label };
}

function FeedList({ lines, emptyText }: { lines: string[]; emptyText: string }) {
  const reversed = [...lines].reverse().slice(0, 30);

  if (reversed.length === 0) {
    return <div className="py-10 text-center text-white/30 text-xs font-mono">{emptyText}</div>;
  }

  return (
    <div className="max-h-[320px] overflow-y-auto space-y-1.5 pr-1 font-mono text-[11px]">
      {reversed.map((line, idx) => {
        const style = logStyle(line);
        return (
          <div
            key={`${idx}-${line.slice(0, 24)}`}
            className="flex gap-2 rounded-lg bg-white/[0.03] px-3 py-2 border border-white/5"
          >
            <span className={`shrink-0 text-[9px] font-bold uppercase tracking-wider px-1.5 py-0.5 rounded ${style.color} bg-white/5`}>
              {style.label}
            </span>
            <span className={`${style.color} break-all leading-relaxed`}>{line}</span>
          </div>
        );
      })}
    </div>
  );
}

function Countdown({ endDateTs }: { endDateTs?: number }) {
  const [remaining, setRemaining] = useState<number | null>(null);

  useEffect(() => {
    if (!endDateTs) {
      setRemaining(null);
      return;
    }
    const tick = () => setRemaining(Math.max(0, endDateTs - Date.now() / 1000));
    tick();
    const id = setInterval(tick, 1000);
    return () => clearInterval(id);
  }, [endDateTs]);

  if (remaining === null) return <span className="text-white/30">--</span>;
  const m = Math.floor(remaining / 60);
  const s = Math.floor(remaining % 60);
  const urgent = remaining < 60;
  return (
    <span className={`font-mono font-bold ${urgent ? "text-amber-400" : "text-white/70"}`}>
      {m}:{s.toString().padStart(2, "0")}
    </span>
  );
}

function LegRow({
  label,
  icon: Icon,
  entry,
  live,
  size,
  cost,
  active,
  color,
}: {
  label: string;
  icon: typeof TrendingUp;
  entry: number;
  live: number;
  size: number;
  cost: number;
  active: boolean;
  color: string;
}) {
  const pnl = active && entry > 0 && live > 0 ? (live - entry) * size : 0;

  return (
    <div className={`rounded-xl border p-3 ${active ? "border-white/15 bg-white/[0.04]" : "border-white/5 bg-white/[0.02] opacity-50"}`}>
      <div className="flex items-center justify-between mb-2">
        <div className="flex items-center gap-2">
          <Icon className={`h-3.5 w-3.5 ${color}`} />
          <span className={`text-xs font-bold ${color}`}>{label}</span>
          {active ? (
            <span className="text-[9px] px-1.5 py-0.5 rounded bg-emerald-500/20 text-emerald-300 font-bold">持仓中</span>
          ) : (
            <span className="text-[9px] text-white/25">未持有</span>
          )}
        </div>
        {active && entry > 0 && (
          <span className={`text-xs font-mono font-bold ${pnl >= 0 ? "text-emerald-400" : "text-red-400"}`}>
            {pnl >= 0 ? "+" : ""}${pnl.toFixed(2)}
          </span>
        )}
      </div>
      <div className="grid grid-cols-2 sm:grid-cols-4 gap-2 text-[11px]">
        <div>
          <p className="text-white/30 mb-0.5">入场价</p>
          <p className="font-mono text-white/80">{entry > 0 ? entry.toFixed(4) : "—"}</p>
        </div>
        <div>
          <p className="text-white/30 mb-0.5">现价</p>
          <p className="font-mono text-white/80">{live > 0 ? live.toFixed(4) : "—"}</p>
        </div>
        <div>
          <p className="text-white/30 mb-0.5">份额</p>
          <p className="font-mono text-white/80">{size > 0 ? size.toFixed(2) : "—"}</p>
        </div>
        <div>
          <p className="text-white/30 mb-0.5">成本</p>
          <p className="font-mono text-white/80">{active && cost > 0 ? `$${cost.toFixed(2)}` : "—"}</p>
        </div>
      </div>
    </div>
  );
}

function PositionCard({ pos, feeRate }: { pos: OpenPosition; feeRate: number }) {
  const isDh = pos.strategy === "DH";
  const isLih = pos.strategy === "LIH";
  const windowMin = pos.windowMinutes ?? 5;
  const yesActive = isDh || (pos.yesSize ?? 0) > 0;
  const noActive = isDh || (pos.noSize ?? 0) > 0;

  return (
    <div className="rounded-2xl border border-white/10 bg-white/[0.03] p-4 space-y-3">
      <div className="flex flex-wrap items-start justify-between gap-2">
        <div>
          <div className="flex items-center gap-2 mb-1">
            <span className="text-sm font-bold uppercase text-white">{pos.asset}</span>
            <span
              className={`px-1.5 py-0.5 rounded text-[10px] font-bold ${
                windowMin === 15 ? "bg-indigo-500/20 text-indigo-300" : "bg-purple-500/20 text-purple-300"
              }`}
            >
              {windowMin}m
            </span>
            {isDh && <span className="text-[10px] text-white/35">折价对冲</span>}
            {isLih && <span className="text-[10px] text-white/35">分腿对冲</span>}
          </div>
          <p className="text-[11px] text-white/40 max-w-lg leading-relaxed">{pos.question}</p>
        </div>
        <div className="text-right space-y-1">
          <div className="flex items-center gap-1.5 justify-end text-[11px] text-white/40">
            <Clock className="h-3 w-3" />
            <span>距结算</span>
            <Countdown endDateTs={pos.endDateTs} />
          </div>
          <p className={`text-sm font-mono font-bold ${pos.pnl >= 0 ? "text-emerald-400" : "text-red-400"}`}>
            浮动 {pos.pnl >= 0 ? "+" : ""}${pos.pnl.toFixed(2)}
          </p>
          {pos.entryFee !== undefined && pos.entryFee > 0 && (
            <p className="text-[10px] text-white/30 font-mono">入场手续费 ≈ ${pos.entryFee.toFixed(2)} ({(feeRate * 100).toFixed(1)}%)</p>
          )}
        </div>
      </div>

      <div className="grid gap-2 sm:grid-cols-2">
        <LegRow
          label="看涨 YES"
          icon={TrendingUp}
          entry={pos.yesEntryPrice ?? 0}
          live={pos.yesLivePrice ?? 0}
          size={pos.yesSize ?? 0}
          cost={pos.yesCost ?? 0}
          active={yesActive}
          color="text-emerald-400"
        />
        <LegRow
          label="看跌 NO"
          icon={TrendingDown}
          entry={pos.noEntryPrice ?? 0}
          live={pos.noLivePrice ?? 0}
          size={pos.noSize ?? 0}
          cost={pos.noCost ?? 0}
          active={noActive}
          color="text-rose-400"
        />
      </div>

      {(pos.yesLivePrice ?? 0) > 0 && (pos.noLivePrice ?? 0) > 0 && (
        <div className="flex flex-wrap gap-4 pt-1 text-[11px] font-mono text-white/40 border-t border-white/5">
          <span>
            双边合计 <span className="text-white/70">{((pos.yesLivePrice ?? 0) + (pos.noLivePrice ?? 0)).toFixed(4)}</span>
          </span>
          <span>
            总成本 <span className="text-white/70">${pos.cost.toFixed(2)}</span>
          </span>
          {isDh && (
            <span>
              锁定利润 <span className="text-emerald-400">${pos.pnl.toFixed(2)}</span>
            </span>
          )}
        </div>
      )}
    </div>
  );
}

export function TradingPanels({ liveState }: { liveState: LiveState }) {
  const lihMode = isLihPrimary(liveState);
  const strategyLabel = strategyShortLabel(liveState);
  const closedTrades = cumulativeClosedTrades(liveState);
  const strategyPnl = strategyRealizedPnl(liveState);

  return (
    <div className="grid gap-5">
      <GlassCard>
        <CardHeader>
          <div className="flex items-center justify-between flex-wrap gap-2">
            <CardTitle className="font-heading text-base font-semibold tracking-tight flex items-center gap-2">
              <Briefcase className="h-4 w-4 text-amber-400/70" />
              当前持仓
            </CardTitle>
            <span className="text-[11px] font-mono text-muted-foreground">
              {liveState.openPositions} 笔 · {strategyLabel} 已实现 ${strategyPnl.toFixed(2)} · 累计成交 {closedTrades} 笔 · 费率{" "}
              {(liveState.feeRate * 100).toFixed(1)}%
            </span>
          </div>
        </CardHeader>
        <CardContent>
          {liveState.positionList.length === 0 ? (
            <div className="py-12 text-center text-muted-foreground text-xs font-mono border border-dashed border-border rounded-xl">
              {lihMode
                ? "暂无持仓，检测到 LIH 便宜腿信号后自动开仓"
                : "暂无持仓，检测到 DH 信号后自动开仓"}
            </div>
          ) : (
            <div className="space-y-4">
              {liveState.positionList.map((pos, idx) => (
                <PositionCard key={`${pos.asset}-${pos.strategy}-${idx}`} pos={pos} feeRate={liveState.feeRate} />
              ))}
            </div>
          )}
        </CardContent>
      </GlassCard>

      <div className="grid gap-5 lg:grid-cols-2">
        <GlassCard>
          <CardHeader>
            <CardTitle className="font-heading text-sm font-semibold tracking-tight flex items-center gap-2">
              <Radio className="h-4 w-4 text-amber-400/70" />
              交易流水
            </CardTitle>
          </CardHeader>
            <CardContent>
            <FeedList
              lines={dedupeTradeTelemetry(
                liveState.telemetryLog.filter(
                  (l) =>
                    l.includes("PLACED") ||
                    l.includes("SETTLED") ||
                    l.includes("CLOSED") ||
                    l.includes("FILLED") ||
                    l.includes("OPENED") ||
                    l.includes("BALANCE") ||
                    l.includes("pending") ||
                    l.includes("awaiting")
                )
              )}
              emptyText="等待成交记录..."
            />
          </CardContent>
        </GlassCard>

        <GlassCard>
          <CardHeader>
            <CardTitle className="font-heading text-sm font-semibold tracking-tight flex items-center gap-2">
              <Zap className="h-4 w-4 text-amber-400/70" />
              {lihMode ? "LIH 策略信号" : "DH 策略信号"}
            </CardTitle>
          </CardHeader>
          <CardContent>
            <FeedList
              lines={liveState.signalLog}
              emptyText={lihMode ? "等待 LIH 策略信号..." : "等待 DH 策略信号..."}
            />
          </CardContent>
        </GlassCard>
      </div>
    </div>
  );
}
