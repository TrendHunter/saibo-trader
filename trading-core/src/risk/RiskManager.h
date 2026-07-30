#pragma once

#include "../signals/Signal.h"
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <deque>
#include <optional>
#include <chrono>
#include <memory>
#include <mutex>
#include <boost/json.hpp>

namespace trading {
class ShadowWindowRecorder;
}

namespace risk {

enum class TradingStatus {
    ACTIVE,
    DAILY_HALT,
    KILLED,
    PAUSED
};

struct Position {
    std::string order_id;
    std::string token_id;
    std::string market_question;
    std::string side;
    double entry_price;
    double size_shares;
    double cost_usdc;
    double opened_at;
    double end_date_ts = 0.0;
    std::string asset = "";
    std::string direction = "";
    std::string strategy = "MANUAL";
    std::string condition_id = "";
    std::optional<double> closed_at;
    std::optional<double> exit_price;
    std::optional<double> pnl_usdc;
    bool paper_mode = true;
    double peak_price = 0.0;
    bool is_neg_risk = false; // true for Polymarket Up/Down (neg-risk) markets
};

/** mm2-tracker-v2 exec metadata for shadow_trades.jsonl (§3.7). */
struct LihExecMeta {
    std::string post_order_type = "FAK";
    std::string trader_side;
    std::string exec_class;
    std::string intent;
    std::string order_id;
    double side_ask_at_fill = 0.0;
    double side_bid_at_fill = 0.0;
    double price_vs_ask_cents = 0.0;
    bool has_side_ask = false;
    bool has_side_bid = false;
};

struct LegInHedgePosition {
    std::string lih_id;
    std::string asset;
    std::string market_question;
    std::string yes_token_id;
    std::string no_token_id;
    std::string condition_id;
    double yes_shares = 0.0;
    double no_shares = 0.0;
    double yes_cost = 0.0;
    double no_cost = 0.0;
    /** Weighted-average fill price per leg (excludes fees; matches exchange display). */
    double yes_entry_price = 0.0;
    double no_entry_price = 0.0;
    double opened_at = 0.0;
    double end_date_ts = 0.0;
    int window_minutes = 5;
    bool is_neg_risk = false;
    bool paper_mode = true;
    /** Live shadow (LIVE_LIH_DRY_RUN): in-memory slot only, no trade history / balance impact. */
    bool is_shadow = false;
    std::optional<double> closed_at;
    std::optional<double> yes_exit_price;
    std::optional<double> no_exit_price;
    std::optional<double> pnl_usdc;
    std::string exit_reason;
    int rebalance_count = 0;
    double entry_fees = 0.0;
};

class RiskManager {
public:
    RiskManager(
        double starting_balance,
        double max_position_fraction = 0.08,
        double daily_loss_limit = 0.20,
        double total_drawdown_kill = 0.40,
        int max_concurrent_positions = 3,
        bool circuit_breaker_enabled = true,
        int circuit_breaker_min_losses = 3,
        int circuit_breaker_window = 5,
        double circuit_breaker_loss_pct = 0.02,
        double circuit_breaker_pause_seconds = 300.0,
        double min_order_size = 5.0
    );

    TradingStatus get_status() const;
    bool is_trading_allowed();
    double get_current_balance() const;
    double get_total_equity() const;
    std::optional<std::string> get_status_reason() const;
    double get_daily_starting_balance() const;
    double get_peak_balance() const;
    double get_starting_balance() const;
    int get_open_position_count() const;
    double get_win_rate() const;
    double get_min_order_size() const;
    
    double get_la_pnl() const;
    int get_total_trades() const;
    int get_total_lih_trades() const;
    int get_winning_trades() const;

    const std::unordered_map<std::string, int>& get_asset_trades() const;
    const std::unordered_map<std::string, int>& get_asset_wins() const;
    const std::unordered_map<std::string, double>& get_asset_pnl() const;

    std::pair<bool, std::string> can_open_position(double position_size_usdc);
    void register_trade_open(const Position& position);
    std::optional<Position> register_trade_close(
        const std::string& order_id,
        double exit_price,
        std::optional<double> exit_timestamp = std::nullopt,
        std::optional<double> actual_proceeds_usdc = std::nullopt
    );

    std::pair<bool, std::string> can_open_lih_leg(
        double leg_cost_usdc,
        bool add_to_existing_lih = false,
        const std::string* lih_id = nullptr,
        double add_matched_shares = 0.0,
        const std::string* slot_asset = nullptr,
        int slot_window_minutes = 0);
    double get_max_leg_cost_usdc() const;
    /** Per market slot (asset+window) cumulative USDC cap; 0 = balance × max_position_fraction. */
    double get_lih_slot_cap_usdc() const;
    void set_lih_max_usdc_per_slot(double v);
    double get_lih_max_usdc_per_slot() const;
    double lih_slot_deployed_usdc(const std::string& asset, int window_minutes) const;
    double get_lih_max_matched_shares() const;
    double lih_remaining_matched_shares(const std::string& lih_id) const;
    void set_lih_max_matched_shares(double v);
    std::optional<LegInHedgePosition> find_open_lih_by_asset(
        const std::string& asset, int window_minutes) const;
    /** Match open LIH to a specific Gamma market (end_ts / token ids), not asset+window only. */
    std::optional<LegInHedgePosition> find_open_lih_for_market(
        const trading::MarketInfo& market) const;
    /** True if another asset/window slot is open or in-flight (global one-slot mode). */
    bool lih_other_slot_busy(const std::string& asset, int window_minutes) const;
    /** Block new leg1 when session leg cap reached (conservative rollout). */
    bool lih_session_leg1_blocked() const;
    void set_lih_one_slot_global(bool v);
    bool get_lih_one_slot_global() const;
    void set_lih_session_max_legs(int v);
    int get_lih_session_max_legs() const;
    int get_lih_session_legs_used() const;
    void reset_lih_session();

    /** Drop all in-memory open LIH rounds (shadow reset / bad reconcile cleanup). */
    void clear_open_lih_positions();
    void clear_closed_lih_positions();
    /** Merge split single-leg closed LIH rows (e.g. -recon orphans) into one hedged record. */
    void consolidate_closed_lih_positions();
    void set_lih_pause_after_round(bool v);
    bool get_lih_pause_after_round() const;
    /** Minimum wallet USDC before opening a new LIH leg1 (0 = off). */
    void set_lih_min_balance_usdc(double v);
    double get_lih_min_balance_usdc() const;
    /** Shadow dry-run: fixed bankroll (SHADOW_BANKROLL_USDC); skip wallet sync, apply PnL on close. */
    void set_shadow_virtual_bankroll(bool v);
    bool shadow_virtual_bankroll() const;
    void set_shadow_window_recorder(trading::ShadowWindowRecorder* r);

    /** True if LEG1 CLOB submit is in-flight for asset+window (not open position). */
    bool lih_leg1_inflight_only(const std::string& asset, int window_minutes) const;
    /** True if an open LIH round or in-flight LEG1 exists for asset+window. */
    bool lih_has_open_or_inflight(const std::string& asset, int window_minutes) const;
    /** Reserve LEG1 slot before CLOB submit; prevents duplicate live orders per tick/restart race. */
    bool try_begin_lih_leg1(const std::string& asset, int window_minutes);
    void end_lih_leg1_inflight(const std::string& asset, int window_minutes);
    /** True while a live hedge/rebalance order is in flight for this LIH round. */
    bool lih_rebalance_inflight(const std::string& lih_id) const;
    /** Reserve rebalance slot before CLOB submit; one in-flight order per lih_id. */
    bool try_begin_lih_rebalance(const std::string& lih_id);
    void end_lih_rebalance_inflight(const std::string& lih_id);
    /** Drop stale leg1/rebalance locks so the next round can enter cleanly. */
    void scrub_lih_inflight_locks(double now_sec);
    std::unordered_map<std::string, LegInHedgePosition> get_open_lih_positions() const;
    LegInHedgePosition register_lih_open_leg1(
        const trading::MarketInfo& market, bool buy_yes, double price, double shares, double now_sec,
        bool is_paper = true, bool debit_balance = true, bool is_shadow = false,
        const LihExecMeta* exec = nullptr);
    void register_lih_add_leg(
        const std::string& lih_id, bool buy_yes, double price, double shares, bool is_paper = true,
        bool debit_balance = true, const LihExecMeta* exec = nullptr);
    void register_lih_add_paired(
        const std::string& lih_id, double yes_price, double no_price, double shares, bool is_paper = true,
        bool debit_balance = true);
    /** Sell heavy-leg gap at bid (Phase B unwind). Returns false if position missing. */
    bool register_lih_unwind(
        const std::string& lih_id, bool sell_yes, double bid_price, double shares,
        bool is_paper = true, bool credit_balance = true);
    std::optional<LegInHedgePosition> register_lih_close(
        const std::string& lih_id,
        double yes_exit,
        double no_exit,
        const std::string& exit_reason,
        std::optional<double> exit_timestamp = std::nullopt);

    /** Fill missing end_date_ts / token ids from active Gamma markets. */
    void sync_lih_from_markets(const std::vector<trading::MarketInfo>& markets);
    /** Close open LIH rows whose end_date_ts is in the past (UI cleanup). */
    int purge_expired_lih_open(double now_sec, double grace_sec = 30.0);

    /** Recompute cash from LIH history; optional reset after false drawdown kill. */
    void reconcile_paper_balance(bool reset_trading_halt = false);

    std::unordered_map<std::string, Position> get_open_positions() const;
    std::vector<Position> get_closed_positions() const;
    std::vector<LegInHedgePosition> get_closed_lih_positions() const;
    double get_lih_pnl() const;

    void update_peak_price(const std::string& order_id, double peak_price);

    void update_balance(double new_balance);
    void set_daily_starting_balance(double balance);
    void set_live_starting_balance(double balance);

    void set_fee_rate(double rate) { fee_rate_ = rate; }
    double get_fee_rate() const { return fee_rate_; }
    double get_max_position_fraction() const;
    double get_daily_loss_limit() const;
    double get_total_drawdown_kill() const;
    int get_max_concurrent_positions() const;

    void set_max_position_fraction(double v);
    void set_daily_loss_limit(double v);
    void set_total_drawdown_kill(double v);
    void set_max_concurrent_positions(int v);

    void pause(const std::string& reason = "Manual pause");
    bool resume();
    bool reset_kill_switch(bool confirm = false);

    // Flat-close any open LA positions left from older sessions (strategy removed).
    int close_legacy_la_positions();

    /** Drop paper/shadow positions and ephemeral LIH locks (live-only startup). */
    void purge_paper_positions();

    boost::json::object export_live_lih_state() const;
    bool import_live_lih_state(const boost::json::object& doc);

private:
    void check_risk_thresholds();
    void check_circuit_breaker();
    void check_circuit_breaker_resume();
    void check_daily_reset();
    void trigger_kill_switch(const std::string& reason);
    void trigger_daily_halt(const std::string& reason);
    double compute_equity_unlocked() const;
    double net_lih_round_pnl(const LegInHedgePosition& p) const;
    static double next_midnight();
    void record_asset_close(const std::string& asset, double pnl, bool won);
    static double now();
    bool is_trading_allowed_no_lock();

    double max_position_fraction_;
    double daily_loss_limit_;
    double total_drawdown_kill_;
    int max_concurrent_positions_;
    bool circuit_breaker_enabled_;
    int circuit_breaker_min_losses_;
    double circuit_breaker_loss_pct_;
    double circuit_breaker_pause_seconds_;
    double min_order_size_;

    double starting_balance_;
    double current_balance_;
    double peak_balance_;
    double daily_starting_balance_;
    double daily_reset_time_;

    TradingStatus status_;
    std::optional<std::string> kill_reason_;

    std::unordered_map<std::string, Position> open_positions_;
    std::vector<Position> closed_positions_;

    std::unordered_map<std::string, LegInHedgePosition> open_lih_positions_;
    std::unordered_set<std::string> lih_leg1_inflight_;
    std::unordered_map<std::string, double> lih_leg1_inflight_since_;
    std::unordered_set<std::string> lih_rebalance_inflight_;
    std::vector<LegInHedgePosition> closed_lih_positions_;
    int total_lih_trades_ = 0;
    double lih_pnl_ = 0.0;

    int total_trades_ = 0;
    int winning_trades_ = 0;
    double total_pnl_ = 0.0;
    double la_pnl_ = 0.0;

    std::unordered_map<std::string, int> asset_trades_;
    std::unordered_map<std::string, int> asset_wins_;
    std::unordered_map<std::string, double> asset_pnl_;

    std::deque<double> recent_la_pnls_;
    int circuit_breaker_window_;
    double circuit_breaker_resume_at_ = 0.0;
    double fee_rate_ = 0.018;
    double lih_max_matched_shares_ = 0.0;
    /** 0 = use balance × max_position_fraction as per-slot cumulative cap. */
    double lih_max_usdc_per_slot_ = 0.0;
    bool lih_one_slot_global_ = true;
    int lih_session_max_legs_ = 2;
    int lih_session_legs_used_ = 0;
    bool lih_pause_after_round_ = false;
    double lih_min_balance_usdc_ = 10.0;
    bool shadow_virtual_bankroll_ = false;
    trading::ShadowWindowRecorder* shadow_window_recorder_ = nullptr;
    /** Shadow dry-run sizing base; never block leg1 when shadow_virtual_bankroll_. */
    double shadow_sizing_balance_unlocked() const;
    void maybe_pause_after_lih_round(const std::string& trigger);
    double lih_slot_cap_usdc_unlocked() const;
    bool lih_other_slot_busy_unlocked(const std::string& asset, int window_minutes) const;
    double lih_slot_deployed_usdc_unlocked(const std::string& asset, int window_minutes) const;
    mutable std::recursive_mutex mtx_;
};

} // namespace risk
