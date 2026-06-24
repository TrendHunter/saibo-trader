#pragma once

#include "Signal.h"
#include "../state/StateStore.h"
#include "../risk/RiskManager.h"
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace trading {

struct LegInAction {
    enum class Kind { OpenLeg1, CompleteHedge } kind;
    MarketInfo market;
    bool buy_yes = false;
    double price = 0.0;
    double shares = 0.0;
    std::string lih_id;
    std::string note;
};

class LegInHedgeDetector {
public:
    LegInHedgeDetector(StateStore& store,
                       std::vector<MarketInfo> markets,
                       double leg1_max_price = 0.45,
                       double target_combined = 0.94,
                       double min_seconds_remaining = 15.0,
                       double leg1_min_seconds_remaining = 30.0,
                       double leg1_start_delay_sec = 5.0,
                       double leg1_cooldown_seconds = 20.0,
                       double rebalance_cooldown_seconds = 5.0,
                       bool use_mirror_prices = true,
                       double leg1_shares = 10.0,
                       bool allow_over_target = true,
                       double force_balance_secs = 60.0,
                       double max_rebalance_shares = 0.0,
                       bool flex_rebalance = false,
                       double flex_dilute_ratio = 0.95,
                       bool leg1_trend_align = false,
                       double trend_lookback_sec = 60.0,
                       bool leg1_trend_mode = false,
                       double leg1_trend_max_price = 0.65,
                       bool leg1_trigger_mode = false,
                       double leg1_trigger_min = 0.70,
                       double endgame_secs = 100.0,
                       double endgame_hold_ask = 0.90,
                       double endgame_resume_hedge_ask = 0.89,
                       double endgame_soft_cap = 1.15,
                       double endgame_step_small = 5.0,
                       double endgame_step_large = 10.0,
                       double endgame_gap_large = 10.0,
                       double endgame_override_secs = 50.0,
                       double endgame_override_cooldown = 2.0,
                       bool endgame_minimize_gap = true,
                       bool endgame_ladder_enabled = true,
                       double endgame_ladder_secs = 90.0,
                       double endgame_ladder_start = 0.95,
                       double endgame_ladder_end = 0.97,
                       double endgame_ladder_step = 0.01,
                       double max_entry_marginal = 1.15);

    std::optional<LegInAction> evaluate(double now_ms, risk::RiskManager& rm);

    void set_active_markets(std::vector<MarketInfo> markets) { markets_ = std::move(markets); }
    void set_leg1_max_price(double v) { leg1_max_price_ = v; }
    void set_target_combined(double v) { target_combined_ = v; }
    void set_leg1_cooldown_seconds(double v) { leg1_cooldown_seconds_ = v; }
    void set_rebalance_cooldown_seconds(double v) { rebalance_cooldown_seconds_ = v; }
    /** @deprecated Use set_leg1_cooldown_seconds (LIH_COOLDOWN_SECONDS alias). */
    void set_cooldown_seconds(double v) { leg1_cooldown_seconds_ = v; }
    void set_min_seconds_remaining(double v) { min_seconds_remaining_ = v; }
    void set_leg1_min_seconds_remaining(double v) { leg1_min_seconds_remaining_ = v; }
    void set_leg1_start_delay_sec(double v) { leg1_start_delay_sec_ = v; }
    void set_use_mirror_prices(bool v) { use_mirror_prices_ = v; }
    void set_leg1_shares(double v) { leg1_shares_ = v; }
    void set_allow_over_target(bool v) { allow_over_target_ = v; }
    void set_force_balance_secs(double v) { force_balance_secs_ = v; }
    void set_max_rebalance_shares(double v) { max_rebalance_shares_ = v; }
    void set_flex_rebalance(bool v) { flex_rebalance_ = v; }
    void set_flex_dilute_ratio(double v) { flex_dilute_ratio_ = v; }
    void set_leg1_trend_align(bool v) { leg1_trend_align_ = v; }
    void set_trend_lookback_sec(double v) { trend_lookback_sec_ = v; }
    void set_leg1_trend_mode(bool v) { leg1_trend_mode_ = v; leg1_trigger_mode_ = false; }
    void set_leg1_trend_max_price(double v) { leg1_trend_max_price_ = v; }
    void set_leg1_trigger_mode(bool v) { leg1_trigger_mode_ = v; if (v) leg1_trend_mode_ = false; }
    void set_leg1_trigger_min(double v) { leg1_trigger_min_ = v; }
    void set_endgame_secs(double v) { endgame_secs_ = v; }
    void set_endgame_hold_ask(double v) { endgame_hold_ask_ = v; }
    void set_endgame_resume_hedge_ask(double v) { endgame_resume_hedge_ask_ = v; }
    void set_endgame_soft_cap(double v) { endgame_soft_cap_ = v; }
    void set_endgame_override_secs(double v) { endgame_override_secs_ = v; }
    void set_endgame_minimize_gap(bool v) { endgame_minimize_gap_ = v; }
    void set_endgame_ladder_enabled(bool v) { endgame_ladder_enabled_ = v; }
    void set_max_entry_marginal(double v) { max_entry_marginal_ = v; }

private:
    bool leg1_trend_allows(const MarketInfo& market, bool pick_yes) const;
    /** Binance spot direction for endgame hold (always checked; independent of leg1_trend_align). */
    bool spot_trend_favors(const MarketInfo& market, bool pick_yes) const;
    struct Quote {
        double yes = 0.0;
        double no = 0.0;
        bool from_mirror = false;
    };

    double cap_shares_budget(double shares, double max_usdc, double unit_cost) const;
    double hedge_fill_shares(
        const std::string& token_id, double gap, double px,
        double max_usdc, double max_matched_shares) const;
    double paired_fill_shares(
        const MarketInfo& market, double yes_p, double no_p,
        double max_usdc, double max_matched_shares) const;
    Quote quote_for(const MarketInfo& market) const;
    /** Live hedge pricing: fresh REST + max(ws,rest) conservative ask. */
    Quote hedge_quote_for(const MarketInfo& market) const;
    double cap_shares(double shares, double balance, double unit_cost) const;
    void log_rebalance_status(const MarketInfo& market, const std::string& key, double now_sec,
                              const risk::LegInHedgePosition& pos, const Quote& q,
                              double yes_avg, double no_avg, double gap) const;
    void log_entry_status(const MarketInfo& market, const std::string& key, double now_sec,
                          const Quote& q, const char* reason) const;
    /** Time-ramped max marginal in endgame ladder window (e.g. 0.95 → 0.97 over 90s). */
    double endgame_ladder_max_marginal(double secs_left) const;

    StateStore& store_;
    std::vector<MarketInfo> markets_;
    double leg1_max_price_;
    double target_combined_;
    double min_seconds_remaining_;
    /** No new leg1 when secs_left below this — wait for next window. */
    double leg1_min_seconds_remaining_;
    /** No leg1 until this many seconds after window open (skip opening volatility). */
    double leg1_start_delay_sec_;
    double leg1_cooldown_seconds_;
    double rebalance_cooldown_seconds_;
    bool use_mirror_prices_;
    double leg1_shares_;
    bool allow_over_target_;
    double force_balance_secs_;
    double max_rebalance_shares_;
    bool flex_rebalance_;
    double flex_dilute_ratio_;
    bool leg1_trend_align_;
    double trend_lookback_sec_;
    bool leg1_trend_mode_;
    double leg1_trend_max_price_;
    /** Shadow-style: enter when either side ask >= trigger_min (pick higher ask). */
    bool leg1_trigger_mode_;
    double leg1_trigger_min_;
    double endgame_secs_;
    double endgame_hold_ask_;
    double endgame_resume_hedge_ask_;
    double endgame_soft_cap_;
    double endgame_step_small_;
    double endgame_step_large_;
    double endgame_gap_large_;
    double endgame_override_secs_;
    double endgame_override_cooldown_;
    /** When true, skip endgame hold — always try 5/10-step gap shrink in endgame. */
    bool endgame_minimize_gap_;
    bool endgame_ladder_enabled_;
    double endgame_ladder_secs_;
    double endgame_ladder_start_;
    double endgame_ladder_end_;
    double endgame_ladder_step_;
    /** Skip leg1 when leg1_ask + opposite_ask exceeds this (0 = disabled). */
    double max_entry_marginal_;
    mutable std::unordered_map<std::string, double> last_status_log_sec_;
    std::unordered_map<std::string, double> last_leg1_time_;
    std::unordered_map<std::string, double> last_rebalance_time_;
    mutable std::unordered_map<std::string, double> last_entry_log_sec_;
};

} // namespace trading
