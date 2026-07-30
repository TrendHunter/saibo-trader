#pragma once

#include "../signals/Signal.h"
#include "../state/StateStore.h"
#include <string>

namespace trading {

enum class RegimeState { OK, RIDE, PAUSE_LEG1 };

struct RegimeDecision {
    RegimeState state = RegimeState::OK;
    bool leg1_allowed = true;
    bool force_spot_leg1 = false;
    double score_b = 0.0;
    double score_c = 0.0;
    bool pre_active = false;
    std::string reason;
    std::string pre_reason;
    std::string state_str() const;
};

/** Environment / news / vol+book gate for leg1 only (hedge always allowed). */
class RegimeGate {
public:
    void set_enabled(bool v) { enabled_ = v; }
    bool enabled() const { return enabled_; }
    void set_store(StateStore* s) { store_ = s; }

    void configure(
        double thresh_b,
        double thresh_c,
        double min_spot_bps,
        double target_combined,
        double b_jump_bps,
        double b_window_bps,
        double c_ask_sum_bad,
        double c_min_depth,
        double hedge_margin,
        int pre_cooldown_sec,
        int pre_extend_sec,
        int pre_max_sec,
        std::string pre_state_path);

    RegimeDecision evaluate(
        const MarketInfo& market, double now_sec, double yes_ask, double no_ask);

    const RegimeDecision& last_decision() const { return last_; }
    void append_dashboard(boost::json::object& root) const;

private:
    void reload_pre_file(double now_sec);
    void maybe_extend_pre(double now_sec, bool pause_live);
    double score_volatility(const MarketInfo& market, double window_total_sec, double secs_left) const;
    double score_book(
        const MarketInfo& market, double yes_ask, double no_ask) const;
    static double clamp01(double v);
    static double parse_iso_utc(const std::string& iso);
    static std::string state_to_string(RegimeState s);

    bool enabled_ = false;
    StateStore* store_ = nullptr;
    double thresh_b_ = 0.75;
    double thresh_c_ = 0.70;
    double min_spot_bps_ = 8.0;
    double target_combined_ = 0.97;
    double b_jump_bps_ = 25.0;
    double b_window_bps_ = 40.0;
    double c_ask_sum_bad_ = 0.015;
    double c_min_depth_ = 50.0;
    double hedge_margin_ = 0.03;
    int pre_cooldown_sec_ = 1800;
    int pre_extend_sec_ = 900;
    int pre_max_sec_ = 7200;
    std::string pre_state_path_ = "data/regime/regime_pre.json";

    bool pre_active_ = false;
    std::string pre_reason_;
    double pre_until_sec_ = 0.0;
    double pre_started_sec_ = 0.0;
    int pre_extend_count_ = 0;
    double last_pre_read_sec_ = 0.0;

    RegimeDecision last_;
};

} // namespace trading
