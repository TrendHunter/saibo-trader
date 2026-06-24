"use client";

import { DashboardLayout } from "@/components/layouts/DashboardLayout";
import { PageContainer } from "@/components/shared/PageContainer";
import { PageHeader } from "@/components/shared/PageHeader";
import {
  GlassCard,
  CardContent,
  CardHeader,
  CardTitle,
  CardDescription,
} from "@/components/shared/GlassCard";
import { Switch } from "@/components/ui/switch";
import { Label } from "@/components/ui/label";
import { Input } from "@/components/ui/input";
import { Button } from "@/components/ui/button";
import { SlidersHorizontal } from "lucide-react";
import { useCallback, useEffect, useState } from "react";
import { useLiveState } from "@/hooks/useLiveState";

type TradingMode = "stopped" | "shadow" | "live";

function deriveTradingMode(live: ReturnType<typeof useLiveState>): TradingMode {
  if (!live.botStreamConnected || live.status !== 0) return "stopped";
  if (live.liveLihDryRun === false) return "live";
  return "shadow";
}

const TRADING_MODE_LABEL: Record<TradingMode, string> = {
  stopped: "停止",
  shadow: "Shadow 运行",
  live: "实盘运行",
};

const ASSET_KEYS_5M = {
  BTC: "DH_ENABLE_5M_BTC",
  ETH: "DH_ENABLE_5M_ETH",
  SOL: "DH_ENABLE_5M_SOL",
} as const;

const ASSET_KEYS_15M = {
  BTC: "DH_ENABLE_15M_BTC",
  ETH: "DH_ENABLE_15M_ETH",
} as const;

function AssetToggleRow({
  label,
  checked,
  disabled,
  onChange,
}: {
  label: string;
  checked: boolean;
  disabled: boolean;
  onChange: (enabled: boolean) => void;
}) {
  return (
    <div className="flex items-center justify-between py-2 pl-3 border-l border-white/10">
      <span className="text-[13px] font-mono text-white/75">{label}</span>
      <Switch checked={checked} disabled={disabled} onCheckedChange={onChange} />
    </div>
  );
}

function MarketTogglesSection({
  live,
  controlsDisabled,
  onPatch,
}: {
  live: ReturnType<typeof useLiveState>;
  controlsDisabled: boolean;
  onPatch: (patch: Record<string, string>, okMessage: string) => Promise<void>;
}) {
  const toggleWindow = (window: "5m" | "15m", enabled: boolean) =>
    onPatch(
      { [window === "5m" ? "DH_ENABLE_5M" : "DH_ENABLE_15M"]: enabled ? "true" : "false" },
      `${window} 窗口已${enabled ? "开启" : "关闭"}`
    );

  const toggleAsset = (envKey: string, label: string, enabled: boolean) =>
    onPatch({ [envKey]: enabled ? "true" : "false" }, `${label} 已${enabled ? "开启" : "关闭"}`);

  return (
    <div className="rounded-xl border border-white/10 bg-white/[0.03] p-4 space-y-4">
      <h4 className="text-[11px] font-medium tracking-widest uppercase text-white/40">市场开关</h4>
      <p className="text-[12px] text-white/40 leading-relaxed">
        控制 LIH 扫描哪些 5m/15m 市场（env：<code className="text-white/50">DH_ENABLE_*</code>）。
      </p>

      <div className="space-y-3">
        <div className="flex items-center justify-between py-1">
          <Label htmlFor="mkt-5m" className="flex flex-col space-y-1">
            <span className="font-semibold text-white/90 text-[14px]">5 分钟窗口</span>
            <span className="font-normal text-white/40 text-[12px]">总开关</span>
          </Label>
          <Switch
            id="mkt-5m"
            checked={live.dhEnable5m}
            disabled={controlsDisabled}
            onCheckedChange={(checked) => toggleWindow("5m", checked)}
          />
        </div>
        <div className={`space-y-1 ${!live.dhEnable5m ? "opacity-40 pointer-events-none" : ""}`}>
          <AssetToggleRow
            label="BTC"
            checked={live.dhEnable5mBtc}
            disabled={controlsDisabled || !live.dhEnable5m}
            onChange={(checked) => toggleAsset(ASSET_KEYS_5M.BTC, "5m BTC", checked)}
          />
          <AssetToggleRow
            label="ETH"
            checked={live.dhEnable5mEth}
            disabled={controlsDisabled || !live.dhEnable5m}
            onChange={(checked) => toggleAsset(ASSET_KEYS_5M.ETH, "5m ETH", checked)}
          />
          <AssetToggleRow
            label="SOL"
            checked={live.dhEnable5mSol}
            disabled={controlsDisabled || !live.dhEnable5m}
            onChange={(checked) => toggleAsset(ASSET_KEYS_5M.SOL, "5m SOL", checked)}
          />
        </div>
      </div>

      <div className="space-y-3 border-t border-white/5 pt-4">
        <div className="flex items-center justify-between py-1">
          <Label htmlFor="mkt-15m" className="flex flex-col space-y-1">
            <span className="font-semibold text-white/90 text-[14px]">15 分钟窗口</span>
            <span className="font-normal text-white/40 text-[12px]">总开关</span>
          </Label>
          <Switch
            id="mkt-15m"
            checked={live.dhEnable15m}
            disabled={controlsDisabled}
            onCheckedChange={(checked) => toggleWindow("15m", checked)}
          />
        </div>
        <div className={`space-y-1 ${!live.dhEnable15m ? "opacity-40 pointer-events-none" : ""}`}>
          <AssetToggleRow
            label="BTC"
            checked={live.dhEnable15mBtc}
            disabled={controlsDisabled || !live.dhEnable15m}
            onChange={(checked) => toggleAsset(ASSET_KEYS_15M.BTC, "15m BTC", checked)}
          />
          <AssetToggleRow
            label="ETH"
            checked={live.dhEnable15mEth}
            disabled={controlsDisabled || !live.dhEnable15m}
            onChange={(checked) => toggleAsset(ASSET_KEYS_15M.ETH, "15m ETH", checked)}
          />
        </div>
      </div>

      <div className="bg-white/5 p-3 rounded-lg border border-white/10 text-[12px] font-mono text-white/45">
        当前扫描 {live.marketsScanned} 个市场 · 5m{" "}
        {[live.dhEnable5mBtc && "BTC", live.dhEnable5mEth && "ETH", live.dhEnable5mSol && "SOL"]
          .filter(Boolean)
          .join(" · ") || "全关"}{" "}
        · 15m {[live.dhEnable15mBtc && "BTC", live.dhEnable15mEth && "ETH"].filter(Boolean).join(" · ") || "全关"}
      </div>
    </div>
  );
}

export default function StrategiesPage() {
  const live = useLiveState();
  const [loading, setLoading] = useState(false);
  const [message, setMessage] = useState("");

  const [leg1Mode, setLeg1Mode] = useState("trigger");
  const [leg1TriggerMin, setLeg1TriggerMin] = useState("0.70");
  const [targetCombined, setTargetCombined] = useState("0.94");
  const [leg1Shares, setLeg1Shares] = useState("20");
  const [forceBalanceSecs, setForceBalanceSecs] = useState("0");
  const [maxMatched, setMaxMatched] = useState("50");
  const [maxRebalance, setMaxRebalance] = useState("0");
  const [lihMinRemaining, setLihMinRemaining] = useState("15");
  const [lihLeg1Cooldown, setLihLeg1Cooldown] = useState("20");
  const [lihRebalanceCooldown, setLihRebalanceCooldown] = useState("5");

  const tradingMode = deriveTradingMode(live);
  const controlsDisabled = loading || live.status === 2 || !live.botStreamConnected;

  const loadEnvConfig = useCallback(async () => {
    try {
      const res = await fetch("/api/bot/config");
      if (!res.ok) return;
      const data = (await res.json()) as { config?: Record<string, string> };
      const cfg = data.config ?? {};
      if (cfg.LIH_LEG1_MODE) setLeg1Mode(cfg.LIH_LEG1_MODE);
      if (cfg.LIH_LEG1_TRIGGER_MIN) setLeg1TriggerMin(cfg.LIH_LEG1_TRIGGER_MIN);
      if (cfg.LIH_TARGET_COMBINED) setTargetCombined(cfg.LIH_TARGET_COMBINED);
      if (cfg.LIH_LEG1_SHARES) setLeg1Shares(cfg.LIH_LEG1_SHARES);
      if (cfg.LIH_FORCE_BALANCE_SECS) setForceBalanceSecs(cfg.LIH_FORCE_BALANCE_SECS);
      if (cfg.LIH_MAX_MATCHED_SHARES) setMaxMatched(cfg.LIH_MAX_MATCHED_SHARES);
      if (cfg.LIH_MAX_REBALANCE_SHARES) setMaxRebalance(cfg.LIH_MAX_REBALANCE_SHARES);
      if (cfg.LIH_MIN_SECONDS_REMAINING) setLihMinRemaining(cfg.LIH_MIN_SECONDS_REMAINING);
      if (cfg.LIH_LEG1_COOLDOWN_SECONDS) setLihLeg1Cooldown(cfg.LIH_LEG1_COOLDOWN_SECONDS);
      else if (cfg.LIH_COOLDOWN_SECONDS) setLihLeg1Cooldown(cfg.LIH_COOLDOWN_SECONDS);
      if (cfg.LIH_REBALANCE_COOLDOWN_SECONDS) setLihRebalanceCooldown(cfg.LIH_REBALANCE_COOLDOWN_SECONDS);
    } catch {
      /* WS fallback below */
    }
  }, []);

  useEffect(() => {
    setTargetCombined(live.lihTargetCombined.toFixed(2));
    loadEnvConfig();
  }, [live.lihTargetCombined, loadEnvConfig]);

  const patchConfig = async (patch: Record<string, string>, okMessage: string) => {
    setLoading(true);
    setMessage("");
    try {
      const res = await fetch("/api/bot/config", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ patch }),
      });
      const data = await res.json();
      if (!res.ok) throw new Error(data.error || "操作失败");
      setMessage(okMessage);
    } catch (err) {
      setMessage(err instanceof Error ? err.message : "操作失败");
    } finally {
      setLoading(false);
    }
  };

  const applyTradingMode = async (mode: TradingMode) => {
    if (mode === tradingMode) return;
    if (mode === "live") {
      const ok = window.confirm(
        "确认开启实盘运行？\n\nBot 将向 Polymarket CLOB 发送真实订单并动用钱包资金。"
      );
      if (!ok) return;
    }
    setLoading(true);
    setMessage("");
    try {
      const patch: Record<string, string> = {};
      if (mode === "shadow") {
        patch.LIVE_LIH_DRY_RUN = "true";
      } else if (mode === "live") {
        patch.LIVE_LIH_DRY_RUN = "false";
      }
      if ((mode === "shadow" || mode === "live") && live.riskMaxConcurrentPositions <= 0) {
        const cfgRes = await fetch("/api/bot/config");
        const cfgData = cfgRes.ok
          ? ((await cfgRes.json()) as { config?: Record<string, string> })
          : { config: {} };
        const envMax = parseInt(cfgData.config?.RISK_MAX_CONCURRENT_POSITIONS ?? "0", 10);
        const restore = envMax > 0 ? envMax : 1;
        patch.RISK_MAX_CONCURRENT_POSITIONS = String(restore);
      }

      let body: Record<string, unknown>;
      if (mode === "stopped") {
        body = { action: "pause", reason: "Web: 停止新开仓" };
      } else if (mode === "shadow") {
        body = { patch, action: "resume", reason: "Web: Shadow 运行" };
      } else {
        body = { patch, action: "resume", reason: "Web: 实盘运行" };
      }
      const res = await fetch("/api/bot/config", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(body),
      });
      const data = await res.json();
      if (!res.ok) throw new Error(data.error || "操作失败");
      setMessage(`已切换为：${TRADING_MODE_LABEL[mode]}`);
    } catch (err) {
      setMessage(err instanceof Error ? err.message : "操作失败");
    } finally {
      setLoading(false);
    }
  };

  const saveLihParams = () =>
    patchConfig(
      {
        LIH_LEG1_MODE: leg1Mode,
        LIH_LEG1_TRIGGER_MIN: leg1TriggerMin,
        LIH_TARGET_COMBINED: targetCombined,
        LIH_LEG1_SHARES: leg1Shares,
        LIH_FORCE_BALANCE_SECS: forceBalanceSecs,
        LIH_MAX_MATCHED_SHARES: maxMatched,
        LIH_MAX_REBALANCE_SHARES: maxRebalance,
        LIH_MIN_SECONDS_REMAINING: lihMinRemaining,
        LIH_LEG1_COOLDOWN_SECONDS: lihLeg1Cooldown,
        LIH_REBALANCE_COOLDOWN_SECONDS: lihRebalanceCooldown,
      },
      "LIH 参数已保存并热更新"
    );

  return (
    <DashboardLayout>
      <PageContainer>
        <PageHeader
          title="策略配置"
          description="LIH 分腿对冲（trigger 模式）：Leg1 → 利润对冲 → 末段阶梯缩 gap。保存后写入 .env 并热更新。"
          icon={SlidersHorizontal}
        />

        <div className="mb-4 rounded-xl border border-emerald-500/20 bg-emerald-500/5 px-4 py-2.5 text-[13px] text-emerald-200/90">
          主策略：<span className="font-mono font-bold">LIH</span>
          {" · "}
          当前：<span className="font-mono font-bold">{TRADING_MODE_LABEL[tradingMode]}</span>
          {live.statusReason && live.status !== 0 && (
            <span className="ml-2 text-white/50">（{live.statusReason}）</span>
          )}
          {!live.botStreamConnected && <span className="ml-2 text-amber-200/90">Bot 未连接</span>}
        </div>

        <GlassCard className="mb-5">
          <CardHeader className="pb-3">
            <CardTitle className="text-base font-semibold text-white/90">交易运行模式</CardTitle>
            <CardDescription className="text-white/40 text-[13px]">
              停止 / Shadow（验簿不发单）/ 实盘。由 <code className="text-white/50">LIVE_LIH_DRY_RUN</code> 与 pause 控制。
            </CardDescription>
          </CardHeader>
          <CardContent>
            <div className="flex flex-wrap gap-2">
              {(["stopped", "shadow", "live"] as TradingMode[]).map((mode) => (
                <Button
                  key={mode}
                  type="button"
                  variant={tradingMode === mode ? "default" : "outline"}
                  disabled={controlsDisabled}
                  className={
                    tradingMode === mode
                      ? mode === "live"
                        ? "bg-emerald-600 hover:bg-emerald-500"
                        : mode === "shadow"
                          ? "bg-amber-600 hover:bg-amber-500"
                          : ""
                      : "border-white/15 bg-white/5 text-white/80"
                  }
                  onClick={() => void applyTradingMode(mode)}
                >
                  {TRADING_MODE_LABEL[mode]}
                </Button>
              ))}
            </div>
          </CardContent>
        </GlassCard>

        <GlassCard>
          <CardHeader>
            <CardTitle className="font-heading text-lg font-semibold tracking-tight text-gradient">
              LIH 参数
            </CardTitle>
          </CardHeader>
          <CardContent className="space-y-5">
            <div className="flex items-center justify-between py-1">
              <Label className="text-white/90 font-medium text-[14px]">Mirror 价 (LIH_USE_MIRROR)</Label>
              <Switch
                checked={live.lihUseMirror}
                disabled={controlsDisabled}
                onCheckedChange={(checked) =>
                  patchConfig({ LIH_USE_MIRROR: checked ? "true" : "false" }, "Mirror 设置已更新")
                }
              />
            </div>

            <MarketTogglesSection live={live} controlsDisabled={controlsDisabled} onPatch={patchConfig} />

            <div className="grid gap-4 sm:grid-cols-2">
              <div className="space-y-2">
                <Label className="text-white/60 text-[12px]">Leg1 模式 (LIH_LEG1_MODE)</Label>
                <Input
                  value={leg1Mode}
                  onChange={(e) => setLeg1Mode(e.target.value)}
                  className="font-mono bg-white/5 border-white/10"
                  placeholder="trigger"
                />
              </div>
              <div className="space-y-2">
                <Label className="text-white/60 text-[12px]">Trigger 下限 (LIH_LEG1_TRIGGER_MIN)</Label>
                <Input
                  value={leg1TriggerMin}
                  onChange={(e) => setLeg1TriggerMin(e.target.value)}
                  className="font-mono bg-white/5 border-white/10"
                />
              </div>
              <div className="space-y-2">
                <Label className="text-white/60 text-[12px]">目标合价 (LIH_TARGET_COMBINED)</Label>
                <Input
                  value={targetCombined}
                  onChange={(e) => setTargetCombined(e.target.value)}
                  className="font-mono bg-white/5 border-white/10"
                />
              </div>
              <div className="space-y-2">
                <Label className="text-white/60 text-[12px]">Leg1 份额 (LIH_LEG1_SHARES)</Label>
                <Input
                  value={leg1Shares}
                  onChange={(e) => setLeg1Shares(e.target.value)}
                  className="font-mono bg-white/5 border-white/10"
                />
              </div>
              <div className="space-y-2">
                <Label className="text-white/60 text-[12px]">最大 matched (LIH_MAX_MATCHED_SHARES)</Label>
                <Input
                  value={maxMatched}
                  onChange={(e) => setMaxMatched(e.target.value)}
                  className="font-mono bg-white/5 border-white/10"
                />
              </div>
              <div className="space-y-2">
                <Label className="text-white/60 text-[12px]">Rebalance 上限 (0=预算)</Label>
                <Input
                  value={maxRebalance}
                  onChange={(e) => setMaxRebalance(e.target.value)}
                  className="font-mono bg-white/5 border-white/10"
                />
              </div>
              <div className="space-y-2">
                <Label className="text-white/60 text-[12px]">窗口剩余秒下限</Label>
                <Input
                  value={lihMinRemaining}
                  onChange={(e) => setLihMinRemaining(e.target.value)}
                  className="font-mono bg-white/5 border-white/10"
                />
              </div>
              <div className="space-y-2">
                <Label className="text-white/60 text-[12px]">Leg1 冷却 (秒)</Label>
                <Input
                  value={lihLeg1Cooldown}
                  onChange={(e) => setLihLeg1Cooldown(e.target.value)}
                  className="font-mono bg-white/5 border-white/10"
                />
              </div>
            </div>

            <div className="flex items-center justify-between pt-2">
              {message && <p className="text-[13px] text-amber-200/90">{message}</p>}
              <Button
                onClick={saveLihParams}
                disabled={loading || controlsDisabled}
                size="lg"
                variant="glass"
                className="ml-auto px-8 font-extrabold tracking-tight rounded-2xl"
              >
                {loading ? "保存中..." : "保存 LIH 参数"}
              </Button>
            </div>
          </CardContent>
        </GlassCard>
      </PageContainer>
    </DashboardLayout>
  );
}
