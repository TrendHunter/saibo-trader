export type TradeLogKind = "open" | "close" | "signal" | "scan" | "other";

export function classifyTradeLog(line: string): { color: string; label: string; kind: TradeLogKind } {
  const upper = line.toUpperCase();
  if (upper.includes("PLACED") || upper.includes("FILLED") || upper.includes("OPENED")) {
    return { color: "text-sky-400", label: "开仓", kind: "open" };
  }
  if (upper.includes("CLOSED") || upper.includes("SETTLED") || upper.includes("EARLY EXIT")) {
    return { color: "text-emerald-400", label: "平仓", kind: "close" };
  }
  if (upper.includes("SIGNAL")) {
    return { color: "text-amber-400", label: "信号", kind: "signal" };
  }
  if (upper.includes("MARKETS REFRESHED")) {
    return { color: "text-white/30", label: "扫描", kind: "scan" };
  }
  return { color: "text-white/60", label: "事件", kind: "other" };
}

/** Bot telemetry lines relevant to trade history (excludes market refresh noise). */
export function filterTradeHistory(lines: string[]): string[] {
  return lines.filter((line) => {
    const upper = line.toUpperCase();
    if (upper.includes("MARKETS REFRESHED") || upper.includes("BALANCE SYNCED")) return false;
    return (
      upper.includes("PLACED") ||
      upper.includes("SETTLED") ||
      upper.includes("EARLY EXIT") ||
      upper.includes("FILLED")
    );
  });
}

export interface ParsedTradeRow {
  kind: TradeLogKind;
  label: string;
  color: string;
  summary: string;
  detail: string;
}

export function parseTradeRow(line: string): ParsedTradeRow {
  const { color, label, kind } = classifyTradeLog(line);

  const settled = line.match(/^SETTLED\s+(\w+)\s+(WIN|LOSS)\s+@\s+([\d.]+)\s+\|\s+(.+)$/i);
  if (settled) {
    return {
      kind,
      label,
      color,
      summary: `${settled[1].toUpperCase()} · ${settled[2]}`,
      detail: settled[4],
    };
  }

  const placed = line.match(/^\[(LA|DH|LIH)\]\s+PLACED\s+(.+)$/i);
  if (placed) {
    return {
      kind,
      label,
      color,
      summary: `${placed[1]} · ${placed[2].split("@")[0].trim()}`,
      detail: placed[2],
    };
  }

  const early = line.match(/^LA EARLY EXIT\s+(.+)$/i);
  if (early) {
    return {
      kind,
      label,
      color,
      summary: "LA · 提前平仓",
      detail: early[1],
    };
  }

  return { kind, label, color, summary: label, detail: line };
}

/** Drop duplicate CLOSED/LEG1 per round; keep distinct HEDGE legs. */
export function dedupeTradeTelemetry(lines: string[]): string[] {
  const seen = new Set<string>();
  const out: string[] = [];
  for (const line of lines) {
    const idMatch = line.match(/LIH-([a-z]+)-(\d+)(?:-recon)?/i);
    let key = line.replace(/\s+/g, " ").trim();
    if (idMatch) {
      const baseId = `LIH-${idMatch[1]}-${idMatch[2]}`;
      const upper = line.toUpperCase();
      if (upper.includes("CLOSED") || upper.includes("SETTLE")) {
        key = `CLOSED:${baseId}`;
      } else if (upper.includes("LEG1")) {
        key = `LEG1:${baseId}`;
      } else if (upper.includes("HEDGE")) {
        const sh = line.match(/\+(\d+(?:\.\d+)?)\s*sh/i);
        key = `HEDGE:${baseId}:${sh?.[1] ?? line}`;
      }
    }
    if (seen.has(key)) continue;
    seen.add(key);
    out.push(line);
  }
  return out;
}
