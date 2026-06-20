import type { TradeRecord } from "@/hooks/useLiveState";

export interface ClobTradeFill {
  id: string;
  orderID?: string;
  tokenID?: string;
  side: string;
  size: number;
  price: number;
  usdcSize: number;
  timestamp: number;
  outcome?: string;
  title: string;
  asset: string;
  windowMinutes?: number | null;
  transactionHash?: string;
}

/** Polymarket activity timestamps are seconds; bot state uses seconds too. */
function normalizeTs(ts: number): number {
  if (!ts || ts <= 0) return 0;
  return ts > 1e12 ? Math.floor(ts / 1000) : ts;
}

export function clobFillToTradeRecord(fill: ClobTradeFill): TradeRecord {
  const ts = normalizeTs(fill.timestamp);
  const cost = fill.usdcSize > 0 ? fill.usdcSize : fill.price * fill.size;
  const side = fill.side || "BUY";
  const outcome = fill.outcome ? ` ${fill.outcome}` : "";
  return {
    id: fill.id || fill.orderID || fill.transactionHash || `clob-${ts}`,
    strategy: "LIH",
    asset: fill.asset || "—",
    status: "closed",
    market: fill.title || "Polymarket",
    side: `${side}${outcome}`.trim(),
    direction: "CLOB",
    entryPrice: fill.price,
    exitPrice: fill.price,
    size: fill.size,
    costUsdc: cost,
    entryFee: 0,
    exitFee: 0,
    pnlUsdc: 0,
    openedAt: ts,
    closedAt: ts,
    exitReason: "CLOB_FILL",
    isPaperMode: false,
    windowMinutes: fill.windowMinutes ?? undefined,
  };
}

export function mergeTradeHistory(
  botRecords: TradeRecord[],
  clobFills: ClobTradeFill[],
  baselineTs = 0
): TradeRecord[] {
  const afterBaseline = (ts: number) => baselineTs <= 0 || ts <= 0 || ts >= baselineTs;
  const filteredBot = botRecords.filter((r) => afterBaseline(r.closedAt || r.openedAt || 0));
  // Bot already has LIH round records — raw CLOB leg fills are redundant (+$0 CLOB_FILL noise).
  const hasLihClosed = filteredBot.some(
    (r) => r.strategy === "LIH" && r.status === "closed" && r.exitReason !== "CLOB_FILL"
  );
  if (hasLihClosed) {
    const seen = new Set<string>();
    const sorted = [...filteredBot].filter((r) => {
      if (r.exitReason === "CLOB_FILL") return false;
      if (r.strategy === "LIH" && r.status === "closed") {
        const m = r.id.match(/^LIH-([a-z]+)-(\d+)/i);
        const key = m ? `LIH-${m[1]}-${m[2]}` : r.id;
        if (seen.has(key)) return false;
        seen.add(key);
      }
      return true;
    });
    sorted.sort((a, b) => {
      const ta = a.closedAt || a.openedAt || 0;
      const tb = b.closedAt || b.openedAt || 0;
      return tb - ta;
    });
    return sorted;
  }
  const byId = new Map<string, TradeRecord>();
  for (const r of filteredBot) {
    byId.set(r.id, r);
  }
  for (const fill of clobFills) {
    const rec = clobFillToTradeRecord(fill);
    if (!afterBaseline(rec.closedAt || rec.openedAt || 0)) continue;
    if (byId.has(rec.id)) continue;
    byId.set(rec.id, rec);
  }
  const merged = Array.from(byId.values());
  merged.sort((a, b) => {
    const ta = a.closedAt || a.openedAt || 0;
    const tb = b.closedAt || b.openedAt || 0;
    return tb - ta;
  });
  return merged;
}
