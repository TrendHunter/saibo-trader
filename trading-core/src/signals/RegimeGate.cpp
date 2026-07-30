#include "RegimeGate.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <boost/json.hpp>
#include <spdlog/spdlog.h>

namespace trading {

namespace {
constexpr double kFloatTol = 1e-6;
constexpr double kPreReadIntervalSec = 5.0;
} // namespace

std::string RegimeDecision::state_str() const {
    switch (state) {
    case RegimeState::RIDE: return "RIDE";
    case RegimeState::PAUSE_LEG1: return "PAUSE_LEG1";
    default: return "OK";
    }
}

void RegimeGate::configure(
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
    std::string pre_state_path) {
    thresh_b_ = thresh_b;
    thresh_c_ = thresh_c;
    min_spot_bps_ = min_spot_bps;
    target_combined_ = target_combined;
    b_jump_bps_ = b_jump_bps;
    b_window_bps_ = b_window_bps;
    c_ask_sum_bad_ = c_ask_sum_bad;
    c_min_depth_ = c_min_depth;
    hedge_margin_ = hedge_margin;
    pre_cooldown_sec_ = pre_cooldown_sec;
    pre_extend_sec_ = pre_extend_sec;
    pre_max_sec_ = pre_max_sec;
    pre_state_path_ = std::move(pre_state_path);
}

double RegimeGate::clamp01(double v) {
    return std::max(0.0, std::min(1.0, v));
}

double RegimeGate::parse_iso_utc(const std::string& iso) {
    if (iso.size() < 20) return 0.0;
    std::tm tm{};
    std::istringstream ss(iso);
    ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    if (ss.fail()) return 0.0;
#ifdef _WIN32
    return static_cast<double>(_mkgmtime(&tm));
#else
    return static_cast<double>(timegm(&tm));
#endif
}

void RegimeGate::reload_pre_file(double now_sec) {
    if (now_sec - last_pre_read_sec_ < kPreReadIntervalSec) return;
    last_pre_read_sec_ = now_sec;

    std::ifstream in(pre_state_path_);
    if (!in) return;

    try {
        const auto val = boost::json::parse(in);
        if (!val.is_object()) return;
        const auto& o = val.as_object();
        const bool file_active = o.contains("pre_active") && o.at("pre_active").is_bool()
            && o.at("pre_active").as_bool();
        double until = 0.0;
        if (o.contains("pre_until_utc") && o.at("pre_until_utc").is_string()) {
            until = parse_iso_utc(std::string(o.at("pre_until_utc").as_string().c_str()));
        }
        if (o.contains("pre_until_unix") && o.at("pre_until_unix").is_number()) {
            until = std::max(until, o.at("pre_until_unix").to_number<double>());
        }
        std::string reason;
        if (o.contains("pre_reason") && o.at("pre_reason").is_string()) {
            reason = std::string(o.at("pre_reason").as_string().c_str());
        }

        if (file_active && until > now_sec + kFloatTol) {
            if (!pre_active_ || until > pre_until_sec_ + kFloatTol) {
                pre_started_sec_ = now_sec;
                pre_extend_count_ = 0;
            }
            pre_active_ = true;
            pre_until_sec_ = until;
            pre_reason_ = reason;
        } else if (!file_active && until <= now_sec + kFloatTol) {
            if (pre_until_sec_ <= now_sec + kFloatTol) {
                pre_active_ = false;
                pre_reason_.clear();
            }
        }
        if (o.contains("pre_extend_count") && o.at("pre_extend_count").is_int64()) {
            pre_extend_count_ = static_cast<int>(o.at("pre_extend_count").as_int64());
        }
    } catch (const std::exception& e) {
        spdlog::debug("RegimeGate pre file parse: {}", e.what());
    }
}

void RegimeGate::maybe_extend_pre(double now_sec, bool pause_live) {
    if (!pre_active_ || pre_until_sec_ <= 0.0) return;
    if (now_sec + kFloatTol < pre_until_sec_) return;

    const double elapsed = now_sec - pre_started_sec_;
    if (pause_live && elapsed + kFloatTol < static_cast<double>(pre_max_sec_)) {
        pre_until_sec_ = now_sec + static_cast<double>(pre_extend_sec_);
        ++pre_extend_count_;
        spdlog::info(
            "[REGIME] Pre extended +{}s (B+C chaos) | until={:.0f} extend#={}",
            pre_extend_sec_, pre_until_sec_, pre_extend_count_);
    } else {
        pre_active_ = false;
        pre_reason_.clear();
    }
}

double RegimeGate::score_volatility(
    const MarketInfo& market, double window_total_sec, double secs_left) const {
    if (!store_) return 0.0;
    const std::string asset = market.asset;
    double score = 0.0;

    const PriceTick latest = store_->get_latest_price(asset);
    if (latest.price <= kFloatTol) return 0.0;

    if (auto px30 = store_->get_price_at(asset, 30.0)) {
        const double jump_bps = std::abs(latest.price - *px30) / *px30 * 10000.0;
        if (b_jump_bps_ > kFloatTol) {
            score = std::max(score, clamp01(jump_bps / (b_jump_bps_ * 2.0)));
        }
    }
    if (auto px60 = store_->get_price_at(asset, 60.0)) {
        const double rv_bps = std::abs(latest.price - *px60) / *px60 * 10000.0;
        if (b_jump_bps_ > kFloatTol) {
            score = std::max(score, clamp01(rv_bps / (b_jump_bps_ * 2.4)));
        }
    }

    const double elapsed = window_total_sec - secs_left;
    if (elapsed > kFloatTol) {
        if (auto open_px = store_->get_price_at(asset, elapsed)) {
            const double win_bps = std::abs(latest.price - *open_px) / *open_px * 10000.0;
            if (b_window_bps_ > kFloatTol) {
                score = std::max(score, clamp01(win_bps / b_window_bps_));
            }
        }
    }
    return score;
}

double RegimeGate::score_book(
    const MarketInfo& market, double yes_ask, double no_ask) const {
    if (yes_ask <= kFloatTol || no_ask <= kFloatTol) return 1.0;

    double score = 0.0;
    const double ask_sum = yes_ask + no_ask;
    const double spread = std::abs(ask_sum - 1.0);
    if (c_ask_sum_bad_ > kFloatTol) {
        score = std::max(score, clamp01(spread / c_ask_sum_bad_));
    }

    double yes_depth = 0.0;
    double no_depth = 0.0;
    if (store_) {
        if (auto yd = store_->get_detection_ask(market.yes_token_id)) {
            if (yd->rest_ok) yes_depth = yd->rest_depth_shares;
        }
        if (auto nd = store_->get_detection_ask(market.no_token_id)) {
            if (nd->rest_ok) no_depth = nd->rest_depth_shares;
        }
    }
    const double depth_min = std::min(yes_depth, no_depth);
    if (c_min_depth_ > kFloatTol && depth_min + kFloatTol < c_min_depth_) {
        score = std::max(score, clamp01(1.0 - depth_min / c_min_depth_));
    }

    if (ask_sum > target_combined_ + hedge_margin_ + kFloatTol) {
        score = std::max(score, clamp01((ask_sum - target_combined_) / 0.08));
    }
    return score;
}

std::string RegimeGate::state_to_string(RegimeState s) {
    switch (s) {
    case RegimeState::RIDE: return "RIDE";
    case RegimeState::PAUSE_LEG1: return "PAUSE_LEG1";
    default: return "OK";
    }
}

RegimeDecision RegimeGate::evaluate(
    const MarketInfo& market, double now_sec, double yes_ask, double no_ask) {
    RegimeDecision d;
    if (!enabled_) {
        last_ = d;
        return d;
    }

    reload_pre_file(now_sec);

    const double window_total_sec = market.window_minutes * 60.0;
    const double secs_left = market.end_date_ts - now_sec;
    d.score_b = score_volatility(market, window_total_sec, secs_left);
    d.score_c = score_book(market, yes_ask, no_ask);

    std::optional<double> spot_bps;
    if (store_ && secs_left > kFloatTol) {
        const double elapsed = window_total_sec - secs_left;
        if (auto open_px = store_->get_price_at(market.asset, elapsed)) {
            const PriceTick latest = store_->get_latest_price(market.asset);
            if (latest.price > kFloatTol && *open_px > kFloatTol) {
                spot_bps = (latest.price - *open_px) / *open_px * 10000.0;
            }
        }
    }

    const bool pause_live = d.score_b + kFloatTol >= thresh_b_
        && d.score_c + kFloatTol >= thresh_c_;
    const bool spot_ok = spot_bps && std::abs(*spot_bps) + kFloatTol >= min_spot_bps_;
    const bool ride_live = d.score_b + kFloatTol >= thresh_b_
        && d.score_c + kFloatTol < thresh_c_ && spot_ok;

    maybe_extend_pre(now_sec, pause_live);

    d.pre_active = pre_active_ && now_sec + kFloatTol < pre_until_sec_;
    d.pre_reason = pre_reason_;

    if (d.pre_active && !ride_live) {
        d.state = RegimeState::PAUSE_LEG1;
        d.leg1_allowed = false;
        d.reason = "regime pre pause — " + (pre_reason_.empty() ? "news/calendar" : pre_reason_);
    } else if (pause_live) {
        d.state = RegimeState::PAUSE_LEG1;
        d.leg1_allowed = false;
        d.reason = "regime pause — B+C chaos";
    } else if (ride_live || (d.pre_active && ride_live)) {
        d.state = RegimeState::RIDE;
        d.leg1_allowed = true;
        d.force_spot_leg1 = true;
        d.reason = "regime ride — spot follow";
    } else {
        d.state = RegimeState::OK;
        d.leg1_allowed = true;
        d.reason = "regime ok";
    }

    static RegimeState last_state = RegimeState::OK;
    static double last_log_sec = 0.0;
    if (d.state != last_state && now_sec - last_log_sec >= 30.0) {
        spdlog::info(
            "[REGIME] {} | B={:.2f} C={:.2f} pre={} | {}",
            d.state_str(), d.score_b, d.score_c, d.pre_active, d.reason);
        last_state = d.state;
        last_log_sec = now_sec;
    }

    last_ = d;
    return d;
}

void RegimeGate::append_dashboard(boost::json::object& root) const {
    if (!enabled_) return;
    boost::json::object rg;
    rg["state"] = last_.state_str();
    rg["leg1_allowed"] = last_.leg1_allowed;
    rg["force_spot_leg1"] = last_.force_spot_leg1;
    rg["reason"] = last_.reason;
    rg["pre_active"] = last_.pre_active;
    if (!last_.pre_reason.empty()) rg["pre_reason"] = last_.pre_reason;
    rg["scores"] = boost::json::object{
        {"B", last_.score_b},
        {"C", last_.score_c},
    };
    if (pre_active_ && pre_until_sec_ > 0.0) {
        rg["pre_until_unix"] = pre_until_sec_;
    }
    rg["hedge_allowed"] = true;
    root["regime"] = std::move(rg);
}

} // namespace trading
