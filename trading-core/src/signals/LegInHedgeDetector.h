#pragma once

#include "Signal.h"
#include "../state/StateStore.h"
#include "../risk/RiskManager.h"
#include <optional>
#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace trading {

class ShadowWindowRecorder;
class RegimeGate;

struct LegInAction {
    enum class Kind { OpenLeg1, AddLeg1, CompleteHedge, UnwindLeg1 } kind;
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
                       double leg1_min_seconds_remaining = 60.0,
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
                       double leg1_trigger_max = 0.0,
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
                       double max_entry_marginal = 1.15,
                       double mid_soft_cap = 0.0,
                       double mid_soft_start_secs = 300.0,
                       bool hedge_feasible_entry = false,
                       double hedge_feasible_cap = 0.0);

    std::optional<LegInAction> evaluate(double now_ms, risk::RiskManager& rm);

    void set_shadow_window_recorder(ShadowWindowRecorder* r) { shadow_window_recorder_ = r; }
    void set_regime_gate(RegimeGate* g) { regime_gate_ = g; }
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
    /** Skip leg1 in windows that opened before process boot (avoid deploy mid-window). */
    void set_skip_partial_window_on_start(bool v) { skip_partial_window_on_start_ = v; }
    void set_process_boot_sec(double v) { process_boot_sec_ = v; }
    void set_use_mirror_prices(bool v) { use_mirror_prices_ = v; }
    void set_leg1_shares(double v) { leg1_shares_ = v; }
    /** Per-tick leg1 clip; 0 = single fill of leg1_shares_. */
    void set_leg1_clip_shares(double v) { leg1_clip_shares_ = v; }
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
    /** 0 = no upper cap (legacy trigger). */
    void set_leg1_trigger_max(double v) { leg1_trigger_max_ = v; }
    void set_endgame_secs(double v) { endgame_secs_ = v; }
    void set_endgame_hold_ask(double v) { endgame_hold_ask_ = v; }
    void set_endgame_resume_hedge_ask(double v) { endgame_resume_hedge_ask_ = v; }
    void set_endgame_soft_cap(double v) { endgame_soft_cap_ = v; }
    void set_endgame_override_secs(double v) { endgame_override_secs_ = v; }
    void set_endgame_minimize_gap(bool v) { endgame_minimize_gap_ = v; }
    void set_endgame_ladder_enabled(bool v) { endgame_ladder_enabled_ = v; }
    void set_max_entry_marginal(double v) { max_entry_marginal_ = v; }
    void set_mid_soft_cap(double v) { mid_soft_cap_ = v; }
    void set_mid_soft_start_secs(double v) { mid_soft_start_secs_ = v; }
    void set_hedge_feasible_entry(bool v) { hedge_feasible_entry_ = v; }
    void set_hedge_feasible_cap(double v) { hedge_feasible_cap_ = v; }
    void set_vwap_entry_gate(bool v) { vwap_entry_gate_ = v; }
    void set_vwap_entry_cap(double v) { vwap_entry_cap_ = v; }
    void set_vwap_depth_ratio(double v) { vwap_depth_ratio_ = v; }
    void set_min_edge_usdc(double v) { min_edge_usdc_ = v; }
    void set_min_edge_per_share(double v) { min_edge_per_share_ = v; }
    void set_unwind_enabled(bool v) { unwind_enabled_ = v; }
    void set_unwind_secs(double v) { unwind_secs_ = v; }
    void set_unwind_cooldown(double v) { unwind_cooldown_ = v; }
    /** Interleave light-leg hedge clips while leg1 still scaling (tracked-account style). */
    void set_parallel_clip_hedge(bool v) { parallel_clip_hedge_ = v; }
    /** Upper bound for parallel clip comb (default 1.0); no profit floor — 0.80 is fine. */
    void set_parallel_hedge_max_combined(double v) { parallel_hedge_max_combined_ = v; }
    /** Optional hard ceiling on parallel-clip comb; 0 = off. */
    void set_early_hedge_max_combined(double v) { early_hedge_max_combined_ = v; }
    /** mm2-style: scale heavy leg by book (100sh), light clips 5sh, leave gap at settle. */
    void set_open_gap_mode(bool v) { open_gap_mode_ = v; }
    /** Late-window entry: spot momentum + favorite side, slow heavy then delayed hedge. */
    void set_mm2_mode(bool v) { mm2_mode_ = v; }
    void set_mm2_min_spot_bps(double v) { mm2_min_spot_bps_ = std::max(0.0, v); }
    /** Enter only when secs_left <= this (e.g. 90 = last 90s of 5m window). */
    void set_mm2_entry_max_secs_left(double v) { mm2_entry_max_secs_left_ = std::max(0.0, v); }
    /** Skip entry when secs_left below this (too close to settle). */
    void set_mm2_entry_min_secs_left(double v) { mm2_entry_min_secs_left_ = std::max(0.0, v); }
    /** Heavy side ask must be >= this (favorite / expensive side). */
    void set_mm2_favorite_min_px(double v) { mm2_favorite_min_px_ = v; }
    /** Weak spot: pick spot direction when |bps| >= this but below min_spot_bps (0=off). */
    void set_mm2_soft_spot_bps(double v) { mm2_soft_spot_bps_ = std::max(0.0, v); }
    /** Late tilt: when max ask >= this and spread >= min_spread, pick expensive side (0=off). */
    void set_mm2_late_tilt_min_ask(double v) { mm2_late_tilt_min_ask_ = std::max(0.0, v); }
    void set_mm2_late_tilt_min_spread(double v) { mm2_late_tilt_min_spread_ = std::max(0.0, v); }
    /** No light-leg hedge until this many seconds after first heavy fill. */
    void set_mm2_heavy_delay_sec(double v) { mm2_heavy_delay_sec_ = std::max(0.0, v); }
    /** Per-tick heavy scale clip in mm2 mode (slow build). */
    void set_mm2_scale_clip_shares(double v) { mm2_scale_clip_shares_ = std::max(0.0, v); }
    /** Cap total heavy shares when scaling (0 = stop at leg1_shares). Path sizing. */
    void set_mm2_heavy_max_shares(double v) { mm2_heavy_max_shares_ = std::max(0.0, v); }
    /** Multiply scale clip when spot favors heavy (>= min_spot_bps). 1 = off. */
    void set_mm2_scale_boost(double v) { mm2_scale_boost_ = std::max(1.0, v); }
    /** Multiply hedge clip when spot against heavy (<= -min_spot_bps). 1 = off. */
    void set_mm2_hedge_boost(double v) { mm2_hedge_boost_ = std::max(1.0, v); }
    /**
     * Stop same-side scale when |spot| >= thr and spot against heavy (0=off).
     * Default 5bps (against_only sweep). Independent of LIH_MM2_MIN_SPOT_BPS.
     */
    void set_mm2_scale_against_stop_bps(double v) {
        mm2_scale_against_stop_bps_ = std::max(0.0, v);
    }
    double mm2_scale_against_stop_bps() const { return mm2_scale_against_stop_bps_; }
    /**
     * HF1-E′: once |yes-no| <= gap_thr AND secs_left <= secs_thr, latch and block
     * further same-side scale for this window (0 gap thr = off). Hedge still runs.
     */
    void set_mm2_hf1e_latch_gap(double v) { mm2_hf1e_latch_gap_ = std::max(0.0, v); }
    void set_mm2_hf1e_latch_secs_left(double v) {
        mm2_hf1e_latch_secs_left_ = std::max(0.0, v);
    }
    double mm2_hf1e_latch_gap() const { return mm2_hf1e_latch_gap_; }
    double mm2_hf1e_latch_secs_left() const { return mm2_hf1e_latch_secs_left_; }
    /**
     * Early lane: when secs_left in (entry_max, early_max], only strong-spot /
     * tilt-fav may enter; else wait for late band. 0 = late-only (entry_max).
     */
    void set_mm2_early_entry_max_secs_left(double v) {
        mm2_early_entry_max_secs_left_ = std::max(0.0, v);
    }
    void set_mm2_early_tilt_min_spread(double v) { mm2_early_tilt_min_spread_ = std::max(0.0, v); }
    void set_mm2_early_tilt_min_fav(double v) { mm2_early_tilt_min_fav_ = std::max(0.0, v); }
    void set_mm2_early_yes_guard(bool v) { mm2_early_yes_guard_ = v; }
    /**
     * Fav-early bypass: when secs_left > early_entry_max, still allow leg1 if
     * favorite-side shape matches research gate B/C (default off; shadow first).
     * Mode B: (spread>=min_spread OR |Δtilt|>=min_dtilt)
     * Mode C: fav_ask in [lo,hi] AND (spread OR Δtilt)
     */
    void set_mm2_fav_early_bypass(bool v) { mm2_fav_early_bypass_ = v; }
    void set_mm2_fav_early_mode(std::string mode) {
        if (!mode.empty() && (mode[0] == 'c' || mode[0] == 'C')) mm2_fav_early_mode_ = "C";
        else mm2_fav_early_mode_ = "B";
    }
    void set_mm2_fav_early_fav_lo(double v) { mm2_fav_early_fav_lo_ = std::max(0.0, std::min(1.0, v)); }
    void set_mm2_fav_early_fav_hi(double v) { mm2_fav_early_fav_hi_ = std::max(0.0, std::min(1.0, v)); }
    void set_mm2_fav_early_min_spread(double v) { mm2_fav_early_min_spread_ = std::max(0.0, v); }
    void set_mm2_fav_early_min_dtilt(double v) { mm2_fav_early_min_dtilt_ = std::max(0.0, v); }
    /**
     * Main-field BJ hours [start, end) keep full v2k. Outside = secondary:
     * require entry ask >= offhours_min_ask (0=off). Default 8..17.
     */
    void set_mm2_main_bj_hours(int start_hour, int end_hour_exclusive) {
        mm2_main_bj_start_ = start_hour;
        mm2_main_bj_end_ = end_hour_exclusive;
    }
    int mm2_main_bj_start() const { return mm2_main_bj_start_; }
    int mm2_main_bj_end() const { return mm2_main_bj_end_; }
    void set_mm2_offhours_min_ask(double v) { mm2_offhours_min_ask_ = std::max(0.0, v); }
    /**
     * Skip leg1 when min(yes,no) rest ask depth < threshold (0=off).
     * Live proxy for mm2 low_liquidity; optional main-field only.
     */
    void set_mm2_min_side_depth(double v) { mm2_min_side_depth_ = std::max(0.0, v); }
    void set_mm2_min_side_depth_main_only(bool v) { mm2_min_side_depth_main_only_ = v; }
    /** Cap first heavy fill USDC when side came from strong spot (0=off). v2i revised. */
    void set_mm2_spot_leg1_max_usdc(double v) { mm2_spot_leg1_max_usdc_ = std::max(0.0, v); }
    /** Cap total heavy notional while |spot| still strong (0=off). */
    void set_mm2_spot_heavy_max_usdc(double v) { mm2_spot_heavy_max_usdc_ = std::max(0.0, v); }
    /** UTC hour window for mm2 session (end exclusive); end < 0 disables gate. */
    void set_mm2_session_utc_hours(int start_hour, int end_hour_exclusive) {
        mm2_session_utc_start_ = start_hour;
        mm2_session_utc_end_ = end_hour_exclusive;
    }
    int mm2_session_utc_start() const { return mm2_session_utc_start_; }
    int mm2_session_utc_end() const { return mm2_session_utc_end_; }
    /** Load active UTC hours from mm2 observation JSON (active_hours_utc). */
    bool load_mm2_session_file(const std::string& path);
    /** Load active UTC hours from m2/YYYY-MM-DD/windows.jsonl session_active flags. */
    bool load_mm2_session_from_pack(const std::string& m2_root);
    bool mm2_session_hour_allowed(int hour_utc) const;
    /** Load mm2 observation pack skip index for today UTC (windows.jsonl). */
    bool load_mm2_obs_skip_pack(const std::string& m2_root);
    bool mm2_obs_skips_leg1(int64_t window_start_ts) const;
    void set_mm2_obs_skip_from_pack(bool v) { mm2_obs_skip_from_pack_ = v; }
    /**
     * Replay: only open leg1 when m2 pack shows a traded cue for this window
     * (same side, after mm2 sec_in). Hedge path stays bot LIH. Needs near-live pack.
     */
    void set_mm2_replay_leg1(bool v) { mm2_replay_leg1_ = v; }
    void set_mm2_replay_lag_tol_sec(double v) { mm2_replay_lag_tol_sec_ = std::max(0.0, v); }
    bool load_mm2_replay_leg1_pack(const std::string& m2_root);
    void set_mm2_skip_flat(bool v) { mm2_skip_flat_ = v; }
    void set_mm2_flat_max_spot_bps(double v) { mm2_flat_max_spot_bps_ = std::max(0.0, v); }
    void set_mm2_flat_max_ask_sum(double v) { mm2_flat_max_ask_sum_ = std::max(0.0, v); }
    /** P8: spot band + choppiness gate from mm2 vol analysis (snap_120-270 std). */
    void set_mm2_vol_gate(bool v) { mm2_vol_gate_ = v; }
    void set_mm2_vol_min_spot_bps(double v) { mm2_vol_min_spot_bps_ = std::max(0.0, v); }
    void set_mm2_vol_max_spot_bps(double v) { mm2_vol_max_spot_bps_ = std::max(0.0, v); }
    void set_mm2_vol_max_spot_std(double v) { mm2_vol_max_spot_std_ = std::max(0.0, v); }
    /**
     * v2j adaptive side (after strong/soft spot):
     *   mom_with_fav ∧ fav_ask<=mom_max → favorite
     *   spread>=min ∧ fav_ask<=spread_max → favorite
     *   else cheap
     */
    void set_mm2_v2j_adaptive(bool v) { mm2_v2j_adaptive_ = v; }
    void set_mm2_v2j_mom_lookback_sec(double v) { mm2_v2j_mom_lookback_sec_ = std::max(1.0, v); }
    void set_mm2_v2j_mom_fav_max_ask(double v) { mm2_v2j_mom_fav_max_ask_ = std::max(0.0, v); }
    void set_mm2_v2j_spread_min(double v) { mm2_v2j_spread_min_ = std::max(0.0, v); }
    void set_mm2_v2j_spread_fav_max_ask(double v) { mm2_v2j_spread_fav_max_ask_ = std::max(0.0, v); }
    /**
     * Tilt-formation entry (research A): wait |tilt_now - tilt_open| >= delta,
     * then adaptive_ext2 side (extreme→cheap, mid fav_px→fav, else cheap).
     * When on, replaces v2j/late-cheap path (spot overrides still win).
     */
    void set_mm2_tilt_entry(bool v) { mm2_tilt_entry_ = v; }
    void set_mm2_tilt_delta(double v) { mm2_tilt_delta_ = std::max(0.0, v); }
    /**
     * After tilt forms: pick the strengthened side (d_tilt>0 → YES), not adaptive_ext2.
     * Research: S2-D1 tilt_follow (~70% hit_win).
     */
    void set_mm2_tilt_side_follow(bool v) { mm2_tilt_side_follow_ = v; }
    /**
     * After side chosen at tilt form: wait until that side's ask dips below form ask
     * before LEG1; if secs_left hits entry_min, take current ask (deadline).
     * Research: cheaper_later.
     */
    void set_mm2_cheaper_later(bool v) { mm2_cheaper_later_ = v; }
    /** Max single heavy-side clip (book-sized down); default 100. */
    void set_heavy_clip_shares(double v) { heavy_clip_shares_ = v; }
    /** Max ask to scale heavy / first leg (e.g. 0.75 for 0.62 entries). */
    void set_heavy_max_price(double v) { heavy_max_price_ = v; }
    /** Open-gap: target max |YES-NO| at settle; 0 = no cap. Over-gap uses gap_hedge_max. */
    void set_max_gap_shares(double v) { max_gap_shares_ = std::max(0.0, v); }
    /** Relaxed marginal cap when gap exceeds max_gap_shares (e.g. 1.08). */
    void set_gap_hedge_max_combined(double v) { gap_hedge_max_combined_ = v; }
    /** Skip light-leg hedge until gap >= this (0 = off). */
    void set_hedge_min_gap_trigger(double v) { hedge_min_gap_trigger_ = std::max(0.0, v); }
    /** When hedging, leave at least this much |YES-NO| (0 = hedge toward 0). */
    void set_hedge_target_min_gap(double v) { hedge_target_min_gap_ = std::max(0.0, v); }

private:
    struct VwapEntryEval {
        bool ok = false;
        double paired_sh = 0.0;
        double yes_vwap = 0.0;
        double no_vwap = 0.0;
        double comb = 0.0;
        double net_edge = 0.0;
        double min_edge = 0.0;
    };
    bool leg1_trend_allows(const MarketInfo& market, bool pick_yes) const;
    /** Binance spot direction for endgame hold (always checked; independent of leg1_trend_align). */
    bool spot_trend_favors(const MarketInfo& market, bool pick_yes) const;
    /** Window-open → now spot return in bps (Binance). */
    std::optional<double> window_spot_ret_bps(
        const MarketInfo& market, double window_total_sec, double secs_left) const;
    /** Window-open → price lookback_sec ago, return in bps (for Δspot / mom). */
    std::optional<double> window_spot_ret_bps_ago(
        const MarketInfo& market, double window_total_sec, double secs_left,
        double lookback_sec) const;
    bool mm2_spot_favors_heavy(
        const MarketInfo& market, bool heavy_yes,
        double window_total_sec, double secs_left) const;
    bool mm2_spot_against_heavy(
        const MarketInfo& market, bool heavy_yes,
        double window_total_sec, double secs_left,
        double thr_bps = -1.0) const;
    double mm2_heavy_scale_cap() const;
    struct Mm2SpotTrack {
        uint8_t milestones = 0;
        std::vector<double> bps;
    };
    void mm2_touch_spot_track(const std::string& key, double elapsed, double spot_bps);
    std::optional<std::string> mm2_vol_gate_reason(
        double spot_bps, const std::vector<double>& samples) const;
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
    double opposite_rest_depth(const std::string& token_id) const;
    /** Cap clip to REST ask ladder; 0 if book stale or below CLOB min size. */
    double shrink_for_rest_book(const std::string& token_id, double want_shares) const;
    static constexpr double kClobMinOrderShares = 5.0;
    bool passes_hedge_feasible_entry(
        const MarketInfo& market, bool pick_yes, double leg1_px, double leg1_shares,
        double opposite_px) const;
    VwapEntryEval eval_vwap_entry(const MarketInfo& market, double want_sh,
                                  double fee_rate) const;
    void log_rebalance_status(const MarketInfo& market, const std::string& key, double now_sec,
                              const risk::LegInHedgePosition& pos, const Quote& q,
                              double yes_avg, double no_avg, double gap) const;
    void log_entry_status(const MarketInfo& market, const std::string& key, double now_sec,
                          const Quote& q, const char* reason) const;
    /** Time-ramped max marginal in endgame ladder window (e.g. 0.95 → 0.97 over 90s). */
    double endgame_ladder_max_marginal(double secs_left) const;
    /** Mid-window parallel clip: upper bound only (ramps late); no LIH_TARGET floor. */
    double active_parallel_hedge_cap(double secs_left) const;
    /** Parallel / gap-cap marginal ceiling; relaxes when gap > max_gap_shares. */
    double gap_hedge_cap_for(double secs_left, double gap) const;

    StateStore& store_;
    std::vector<MarketInfo> markets_;
    double leg1_max_price_;
    double target_combined_;
    double min_seconds_remaining_;
    /** No new leg1 when secs_left below this — wait for next window. */
    double leg1_min_seconds_remaining_;
    /** No leg1 until this many seconds after window open (skip opening volatility). */
    double leg1_start_delay_sec_;
    bool skip_partial_window_on_start_ = true;
    double process_boot_sec_ = 0.0;
    double leg1_cooldown_seconds_;
    double rebalance_cooldown_seconds_;
    bool use_mirror_prices_;
    double leg1_shares_;
    double leg1_clip_shares_;
    bool allow_over_target_;
    double force_balance_secs_;
    double max_rebalance_shares_;
    bool flex_rebalance_;
    double flex_dilute_ratio_;
    bool leg1_trend_align_;
    double trend_lookback_sec_;
    bool leg1_trend_mode_;
    double leg1_trend_max_price_;
    /** Shadow-style: enter when either side ask in [trigger_min, trigger_max] (pick higher). */
    bool leg1_trigger_mode_;
    double leg1_trigger_min_;
    double leg1_trigger_max_;
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
    /** Mid-window: hedge when marginal <= this (must be > target; 0 = off). */
    double mid_soft_cap_;
    /** Apply mid-soft when secs_left <= this (0 = always when not in endgame). */
    double mid_soft_start_secs_;
    bool hedge_feasible_entry_;
    /** Max leg1+opp at entry; 0 = use mid_soft_cap_. */
    double hedge_feasible_cap_;
    /** Walk REST book on both legs before leg1 (Roh VWAP gate). */
    bool vwap_entry_gate_;
    /** Max vwap_yes+vwap_no; 0 = target_combined or mid_soft_cap. */
    double vwap_entry_cap_;
    /** Min paired fill vs requested leg1 shares. */
    double vwap_depth_ratio_;
    /** Min net USDC after fees; 0 = use min_edge_per_share × sh. */
    double min_edge_usdc_;
    double min_edge_per_share_;
    /** Phase B: sell heavy leg at bid if still unhedged after T seconds. */
    bool unwind_enabled_;
    double unwind_secs_;
    double unwind_cooldown_;
    bool parallel_clip_hedge_ = false;
    double parallel_hedge_max_combined_ = 1.0;
    double early_hedge_max_combined_ = 0.0;
    bool open_gap_mode_ = false;
    bool mm2_mode_ = false;
    double mm2_min_spot_bps_ = 0.0;
    double mm2_entry_max_secs_left_ = 180.0;
    double mm2_entry_min_secs_left_ = 20.0;
    double mm2_favorite_min_px_ = 0.48;
    double mm2_soft_spot_bps_ = 0.0;
    double mm2_late_tilt_min_ask_ = 0.0;
    double mm2_late_tilt_min_spread_ = 0.15;
    double mm2_heavy_delay_sec_ = 40.0;
    double mm2_scale_clip_shares_ = 10.0;
    double mm2_heavy_max_shares_ = 0.0;
    double mm2_scale_boost_ = 1.0;
    double mm2_hedge_boost_ = 1.0;
    /** Against-only scale stop threshold in bps (0=off). */
    double mm2_scale_against_stop_bps_ = 0.0;
    /** HF1-E′ latch: gap<=thr and secs_left<=secs (0 gap=off). */
    double mm2_hf1e_latch_gap_ = 0.0;
    double mm2_hf1e_latch_secs_left_ = 100.0;
    std::unordered_map<std::string, bool> mm2_hf1e_latched_by_key_;
    double mm2_early_entry_max_secs_left_ = 0.0;
    double mm2_early_tilt_min_spread_ = 0.35;
    double mm2_early_tilt_min_fav_ = 0.65;
    bool mm2_early_yes_guard_ = false;
    bool mm2_fav_early_bypass_ = false;
    std::string mm2_fav_early_mode_ = "B";
    double mm2_fav_early_fav_lo_ = 0.52;
    double mm2_fav_early_fav_hi_ = 0.65;
    double mm2_fav_early_min_spread_ = 0.15;
    double mm2_fav_early_min_dtilt_ = 0.10;
    /** BJ main field [start, end); end<=start disables split (treat all as main). */
    int mm2_main_bj_start_ = 8;
    int mm2_main_bj_end_ = 17;
    /** Off-hours: skip leg1 when chosen ask below this (0=off). Secondary strategy. */
    double mm2_offhours_min_ask_ = 0.0;
    /** Skip when min(yes,no) rest depth below this (0=off). Daytime low_liq proxy. */
    double mm2_min_side_depth_ = 0.0;
    bool mm2_min_side_depth_main_only_ = true;
    double mm2_spot_leg1_max_usdc_ = 0.0;
    double mm2_spot_heavy_max_usdc_ = 0.0;
    int mm2_session_utc_start_ = -1;
    int mm2_session_utc_end_ = -1;
    std::array<bool, 24> mm2_session_hours_{};
    bool mm2_session_use_hour_list_ = false;
    bool mm2_obs_skip_from_pack_ = false;
    std::unordered_map<int64_t, std::string> mm2_obs_skip_by_wts_;
    bool mm2_replay_leg1_ = false;
    double mm2_replay_lag_tol_sec_ = 15.0;
    struct Mm2ReplayCue {
        bool buy_yes = true;
        double sec_in = 0.0;
        double shares = 0.0;
    };
    std::unordered_map<int64_t, Mm2ReplayCue> mm2_replay_leg1_by_wts_;
    bool mm2_skip_flat_ = false;
    double mm2_flat_max_spot_bps_ = 0.0;
    double mm2_flat_max_ask_sum_ = 0.0;
    bool mm2_vol_gate_ = false;
    double mm2_vol_min_spot_bps_ = 0.0;
    double mm2_vol_max_spot_bps_ = 0.0;
    double mm2_vol_max_spot_std_ = 0.0;
    std::unordered_map<std::string, Mm2SpotTrack> mm2_spot_tracks_;
    bool mm2_v2j_adaptive_ = false;
    double mm2_v2j_mom_lookback_sec_ = 60.0;
    double mm2_v2j_mom_fav_max_ask_ = 0.60;
    double mm2_v2j_spread_min_ = 0.12;
    double mm2_v2j_spread_fav_max_ask_ = 0.58;
    bool mm2_tilt_entry_ = false;
    double mm2_tilt_delta_ = 0.25;
    bool mm2_tilt_side_follow_ = false;
    bool mm2_cheaper_later_ = false;
    /** First-seen yes_ask - no_ask for each window key (tilt open). */
    std::unordered_map<std::string, double> mm2_open_tilt_by_key_;
    struct CheaperLaterArm {
        bool pick_yes = true;
        double form_ask = 0.0;
        std::string tag;
    };
    /** Armed after tilt form when cheaper_later is on; cleared on LEG1 / window end. */
    std::unordered_map<std::string, CheaperLaterArm> mm2_cheaper_later_by_key_;
    double heavy_clip_shares_ = 100.0;
    double heavy_max_price_ = 0.75;
    double max_gap_shares_ = 0.0;
    double gap_hedge_max_combined_ = 0.0;
    double hedge_min_gap_trigger_ = 0.0;
    double hedge_target_min_gap_ = 0.0;
    double light_clip_shares() const;
    double heavy_clip_cap() const;
    mutable std::unordered_map<std::string, double> last_status_log_sec_;
    std::unordered_map<std::string, double> last_leg1_time_;
    std::unordered_map<std::string, double> last_rebalance_time_;
    std::unordered_map<std::string, double> last_unwind_time_;
    mutable std::unordered_map<std::string, double> last_entry_log_sec_;
    ShadowWindowRecorder* shadow_window_recorder_ = nullptr;
    RegimeGate* regime_gate_ = nullptr;
};

} // namespace trading
