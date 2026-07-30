#pragma once
#include <string>
#include <string_view>
#include <unordered_map>
#include <shared_mutex>
#include <optional>
#include <deque>
#include <vector>
#include <atomic>
#include "../signals/Signal.h"

#include "../risk/RiskManager.h"

namespace trading {

struct PriceTick {
    double price;
    double timestamp_ms;
    double volume;
    double received_at;
};

struct TokenPrice {
    double price;
    std::string side;
    double ts;
};

class StateStore {
public:
    void set_risk_manager(risk::RiskManager* rm) { risk_manager_ = rm; }
    void set_paper_mode(bool pm) { paper_mode_ = pm; }
    bool is_paper_mode() const { return paper_mode_; }
    void set_fee_rate(double rate) { fee_rate_ = rate; }
    void set_strategy(std::string s) { strategy_ = std::move(s); }
    void set_dh_window_enabled(bool enable_5m, bool enable_15m) {
        dh_enable_5m_ = enable_5m;
        dh_enable_15m_ = enable_15m;
    }
    bool dh_enable_5m() const { return dh_enable_5m_; }
    bool dh_enable_15m() const { return dh_enable_15m_; }
    void set_dh_asset_enabled(int window_minutes, const std::string& asset, bool enabled);
    bool dh_asset_enabled(int window_minutes, const std::string& asset) const;
    void set_binance_feed_enabled(bool enabled) { binance_feed_enabled_ = enabled; }

    struct TokenFeeParams {
        double rate = 0.0;
        double exponent = 0.0;
        bool from_api = false;
    };
    void set_token_fee_params(const std::string& token_id, double rate, double exponent);
    TokenFeeParams get_token_fee_params(std::string_view token_id) const;

    void update_btc_price(const PriceTick& tick);
    std::optional<PriceTick> get_latest_btc_price() const;
    // Note: use get_price_at("btc", seconds_ago) for historical BTC lookups.

    void update_eth_price(const PriceTick& tick);
    std::optional<PriceTick> get_latest_eth_price() const;

    void update_sol_price(const PriceTick& tick);
    std::optional<PriceTick> get_latest_sol_price() const;

    void update_token_price(std::string_view token_id, const TokenPrice& price);
    void update_token_bid(std::string_view token_id, const TokenPrice& price);
    void update_ws_book_ask(std::string_view token_id, const TokenPrice& price);
    void update_rest_book_ask(std::string_view token_id, double price, double depth_shares = 0.0);
    void update_rest_book_bid(std::string_view token_id, double price);
    std::optional<TokenPrice> get_token_bid(std::string_view token_id) const;
    std::optional<TokenPrice> get_token_price(std::string_view token_id) const;
    // Official CLOB mark (best bid) and buy (best ask) — REST preferred, WS fallback.
    std::optional<double> get_official_mark_bid(
        std::string_view token_id, double rest_max_age_sec = 20.0) const;
    std::optional<double> get_official_buy_ask(
        std::string_view token_id, double rest_max_age_sec = 20.0) const;

    struct DetectionAsk {
        double conservative_ask = 0.0;
        double ws_book_ask = 0.0;
        double rest_book_ask = 0.0;
        double rest_depth_shares = 0.0;
        bool ws_ok = false;
        bool rest_ok = false;
    };
    void set_book_aware_detect(bool v) { book_aware_detect_ = v; }
    bool book_aware_detect() const { return book_aware_detect_; }
    void set_paper_official_book(bool v) { paper_official_book_ = v; }
    bool paper_official_book() const { return paper_official_book_; }
    void set_paper_depth_sim(bool v) { paper_depth_sim_ = v; }
    bool paper_depth_sim() const { return paper_depth_sim_; }
    void set_paper_slippage_pct(double v) { paper_slippage_pct_ = v; }
    double paper_slippage_pct() const { return paper_slippage_pct_; }
    void set_paper_realism_enabled(bool v) { paper_realism_enabled_ = v; }
    bool paper_realism_enabled() const { return paper_realism_enabled_; }
    void set_paper_liquidity_take_ratio(double v) { paper_liquidity_take_ratio_ = v; }
    double paper_liquidity_take_ratio() const { return paper_liquidity_take_ratio_; }
    void set_paper_min_fill_ratio(double v) { paper_min_fill_ratio_ = v; }
    double paper_min_fill_ratio() const { return paper_min_fill_ratio_; }
    void set_paper_book_max_age_secs(double v) { paper_book_max_age_secs_ = v; }
    double paper_book_max_age_secs() const { return paper_book_max_age_secs_; }
    void set_paper_hedge_fail_rate(double v) { paper_hedge_fail_rate_ = v; }
    double paper_hedge_fail_rate() const { return paper_hedge_fail_rate_; }
    void set_paper_leg1_extra_slip_pct(double v) { paper_leg1_extra_slip_pct_ = v; }
    double paper_leg1_extra_slip_pct() const { return paper_leg1_extra_slip_pct_; }
    void set_paper_hedge_extra_slip_pct(double v) { paper_hedge_extra_slip_pct_ = v; }
    double paper_hedge_extra_slip_pct() const { return paper_hedge_extra_slip_pct_; }
    void set_paper_force_extra_slip_pct(double v) { paper_force_extra_slip_pct_ = v; }
    double paper_force_extra_slip_pct() const { return paper_force_extra_slip_pct_; }

    struct BookLevel {
        double price = 0.0;
        double size = 0.0;
    };
    struct WalkFillResult {
        double shares = 0.0;
        double avg_price = 0.0;
        double cost_usdc = 0.0;
        int levels_used = 0;
    };
    void update_rest_ask_ladder(std::string_view token_id, std::vector<BookLevel> levels);
    WalkFillResult walk_ask_fill(
        std::string_view token_id, double max_shares, double rest_max_age_sec = 20.0) const;
    WalkFillResult walk_paired_fill(
        std::string_view yes_token, std::string_view no_token,
        double max_shares, double balance_usdc, double balance_reserve = 0.995,
        double rest_max_age_sec = 20.0) const;
    std::optional<DetectionAsk> get_detection_ask(
        std::string_view token_id, double rest_max_age_sec = 20.0) const;

    void update_markets(const std::vector<MarketInfo>& markets);
    std::string get_dashboard_json() const;
    PriceTick get_latest_price(const std::string& asset) const;
    std::optional<double> get_price_at(const std::string& asset, double seconds_ago) const;

    // Telemetry & signal log
    void push_telemetry(const std::string& line);
    void push_signal(const std::string& line);

    struct MirrorAssetQuote {
        double book_yes = 0.0;
        double book_no = 0.0;
        double ws_yes = 0.0;
        double ws_no = 0.0;
        double updated_at = 0.0;
        bool fresh = false;
    };

    void set_lih_enabled(bool v) { lih_enabled_ = v; }
    bool lih_enabled() const { return lih_enabled_; }
    bool lih_use_mirror() const { return lih_use_mirror_; }
    void set_lih_config(double leg1_max, double target_combined, bool use_mirror) {
        lih_leg1_max_price_ = leg1_max;
        lih_target_combined_ = target_combined;
        lih_use_mirror_ = use_mirror;
    }
    double lih_leg1_max_price() const { return lih_leg1_max_price_; }
    double lih_target_combined() const { return lih_target_combined_; }
    void set_lih_mid_soft_cap(double v) { lih_mid_soft_cap_ = std::max(0.0, v); }
    double lih_mid_soft_cap() const { return lih_mid_soft_cap_; }
    void set_lih_leg1_mode(const std::string& mode) { lih_leg1_mode_ = mode; }
    bool lih_leg1_trend_mode() const { return lih_leg1_mode_ == "trend" || lih_leg1_mode_ == "expensive"; }
    bool lih_leg1_trigger_mode() const { return lih_leg1_mode_ == "trigger"; }
    void set_lih_leg1_trend_max_price(double v) { lih_leg1_trend_max_price_ = v; }
    double lih_leg1_trend_max_price() const { return lih_leg1_trend_max_price_; }
    void set_lih_leg1_trigger_min(double v) { lih_leg1_trigger_min_ = v; }
    double lih_leg1_trigger_min() const { return lih_leg1_trigger_min_; }
    void set_lih_leg1_trigger_max(double v) { lih_leg1_trigger_max_ = v; }
    double lih_leg1_trigger_max() const { return lih_leg1_trigger_max_; }
    /** leg1 execution: "taker" (FAK) or "gtc" (limit clip; shadow dry-run sim first). */
    void set_lih_leg1_order_mode(std::string mode) { lih_leg1_order_mode_ = std::move(mode); }
    const std::string& lih_leg1_order_mode() const { return lih_leg1_order_mode_; }
    /** LIH decision quotes: "rest" = REST book only (shadow-aligned); else max(WS,REST). */
    void set_lih_quote_mode(std::string mode) { lih_quote_mode_ = std::move(mode); }
    bool lih_quote_rest_only() const { return lih_quote_mode_ == "rest"; }
    /** Secondary (off BJ main) gates — mirrored for OrderRouter exec-time recheck. */
    void set_mm2_secondary_gates(int bj_start, int bj_end_exclusive, double offhours_min_ask) {
        mm2_main_bj_start_ = bj_start;
        mm2_main_bj_end_ = bj_end_exclusive;
        mm2_offhours_min_ask_ = offhours_min_ask > 0.0 ? offhours_min_ask : 0.0;
    }
    int mm2_main_bj_start() const { return mm2_main_bj_start_; }
    int mm2_main_bj_end() const { return mm2_main_bj_end_; }
    double mm2_offhours_min_ask() const { return mm2_offhours_min_ask_; }
    void set_live_lih_dry_run(bool v) { live_lih_dry_run_ = v; }
    bool live_lih_dry_run() const { return live_lih_dry_run_; }
    void set_trades_baseline_ts(double ts) { trades_baseline_ts_ = ts; }
    double trades_baseline_ts() const { return trades_baseline_ts_; }
    void set_mirror_path(std::string path) { mirror_path_ = std::move(path); }
    void reload_live_mirror(double max_age_sec = 45.0);
    std::optional<MirrorAssetQuote> get_mirror_quote(const std::string& asset) const;

private:
    risk::RiskManager* risk_manager_ = nullptr;
    bool paper_mode_ = true;
    double fee_rate_ = 0.018;
    std::string strategy_ = "leg_in";
    bool dh_enable_5m_ = true;
    bool dh_enable_15m_ = true;
    bool dh_5m_btc_ = true;
    bool dh_5m_eth_ = true;
    bool dh_5m_sol_ = true;
    bool dh_15m_btc_ = true;
    bool dh_15m_eth_ = true;
    bool binance_feed_enabled_ = true;
    bool book_aware_detect_ = true;
    bool paper_official_book_ = true;
    bool paper_depth_sim_ = false;
    double paper_slippage_pct_ = 0.0;
    bool paper_realism_enabled_ = false;
    double paper_liquidity_take_ratio_ = 1.0;
    double paper_min_fill_ratio_ = 0.0;
    double paper_book_max_age_secs_ = 20.0;
    double paper_hedge_fail_rate_ = 0.0;
    double paper_leg1_extra_slip_pct_ = 0.0;
    double paper_hedge_extra_slip_pct_ = 0.0;
    double paper_force_extra_slip_pct_ = 0.0;
    bool lih_enabled_ = true;
    double lih_leg1_max_price_ = 0.45;
    double lih_target_combined_ = 0.94;
    double lih_mid_soft_cap_ = 0.0;
    std::string lih_leg1_mode_ = "cheap";
    double lih_leg1_trend_max_price_ = 0.65;
    double lih_leg1_trigger_min_ = 0.70;
    double lih_leg1_trigger_max_ = 0.0;
    std::string lih_leg1_order_mode_ = "taker";
    std::string lih_quote_mode_ = "conservative";
    int mm2_main_bj_start_ = 8;
    int mm2_main_bj_end_ = 17;
    double mm2_offhours_min_ask_ = 0.0;
    bool lih_use_mirror_ = true;
    bool live_lih_dry_run_ = true;
    double trades_baseline_ts_ = 0.0;
    std::string mirror_path_ = "logs/live_mirror.json";
    mutable std::shared_mutex mirror_mutex_;
    std::unordered_map<std::string, MirrorAssetQuote> mirror_by_asset_;
    double mirror_loaded_at_ = 0.0;

    mutable std::shared_mutex btc_mutex_;
    PriceTick latest_btc_tick_{};
    std::deque<PriceTick> btc_history_;
    uint64_t btc_tick_count_ = 0;

    mutable std::shared_mutex eth_mutex_;
    PriceTick latest_eth_tick_{};
    std::deque<PriceTick> eth_history_;
    uint64_t eth_tick_count_ = 0;

    mutable std::shared_mutex sol_mutex_;
    PriceTick latest_sol_tick_{};
    std::deque<PriceTick> sol_history_;
    uint64_t sol_tick_count_ = 0;

    mutable std::shared_mutex token_mutex_;
    std::unordered_map<std::string, TokenPrice> token_prices_;
    std::unordered_map<std::string, TokenPrice> token_bids_;
    std::unordered_map<std::string, TokenPrice> ws_book_asks_;
    std::unordered_map<std::string, TokenPrice> rest_book_asks_;
    std::unordered_map<std::string, TokenPrice> rest_book_bids_;
    std::unordered_map<std::string, double> rest_book_depth_;
    std::unordered_map<std::string, std::vector<BookLevel>> rest_ask_ladders_;
    std::unordered_map<std::string, TokenFeeParams> token_fee_params_;

    mutable std::shared_mutex market_mutex_;
    std::vector<MarketInfo> markets_;

    mutable std::shared_mutex log_mutex_;
    std::deque<std::string> telemetry_log_;
    std::deque<std::string> signal_log_;
    static constexpr size_t MAX_LOG_LINES = 100;
};

} // namespace trading
