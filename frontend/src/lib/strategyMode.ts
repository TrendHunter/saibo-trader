import type { LiveState } from "@/hooks/useLiveState";

function closedLihRounds(state: LiveState): number {
  const fromHistory = state.tradeHistory.filter((r) => r.strategy === "LIH" && r.status === "closed").length;
  return Math.max(state.totalLihTrades, fromHistory);
}

/** LIH-only stack; always treat as LIH for UI. */
export function isLihPrimary(_state: Pick<LiveState, "lihEnabled" | "strategy" | "tradeHistory">): boolean {
  return true;
}

export function strategyShortLabel(_state: Pick<LiveState, "lihEnabled" | "strategy" | "tradeHistory">): string {
  return "LIH";
}

export function cumulativeClosedTrades(state: LiveState): number {
  return closedLihRounds(state);
}

export function strategyRealizedPnl(state: LiveState): number {
  return state.lihPnl;
}
