"use client";

import { DashboardLayout } from "@/components/layouts/DashboardLayout";
import { PageContainer } from "@/components/shared/PageContainer";
import { PageHeader } from "@/components/shared/PageHeader";
import { GlassCard, CardContent, CardHeader, CardTitle } from "@/components/shared/GlassCard";
import { PmMarketPanel } from "@/components/dashboard/PmMarketPanel";
import { TradingPanels } from "@/components/dashboard/TradingPanels";
import { PreflightBanner } from "@/components/dashboard/PreflightBanner";
import { useLiveState } from "@/hooks/useLiveState";
import { coreStatusLabel } from "@/lib/coreStatus";
import {
  cumulativeClosedTrades,
  isLihPrimary,
  strategyRealizedPnl,
  strategyShortLabel,
} from "@/lib/strategyMode";
import { Activity, DollarSign, Briefcase, Percent, TrendingUp } from "lucide-react";
import { useState, useEffect } from "react";

export default function DashboardPage() {
  const liveState = useLiveState();
  const [mounted, setMounted] = useState(false);
  const lihMode = isLihPrimary(liveState);
  const strategyLabel = strategyShortLabel(liveState);
  const closedTrades = cumulativeClosedTrades(liveState);
  const strategyPnl = strategyRealizedPnl(liveState);

  useEffect(() => {
    setMounted(true);
  }, []);

  const isShadow = liveState.liveLihDryRun !== false;
  const cumulativePnl =
    (liveState.tradesBaselineTs ?? 0) > 0 ? strategyPnl : liveState.totalPnl;
  const pnlColor = cumulativePnl >= 0 ? "text-emerald-400" : "text-red-400";
  const coreStatus = coreStatusLabel(liveState.status, liveState.botStreamConnected);
  const hasLiveWallet =
    liveState.walletSource === "live" &&
    liveState.realWalletBalance != null &&
    liveState.realWalletBalance > 0;
  const displayBalance = liveState.realWalletBalance ?? liveState.balance;

  return (
    <DashboardLayout>
      <PageContainer className="space-y-5">
        <PageHeader
          title="仪表盘"
          description={`${strategyLabel} 分腿对冲 · 实时行情、持仓与成交流水。`}
          icon={Activity}
        />

        <PreflightBanner />

        {isShadow && (
          <div className="rounded-lg border border-amber-500/25 bg-amber-500/8 px-4 py-2.5 text-[13px] text-amber-100/90">
            📋 <strong>Shadow 模式</strong>（LIVE_LIH_DRY_RUN=true）— 验簿与信号，不向 CLOB 发单。费率边际已按
            {liveState.useDynamicFees ? " Polymarket V2 动态费率曲线" : ` 约 ${(liveState.feeRate * 100).toFixed(1)}% 扁平费率`}
            扣除。
          </div>
        )}

        <div className="grid gap-4 sm:grid-cols-2 xl:grid-cols-4">
          <GlassCard>
            <CardHeader className="flex flex-row items-center justify-between space-y-0 pb-2">
              <CardTitle className="text-xs font-medium uppercase tracking-wider text-muted-foreground">总余额</CardTitle>
              <DollarSign className="h-4 w-4 text-amber-400/50" />
            </CardHeader>
            <CardContent className="pb-4">
              <div className="text-2xl font-mono font-bold tracking-tight">${displayBalance.toFixed(2)}</div>
              {hasLiveWallet && liveState.cashBalance != null && liveState.cashBalance > 0 && (
                <p className="text-xs text-muted-foreground mt-1 font-mono">
                  现金 ${liveState.cashBalance.toFixed(2)}
                  {(liveState.positionsValue ?? 0) > 0 && (
                    <span> · 持仓市值 ${(liveState.positionsValue ?? 0).toFixed(2)}</span>
                  )}
                </p>
              )}
              <p className={`text-xs font-mono mt-2 ${pnlColor}`}>
                累计盈亏 {cumulativePnl >= 0 ? "+" : ""}${cumulativePnl.toFixed(2)}
                <span className="text-muted-foreground ml-2">
                  今日 {liveState.dailyPnl >= 0 ? "+" : ""}${liveState.dailyPnl.toFixed(2)}
                </span>
              </p>
            </CardContent>
          </GlassCard>

          <GlassCard>
            <CardHeader className="flex flex-row items-center justify-between space-y-0 pb-2">
              <CardTitle className="text-xs font-medium uppercase tracking-wider text-muted-foreground">持仓数量</CardTitle>
              <Briefcase className="h-4 w-4 text-amber-400/50" />
            </CardHeader>
            <CardContent className="pb-4">
              <div className="text-2xl font-mono font-bold tracking-tight">{liveState.openPositions}</div>
              <p className="text-xs text-muted-foreground mt-2">
                累计 {strategyLabel} 成交 {closedTrades} 笔
                {strategyPnl !== 0 && (
                  <span className="ml-2">
                    · 已实现 {strategyPnl >= 0 ? "+" : ""}${strategyPnl.toFixed(2)}
                  </span>
                )}
              </p>
            </CardContent>
          </GlassCard>

          <GlassCard>
            <CardHeader className="flex flex-row items-center justify-between space-y-0 pb-2">
              <CardTitle className="text-xs font-medium uppercase tracking-wider text-muted-foreground">胜率</CardTitle>
              <Percent className="h-4 w-4 text-amber-400/50" />
            </CardHeader>
            <CardContent className="pb-4">
              <div className="text-2xl font-mono font-bold tracking-tight">
                {(Math.min(liveState.winRate, 1) * 100).toFixed(1)}%
              </div>
              <p className="text-xs text-muted-foreground mt-2">历史平仓胜率</p>
            </CardContent>
          </GlassCard>

          <GlassCard>
            <CardHeader className="flex flex-row items-center justify-between space-y-0 pb-2">
              <CardTitle className="text-xs font-medium uppercase tracking-wider text-muted-foreground">核心状态</CardTitle>
              <TrendingUp className="h-4 w-4 text-amber-400/60" />
            </CardHeader>
            <CardContent className="pb-4">
              <div className="flex items-center gap-2">
                <span className="relative flex h-2 w-2">
                  {coreStatus.pulse && (
                    <span className="animate-ping absolute inline-flex h-full w-full rounded-full bg-amber-400 opacity-75" />
                  )}
                  <span
                    className={`relative inline-flex rounded-full h-2 w-2 ${
                      liveState.status === 0 ? "bg-emerald-400" : liveState.status === 2 ? "bg-red-400" : "bg-amber-400"
                    }`}
                  />
                </span>
                <span className={`text-xl font-mono font-bold ${coreStatus.color}`}>{coreStatus.text}</span>
              </div>
              {liveState.statusReason && (
                <p
                  className="text-[10px] text-muted-foreground mt-2 font-mono leading-relaxed line-clamp-2"
                  title={liveState.statusReason}
                >
                  {liveState.statusReason}
                </p>
              )}
              <p className="text-[10px] text-muted-foreground mt-2 font-mono">
                更新：{mounted ? new Date(liveState.timestamp || Date.now()).toLocaleTimeString("zh-CN") : "--:--:--"}
              </p>
            </CardContent>
          </GlassCard>
        </div>

        <div className="rounded-lg border border-violet-500/20 bg-violet-500/8 px-4 py-2.5 text-[13px] text-violet-100/90">
          <strong>LIH</strong> — trigger leg1 + 利润对冲（合价 ≤ {liveState.lihTargetCombined.toFixed(2)}），末段阶梯缩 gap。
        </div>

        <TradingPanels liveState={liveState} />

        <PmMarketPanel
          opportunities={liveState.dhOpportunities}
          dhSumTarget={liveState.dhSumTarget}
          dhMinDiscount={liveState.dhMinDiscount}
          feeRate={liveState.feeRate}
          timestamp={liveState.timestamp}
          marketsScanned={liveState.marketsScanned}
          lihEnabled={lihMode}
          lihTargetCombined={liveState.lihTargetCombined}
        />
      </PageContainer>
    </DashboardLayout>
  );
}
