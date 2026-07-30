#pragma once

#include "../signals/Signal.h"
#include "../signals/RegimeGate.h"
#include "../state/StateStore.h"
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace risk {
struct LegInHedgePosition;
}

namespace trading {

/** Per 5m window ledger for shadow/mm2 comparison (logs/shadow_windows.jsonl). */
class ShadowWindowRecorder {
public:
    void set_enabled(bool v) { enabled_ = v; }
    bool enabled() const { return enabled_; }
    void set_store(StateStore* store) { store_ = store; }
    void set_asset_filter(std::string asset, int window_minutes);

    void tick(const MarketInfo& market, double now_sec, double yes_ask, double no_ask) const;
    void record_skip(
        const MarketInfo& market, double now_sec, const char* reason,
        double yes_ask, double no_ask) const;
    void on_leg1(
        const risk::LegInHedgePosition& pos, bool buy_yes, double price, double shares,
        double now_sec);
    void on_hedge_fill(const risk::LegInHedgePosition& pos);
    void on_closed(const risk::LegInHedgePosition& pos, double pnl_usdc);

    /** Finalize any windows whose end_ts has passed. */
    void flush_expired(double now_sec) const;

    void update_regime(
        const MarketInfo& market, double now_sec, const RegimeDecision& decision) const;

private:
    struct SnapFields {
        bool taken = false;
        double yes_ask = 0.0;
        double no_ask = 0.0;
        double yes_depth = 0.0;
        double no_depth = 0.0;
        double btc_spot = 0.0;
        double spot_bps = 0.0;
        std::string expensive_side;
        double expensive_ask = 0.0;
    };

    struct WindowState {
        int64_t window_start_ts = 0;
        int64_t window_end_ts = 0;
        std::string asset;
        int window_minutes = 5;
        bool traded = false;
        std::string skip_reason = "no_attempt";
        std::string last_skip_reason;
        std::string leg1_side;
        double leg1_px = 0.0;
        double leg1_shares = 0.0;
        double leg1_sec_in = 0.0;
        int total_fills = 0;
        double total_round_pnl = 0.0;
        double final_gap = 0.0;
        std::string winning_side;
        double spot_open = 0.0;
        std::string regime_state = "OK";
        std::string regime_reason;
        double regime_score_b = 0.0;
        double regime_score_c = 0.0;
        std::string regime_pre_reason;
        static constexpr int kSnapCount = 4;
        static constexpr std::array<int, kSnapCount> kSnapOffsets{{120, 180, 240, 270}};
        std::array<SnapFields, kSnapCount> snaps{};
    };

    bool matches_filter(const MarketInfo& market) const;
    std::string slot_key(const MarketInfo& market) const;
    int64_t window_start_ts_for(const MarketInfo& market) const;
    bool window_finalized(int64_t start) const;
    WindowState& ensure_window(const MarketInfo& market, double now_sec) const;
    void maybe_snapshots(WindowState& w, const MarketInfo& market, double now_sec,
                         double yes_ask, double no_ask) const;
    void fill_snap(SnapFields& snap, const MarketInfo& market, double elapsed_sec,
                   double yes_ask, double no_ask) const;
    std::optional<double> spot_ret_bps(
        const MarketInfo& market, double window_total_sec, double elapsed_sec) const;
    static std::string normalize_skip_reason(const char* raw);
    static std::string side_label(bool buy_yes);
    static std::string iso_utc(int64_t ts);
    void finalize(WindowState& w) const;
    void write_row(const WindowState& w) const;

    bool enabled_ = false;
    StateStore* store_ = nullptr;
    std::string asset_filter_ = "btc";
    int window_minutes_filter_ = 5;
    mutable std::unordered_map<std::string, WindowState> active_;
    /** Each window_start_ts written at most once (prevents post-settle tick spam). */
    mutable std::unordered_set<int64_t> finalized_ts_;
};

} // namespace trading
