#include "LegInHedgeDetector.h"
#include "RegimeGate.h"
#include "../telemetry/ShadowWindowRecorder.h"
#include <boost/json.hpp>
#include <spdlog/spdlog.h>
#include <fmt/format.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <ctime>
#include <fstream>
#include <numeric>
#include <optional>
#include <sstream>

namespace trading {

namespace {
constexpr double kLegMinUsdc = 1.0;
constexpr double kFloatTol = 1e-6;
constexpr double kStatusLogIntervalSec = 30.0;
constexpr double kBalanceReserve = 0.995;
constexpr double kEntryDepthFillRatio = 0.90;
/** YES/NO within this gap = balanced; skip one-sided hedge (paired scale/dilute only). */
constexpr double kLihBalancedGapShares = 0.5;

std::string utc_day_string() {
    const time_t t = time(nullptr);
    tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &t);
#else
    gmtime_r(&t, &utc);
#endif
    return fmt::format("{:04d}-{:02d}-{:02d}", utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday);
}

bool mm2_obs_skip_reason_blocks(const std::string& reason) {
    if (reason.empty() || reason == "traded") return false;
    if (reason == "too_late") return false;
    if (reason == "session_off") return false;  // hour gate handles session
    if (reason == "unknown" || reason == "no_snapshots") return false;
    return true;
}

bool mm2_flat_heuristic_skip(double yes_ask, double no_ask, double spot_bps, bool strong_spot,
                             double flat_max_spot_bps, double flat_max_ask_sum, bool skip_flat) {
    if (!skip_flat || strong_spot) return false;
    if (yes_ask <= kFloatTol || no_ask <= kFloatTol) return true;
    if (yes_ask <= 0.02 + kFloatTol || no_ask <= 0.02 + kFloatTol) return true;
    if (yes_ask >= 0.98 - kFloatTol || no_ask >= 0.98 - kFloatTol) return true;
    const double ask_sum = yes_ask + no_ask;
    if (flat_max_ask_sum > kFloatTol && ask_sum > flat_max_ask_sum + kFloatTol) return true;
    // flat_book proxy: spot already moved too much (fitted upper band ~5.57bps)
    if (flat_max_spot_bps > kFloatTol && std::abs(spot_bps) > flat_max_spot_bps + kFloatTol) {
        return true;
    }
    const double lo = std::min(yes_ask, no_ask);
    const double hi = std::max(yes_ask, no_ask);
    if (lo >= 0.38 - kFloatTol && hi <= 0.62 + kFloatTol) return true;
    return false;
}

double spot_bps_pop_stdev(const std::vector<double>& samples) {
    if (samples.size() < 2) return 0.0;
    const double mean = std::accumulate(samples.begin(), samples.end(), 0.0)
        / static_cast<double>(samples.size());
    double acc = 0.0;
    for (const double x : samples) {
        const double d = x - mean;
        acc += d * d;
    }
    return std::sqrt(acc / static_cast<double>(samples.size()));
}

int window_start_utc_hour(const MarketInfo& market, double window_total_sec) {
    const time_t t = static_cast<time_t>(market.end_date_ts - window_total_sec);
    tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &t);
#else
    gmtime_r(&t, &utc);
#endif
    return utc.tm_hour;
}

int window_start_bj_hour(const MarketInfo& market, double window_total_sec) {
    return (window_start_utc_hour(market, window_total_sec) + 8) % 24;
}
} // namespace

bool LegInHedgeDetector::load_mm2_session_file(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        spdlog::warn("mm2 session file missing: {}", path);
        return false;
    }
    try {
        const auto val = boost::json::parse(in);
        if (!val.is_object()) return false;
        const auto& o = val.as_object();
        const auto* arr = o.if_contains("active_hours_utc");
        if (!arr || !arr->is_array()) return false;
        mm2_session_hours_.fill(false);
        mm2_session_use_hour_list_ = false;
        for (const auto& item : arr->as_array()) {
            int h = -1;
            if (item.is_int64()) {
                h = static_cast<int>(item.as_int64());
            } else if (item.is_uint64()) {
                h = static_cast<int>(item.as_uint64());
            } else if (item.is_double()) {
                h = static_cast<int>(item.as_double());
            }
            if (h >= 0 && h < 24) {
                mm2_session_hours_[static_cast<size_t>(h)] = true;
                mm2_session_use_hour_list_ = true;
            }
        }
        if (mm2_session_use_hour_list_) {
            std::string hours;
            for (int h = 0; h < 24; ++h) {
                if (mm2_session_hours_[static_cast<size_t>(h)]) {
                    if (!hours.empty()) hours += ',';
                    hours += std::to_string(h);
                }
            }
            spdlog::info("mm2 session hours loaded from {}: [{}]", path, hours);
        }
        return mm2_session_use_hour_list_;
    } catch (const std::exception& e) {
        spdlog::warn("mm2 session file parse failed {}: {}", path, e.what());
        return false;
    }
}

bool LegInHedgeDetector::load_mm2_session_from_pack(const std::string& m2_root) {
    if (m2_root.empty()) return false;
    const std::string path = m2_root + "/" + utc_day_string() + "/windows.jsonl";
    std::ifstream in(path);
    if (!in) {
        spdlog::warn("mm2 session pack missing: {}", path);
        return false;
    }
    mm2_session_hours_.fill(false);
    mm2_session_use_hour_list_ = false;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] != '{') continue;
        try {
            const auto val = boost::json::parse(line);
            if (!val.is_object()) continue;
            const auto& o = val.as_object();
            const auto* active = o.if_contains("session_active");
            if (!active || !active->is_bool() || !active->as_bool()) continue;
            const auto* ts = o.if_contains("window_start_ts");
            if (!ts || !(ts->is_int64() || ts->is_uint64() || ts->is_double())) continue;
            time_t wts = 0;
            if (ts->is_int64()) wts = static_cast<time_t>(ts->as_int64());
            else if (ts->is_uint64()) wts = static_cast<time_t>(ts->as_uint64());
            else wts = static_cast<time_t>(ts->as_double());
            tm utc{};
#ifdef _WIN32
            gmtime_s(&utc, &wts);
#else
            gmtime_r(&wts, &utc);
#endif
            const int h = utc.tm_hour;
            if (h >= 0 && h < 24) {
                mm2_session_hours_[static_cast<size_t>(h)] = true;
                mm2_session_use_hour_list_ = true;
            }
        } catch (const std::exception&) {
            continue;
        }
    }
    if (mm2_session_use_hour_list_) {
        std::string hours;
        for (int h = 0; h < 24; ++h) {
            if (mm2_session_hours_[static_cast<size_t>(h)]) {
                if (!hours.empty()) hours += ',';
                hours += std::to_string(h);
            }
        }
        spdlog::info("mm2 session hours from pack {}: [{}]", path, hours);
    }
    return mm2_session_use_hour_list_;
}

bool LegInHedgeDetector::mm2_session_hour_allowed(int hour_utc) const {
    if (mm2_session_use_hour_list_) {
        return hour_utc >= 0 && hour_utc < 24
            && mm2_session_hours_[static_cast<size_t>(hour_utc)];
    }
    if (mm2_session_utc_end_ >= 0) {
        return hour_utc >= mm2_session_utc_start_ && hour_utc < mm2_session_utc_end_;
    }
    return true;
}

bool LegInHedgeDetector::load_mm2_obs_skip_pack(const std::string& m2_root) {
    if (m2_root.empty()) return false;
    const std::string path = m2_root + "/" + utc_day_string() + "/windows.jsonl";
    std::ifstream in(path);
    if (!in) {
        spdlog::warn("mm2 obs skip pack missing: {}", path);
        return false;
    }
    std::unordered_map<int64_t, std::string> next;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] != '{') continue;
        try {
            const auto val = boost::json::parse(line);
            if (!val.is_object()) continue;
            const auto& o = val.as_object();
            const auto* ts = o.if_contains("window_start_ts");
            if (!ts || !(ts->is_int64() || ts->is_uint64() || ts->is_double())) continue;
            int64_t wts = 0;
            if (ts->is_int64()) wts = ts->as_int64();
            else if (ts->is_uint64()) wts = static_cast<int64_t>(ts->as_uint64());
            else wts = static_cast<int64_t>(ts->as_double());

            bool traded = false;
            if (const auto* v = o.if_contains("account_traded")) {
                if (v->is_bool()) traded = v->as_bool();
            }
            if (!traded) {
                if (const auto* v = o.if_contains("mm2_traded")) {
                    if (v->is_bool()) traded = v->as_bool();
                }
            }
            std::string skip;
            if (const auto* v = o.if_contains("skip_reason")) {
                if (v->is_string()) skip = v->as_string().c_str();
            }
            if (traded || skip == "traded") {
                next.erase(wts);
                continue;
            }
            if (mm2_obs_skip_reason_blocks(skip)) {
                next[wts] = skip;
            } else {
                next.erase(wts);
            }
        } catch (const std::exception&) {
            continue;
        }
    }
    mm2_obs_skip_by_wts_ = std::move(next);
    spdlog::info("mm2 obs skip index loaded from {}: {} blocked windows", path,
                 mm2_obs_skip_by_wts_.size());
    return true;
}

bool LegInHedgeDetector::mm2_obs_skips_leg1(int64_t window_start_ts) const {
    if (!mm2_obs_skip_from_pack_) return false;
    const auto it = mm2_obs_skip_by_wts_.find(window_start_ts);
    if (it == mm2_obs_skip_by_wts_.end()) return false;
    return mm2_obs_skip_reason_blocks(it->second);
}

namespace {

std::optional<bool> parse_mm2_leg1_buy_yes(const std::string& side) {
    if (side.empty()) return std::nullopt;
    std::string s = side;
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (s == "up" || s == "yes") return true;
    if (s == "down" || s == "no") return false;
    return std::nullopt;
}

int64_t json_int64_field(const boost::json::object& o, const char* key) {
    const auto* v = o.if_contains(key);
    if (!v) return 0;
    if (v->is_int64()) return v->as_int64();
    if (v->is_uint64()) return static_cast<int64_t>(v->as_uint64());
    if (v->is_double()) return static_cast<int64_t>(v->as_double());
    return 0;
}

double json_double_field(const boost::json::object& o, const char* key, double def = 0.0) {
    const auto* v = o.if_contains(key);
    if (!v) return def;
    if (v->is_double()) return v->as_double();
    if (v->is_int64()) return static_cast<double>(v->as_int64());
    if (v->is_uint64()) return static_cast<double>(v->as_uint64());
    return def;
}

} // namespace

bool LegInHedgeDetector::load_mm2_replay_leg1_pack(const std::string& m2_root) {
    if (m2_root.empty()) return false;
    const std::string day = utc_day_string();
    const std::string win_path = m2_root + "/" + day + "/windows.jsonl";
    const std::string fill_path = m2_root + "/" + day + "/fills.jsonl";
    std::unordered_map<int64_t, Mm2ReplayCue> next;

    auto ingest_cue = [&](int64_t wts, bool buy_yes, double sec_in, double shares) {
        if (wts <= 0 || sec_in < 0.0) return;
        Mm2ReplayCue cue;
        cue.buy_yes = buy_yes;
        cue.sec_in = sec_in;
        cue.shares = shares > 0.0 ? shares : 0.0;
        auto it = next.find(wts);
        // Prefer earliest first-fill cue (smaller sec_in).
        if (it == next.end() || cue.sec_in + kFloatTol < it->second.sec_in) {
            next[wts] = cue;
        }
    };

    {
        std::ifstream in(win_path);
        if (in) {
            std::string line;
            while (std::getline(in, line)) {
                if (line.empty() || line[0] != '{') continue;
                try {
                    const auto val = boost::json::parse(line);
                    if (!val.is_object()) continue;
                    const auto& o = val.as_object();
                    bool traded = false;
                    if (const auto* v = o.if_contains("account_traded"); v && v->is_bool()) {
                        traded = v->as_bool();
                    }
                    if (!traded) {
                        if (const auto* v = o.if_contains("mm2_traded"); v && v->is_bool()) {
                            traded = v->as_bool();
                        }
                    }
                    if (!traded) continue;
                    const int64_t wts = json_int64_field(o, "window_start_ts");
                    std::string side;
                    if (const auto* v = o.if_contains("leg1_side"); v && v->is_string()) {
                        side = v->as_string().c_str();
                    }
                    const auto buy = parse_mm2_leg1_buy_yes(side);
                    if (!buy) continue;
                    ingest_cue(wts, *buy, json_double_field(o, "leg1_sec_in"),
                               json_double_field(o, "leg1_shares"));
                } catch (const std::exception&) {
                    continue;
                }
            }
        } else {
            spdlog::warn("mm2 replay windows pack missing: {}", win_path);
        }
    }

    {
        std::ifstream in(fill_path);
        if (in) {
            std::string line;
            while (std::getline(in, line)) {
                if (line.empty() || line[0] != '{') continue;
                try {
                    const auto val = boost::json::parse(line);
                    if (!val.is_object()) continue;
                    const auto& o = val.as_object();
                    bool first = false;
                    if (const auto* v = o.if_contains("is_first_fill"); v && v->is_bool()) {
                        first = v->as_bool();
                    }
                    if (!first) continue;
                    const int64_t wts = json_int64_field(o, "window_start_ts");
                    std::string side;
                    if (const auto* v = o.if_contains("side"); v && v->is_string()) {
                        side = v->as_string().c_str();
                    }
                    const auto buy = parse_mm2_leg1_buy_yes(side);
                    if (!buy) continue;
                    ingest_cue(wts, *buy, json_double_field(o, "sec_in"),
                               json_double_field(o, "shares"));
                } catch (const std::exception&) {
                    continue;
                }
            }
        }
    }

    mm2_replay_leg1_by_wts_ = std::move(next);
    spdlog::info("mm2 replay leg1 cues loaded from {} / {}: {} windows", win_path, fill_path,
                 mm2_replay_leg1_by_wts_.size());
    return !mm2_replay_leg1_by_wts_.empty();
}

LegInHedgeDetector::LegInHedgeDetector(StateStore& store,
                                       std::vector<MarketInfo> markets,
                                       double leg1_max_price,
                                       double target_combined,
                                       double min_seconds_remaining,
                                       double leg1_min_seconds_remaining,
                                       double leg1_start_delay_sec,
                                       double leg1_cooldown_seconds,
                                       double rebalance_cooldown_seconds,
                                       bool use_mirror_prices,
                                       double leg1_shares,
                                       bool allow_over_target,
                                       double force_balance_secs,
                                       double max_rebalance_shares,
                                       bool flex_rebalance,
                                       double flex_dilute_ratio,
                                       bool leg1_trend_align,
                                       double trend_lookback_sec,
                                       bool leg1_trend_mode,
                                       double leg1_trend_max_price,
                                       bool leg1_trigger_mode,
                                       double leg1_trigger_min,
                                       double leg1_trigger_max,
                                       double endgame_secs,
                                       double endgame_hold_ask,
                                       double endgame_resume_hedge_ask,
                                       double endgame_soft_cap,
                                       double endgame_step_small,
                                       double endgame_step_large,
                                       double endgame_gap_large,
                                       double endgame_override_secs,
                                       double endgame_override_cooldown,
                                       bool endgame_minimize_gap,
                                       bool endgame_ladder_enabled,
                                       double endgame_ladder_secs,
                                       double endgame_ladder_start,
                                       double endgame_ladder_end,
                                       double endgame_ladder_step,
                                       double max_entry_marginal,
                                       double mid_soft_cap,
                                       double mid_soft_start_secs,
                                       bool hedge_feasible_entry,
                                       double hedge_feasible_cap)
    : store_(store),
      markets_(std::move(markets)),
      leg1_max_price_(leg1_max_price),
      target_combined_(target_combined),
      min_seconds_remaining_(min_seconds_remaining),
      leg1_min_seconds_remaining_(leg1_min_seconds_remaining),
      leg1_start_delay_sec_(leg1_start_delay_sec),
      leg1_cooldown_seconds_(leg1_cooldown_seconds),
      rebalance_cooldown_seconds_(rebalance_cooldown_seconds),
      use_mirror_prices_(use_mirror_prices),
      leg1_shares_(leg1_shares),
      leg1_clip_shares_(0.0),
      allow_over_target_(allow_over_target),
      force_balance_secs_(force_balance_secs),
      max_rebalance_shares_(max_rebalance_shares),
      flex_rebalance_(flex_rebalance),
      flex_dilute_ratio_(flex_dilute_ratio),
      leg1_trend_align_(leg1_trend_align),
      trend_lookback_sec_(trend_lookback_sec),
      leg1_trend_mode_(leg1_trend_mode),
      leg1_trend_max_price_(leg1_trend_max_price),
      leg1_trigger_mode_(leg1_trigger_mode),
      leg1_trigger_min_(leg1_trigger_min),
      leg1_trigger_max_(leg1_trigger_max),
      endgame_secs_(endgame_secs),
      endgame_hold_ask_(endgame_hold_ask),
      endgame_resume_hedge_ask_(endgame_resume_hedge_ask),
      endgame_soft_cap_(endgame_soft_cap),
      endgame_step_small_(endgame_step_small),
      endgame_step_large_(endgame_step_large),
      endgame_gap_large_(endgame_gap_large),
      endgame_override_secs_(endgame_override_secs),
      endgame_override_cooldown_(endgame_override_cooldown),
      endgame_minimize_gap_(endgame_minimize_gap),
      endgame_ladder_enabled_(endgame_ladder_enabled),
      endgame_ladder_secs_(endgame_ladder_secs),
      endgame_ladder_start_(endgame_ladder_start),
      endgame_ladder_end_(endgame_ladder_end),
      endgame_ladder_step_(endgame_ladder_step),
      max_entry_marginal_(max_entry_marginal),
      mid_soft_cap_(mid_soft_cap),
      mid_soft_start_secs_(mid_soft_start_secs),
      hedge_feasible_entry_(hedge_feasible_entry),
      hedge_feasible_cap_(hedge_feasible_cap),
      vwap_entry_gate_(false),
      vwap_entry_cap_(0.0),
      vwap_depth_ratio_(kEntryDepthFillRatio),
      min_edge_usdc_(0.0),
      min_edge_per_share_(0.05),
      unwind_enabled_(true),
      unwind_secs_(120.0),
      unwind_cooldown_(10.0) {}

bool LegInHedgeDetector::spot_trend_favors(const MarketInfo& market, bool pick_yes) const {
    const std::string asset = market.asset;
    if (asset.empty()) return false;

    const auto past = store_.get_price_at(asset, trend_lookback_sec_);
    const PriceTick latest = store_.get_latest_price(asset);
    if (!past || latest.price <= kFloatTol) return false;

    const double move = latest.price - *past;
    if (pick_yes) return move >= -kFloatTol;
    return move <= kFloatTol;
}

std::optional<double> LegInHedgeDetector::window_spot_ret_bps(
    const MarketInfo& market, double window_total_sec, double secs_left) const {
    const std::string& asset = market.asset;
    if (asset.empty()) return std::nullopt;
    const double elapsed = window_total_sec - secs_left;
    if (elapsed <= kFloatTol) return std::nullopt;
    const auto open_px = store_.get_price_at(asset, elapsed);
    const PriceTick latest = store_.get_latest_price(asset);
    if (!open_px || latest.price <= kFloatTol || *open_px <= kFloatTol) return std::nullopt;
    return (latest.price - *open_px) / *open_px * 10000.0;
}

std::optional<double> LegInHedgeDetector::window_spot_ret_bps_ago(
    const MarketInfo& market, double window_total_sec, double secs_left,
    double lookback_sec) const {
    const std::string& asset = market.asset;
    if (asset.empty() || lookback_sec <= kFloatTol) return std::nullopt;
    const double elapsed = window_total_sec - secs_left;
    if (elapsed <= lookback_sec + kFloatTol) return std::nullopt;
    const auto open_px = store_.get_price_at(asset, elapsed);
    const auto past_px = store_.get_price_at(asset, lookback_sec);
    if (!open_px || !past_px || *open_px <= kFloatTol || *past_px <= kFloatTol) {
        return std::nullopt;
    }
    return (*past_px - *open_px) / *open_px * 10000.0;
}

void LegInHedgeDetector::mm2_touch_spot_track(
    const std::string& key, double elapsed, double spot_bps) {
    auto& tr = mm2_spot_tracks_[key];
    static constexpr double kMarks[] = {120.0, 180.0, 240.0};
    for (size_t i = 0; i < 3; ++i) {
        const uint8_t bit = static_cast<uint8_t>(1u << i);
        if ((tr.milestones & bit) != 0) continue;
        if (elapsed + kFloatTol < kMarks[i]) continue;
        tr.milestones |= bit;
        tr.bps.push_back(spot_bps);
    }
}

std::optional<std::string> LegInHedgeDetector::mm2_vol_gate_reason(
    double spot_bps, const std::vector<double>& samples) const {
    if (!mm2_vol_gate_) return std::nullopt;
    const double abs_spot = std::abs(spot_bps);
    if (mm2_vol_min_spot_bps_ > kFloatTol
        && abs_spot < mm2_vol_min_spot_bps_ - kFloatTol) {
        return "mm2 vol low";
    }
    if (mm2_vol_max_spot_bps_ > kFloatTol
        && abs_spot > mm2_vol_max_spot_bps_ + kFloatTol) {
        return "mm2 vol high";
    }
    if (mm2_vol_max_spot_std_ > kFloatTol && samples.size() >= 2) {
        const double sd = spot_bps_pop_stdev(samples);
        if (sd > mm2_vol_max_spot_std_ + kFloatTol) return "mm2 choppy";
    }
    return std::nullopt;
}

bool LegInHedgeDetector::mm2_spot_favors_heavy(
    const MarketInfo& market, bool heavy_yes,
    double window_total_sec, double secs_left) const {
    const auto bps = window_spot_ret_bps(market, window_total_sec, secs_left);
    if (!bps || std::abs(*bps) < mm2_min_spot_bps_ - kFloatTol) return false;
    const bool spot_up = *bps > kFloatTol;
    return heavy_yes ? spot_up : !spot_up;
}

bool LegInHedgeDetector::mm2_spot_against_heavy(
    const MarketInfo& market, bool heavy_yes,
    double window_total_sec, double secs_left,
    double thr_bps) const {
    const double thr = thr_bps >= 0.0 ? thr_bps : mm2_min_spot_bps_;
    if (thr <= kFloatTol) return false;
    const auto bps = window_spot_ret_bps(market, window_total_sec, secs_left);
    if (!bps || std::abs(*bps) < thr - kFloatTol) return false;
    const bool spot_up = *bps > kFloatTol;
    return heavy_yes ? !spot_up : spot_up;
}

double LegInHedgeDetector::mm2_heavy_scale_cap() const {
    if (mm2_mode_ && mm2_heavy_max_shares_ > kFloatTol) {
        return std::max(mm2_heavy_max_shares_, leg1_shares_);
    }
    return leg1_shares_;
}

double LegInHedgeDetector::opposite_rest_depth(const std::string& token_id) const {
    const auto det = store_.get_detection_ask(token_id);
    if (!det || !det->rest_ok || det->rest_depth_shares <= kFloatTol) return 0.0;
    return det->rest_depth_shares;
}

double LegInHedgeDetector::shrink_for_rest_book(
    const std::string& token_id, double want_shares) const {
    if (want_shares <= kFloatTol) return 0.0;
    const auto wf = store_.walk_ask_fill(token_id, want_shares);
    if (wf.shares <= kFloatTol) return 0.0;
    return wf.shares;
}

double LegInHedgeDetector::light_clip_shares() const {
    return leg1_clip_shares_ > kFloatTol ? leg1_clip_shares_ : kClobMinOrderShares;
}

double LegInHedgeDetector::heavy_clip_cap() const {
    return heavy_clip_shares_ > kFloatTol ? heavy_clip_shares_ : leg1_shares_;
}

bool LegInHedgeDetector::passes_hedge_feasible_entry(
    const MarketInfo& market, bool pick_yes, double leg1_px, double leg1_shares,
    double opposite_px) const {
    if (!hedge_feasible_entry_ || leg1_px <= kFloatTol) return true;
    const double cap = hedge_feasible_cap_ > target_combined_ + kFloatTol
        ? hedge_feasible_cap_
        : (mid_soft_cap_ > target_combined_ + kFloatTol ? mid_soft_cap_ : 0.98);
    const double max_opp = cap - leg1_px;
    if (max_opp <= kFloatTol || opposite_px > max_opp + kFloatTol) return false;
    const auto& opp_tok = pick_yes ? market.no_token_id : market.yes_token_id;
    const double depth = opposite_rest_depth(opp_tok);
    if (depth + kFloatTol < leg1_shares * kEntryDepthFillRatio) return false;
    return true;
}

LegInHedgeDetector::VwapEntryEval LegInHedgeDetector::eval_vwap_entry(
    const MarketInfo& market, double want_sh, double fee_rate) const {
    VwapEntryEval r;
    if (!vwap_entry_gate_ || want_sh <= kFloatTol) {
        r.ok = true;
        r.paired_sh = want_sh;
        return r;
    }

    const auto yf = store_.walk_ask_fill(market.yes_token_id, want_sh);
    const auto nf = store_.walk_ask_fill(market.no_token_id, want_sh);
    r.paired_sh = std::min({yf.shares, nf.shares, want_sh});
    if (r.paired_sh + kFloatTol < want_sh * vwap_depth_ratio_) {
        return r;
    }

    const auto y2 = store_.walk_ask_fill(market.yes_token_id, r.paired_sh);
    const auto n2 = store_.walk_ask_fill(market.no_token_id, r.paired_sh);
    r.paired_sh = std::min(y2.shares, n2.shares);
    if (r.paired_sh + kFloatTol < want_sh * vwap_depth_ratio_) {
        return r;
    }
    if (y2.avg_price <= kFloatTol || n2.avg_price <= kFloatTol) {
        return r;
    }

    r.yes_vwap = y2.avg_price;
    r.no_vwap = n2.avg_price;
    r.comb = y2.avg_price + n2.avg_price;

    const double cap = vwap_entry_cap_ > target_combined_ + kFloatTol
        ? vwap_entry_cap_
        : (mid_soft_cap_ > target_combined_ + kFloatTol ? mid_soft_cap_ : target_combined_);
    if (r.comb > cap + kFloatTol) {
        return r;
    }

    const double gross = r.paired_sh * (1.0 - r.comb);
    const double fees = fee_rate * (y2.cost_usdc + n2.cost_usdc);
    r.net_edge = gross - fees;
    r.min_edge = min_edge_usdc_ > kFloatTol
        ? min_edge_usdc_
        : min_edge_per_share_ * r.paired_sh;
    if (r.net_edge + kFloatTol < r.min_edge) {
        return r;
    }

    r.ok = true;
    return r;
}

bool LegInHedgeDetector::leg1_trend_allows(const MarketInfo& market, bool pick_yes) const {
    if (!leg1_trend_align_) return true;
    return spot_trend_favors(market, pick_yes);
}

LegInHedgeDetector::Quote LegInHedgeDetector::quote_for(const MarketInfo& market) const {
    Quote q;
    auto side_ask = [&](const std::optional<StateStore::DetectionAsk>& det) -> double {
        if (!det) return 0.0;
        if (store_.lih_quote_rest_only()) {
            return det->rest_ok ? det->rest_book_ask : 0.0;
        }
        if (store_.book_aware_detect()) {
            if (det->conservative_ask > kFloatTol) return det->conservative_ask;
            if (det->rest_ok) return det->rest_book_ask;
            if (det->ws_ok) return det->ws_book_ask;
            return 0.0;
        }
        if (det->rest_ok) return det->rest_book_ask;
        if (det->ws_ok) return det->ws_book_ask;
        return 0.0;
    };

    if (store_.book_aware_detect()) {
        const auto yes_det = store_.get_detection_ask(market.yes_token_id);
        const auto no_det = store_.get_detection_ask(market.no_token_id);
        q.yes = side_ask(yes_det);
        q.no = side_ask(no_det);
        if (q.yes > kFloatTol && q.no > kFloatTol) return q;
    }
    if (store_.paper_official_book()) {
        auto yes = store_.get_official_buy_ask(market.yes_token_id);
        auto no = store_.get_official_buy_ask(market.no_token_id);
        if (yes && no && *yes > 0 && *no > 0) {
            q.yes = *yes;
            q.no = *no;
            return q;
        }
    }
    if (use_mirror_prices_) {
        auto mir = store_.get_mirror_quote(market.asset);
        if (mir && mir->fresh) {
            q.yes = mir->book_yes > 0 ? mir->book_yes : mir->ws_yes;
            q.no = mir->book_no > 0 ? mir->book_no : mir->ws_no;
            q.from_mirror = true;
            if (q.yes > 0 && q.no > 0) return q;
        }
    }
    auto yes = store_.get_token_price(market.yes_token_id);
    auto no = store_.get_token_price(market.no_token_id);
    if (yes && yes->side == "BUY") q.yes = yes->price;
    if (no && no->side == "BUY") q.no = no->price;
    return q;
}

LegInHedgeDetector::Quote LegInHedgeDetector::hedge_quote_for(const MarketInfo& market) const {
    Quote q;
    constexpr double kHedgeRestMaxAgeSec = 5.0;
    auto hedge_side = [&](const std::optional<StateStore::DetectionAsk>& det) -> double {
        if (!det) return 0.0;
        if (store_.lih_quote_rest_only()) {
            return det->rest_ok ? det->rest_book_ask : 0.0;
        }
        if (!store_.is_paper_mode() && !det->rest_ok) return 0.0;
        if (det->conservative_ask > kFloatTol) return det->conservative_ask;
        if (det->rest_ok) return det->rest_book_ask;
        if (store_.is_paper_mode() && det->ws_ok) return det->ws_book_ask;
        return 0.0;
    };

    if (store_.book_aware_detect()) {
        const auto yes_det = store_.get_detection_ask(market.yes_token_id, kHedgeRestMaxAgeSec);
        const auto no_det = store_.get_detection_ask(market.no_token_id, kHedgeRestMaxAgeSec);
        q.yes = hedge_side(yes_det);
        q.no = hedge_side(no_det);
        if (store_.lih_quote_rest_only()) {
            if (q.yes > kFloatTol || q.no > kFloatTol) return q;
        } else if (q.yes > kFloatTol && q.no > kFloatTol) {
            return q;
        }
    }
    return quote_for(market);
}

double LegInHedgeDetector::cap_shares_budget(double shares, double max_usdc, double unit_cost) const {
    if (shares <= kFloatTol || unit_cost <= kFloatTol) return 0.0;
    double capped = shares;
    if (max_rebalance_shares_ > kFloatTol) {
        capped = std::min(capped, max_rebalance_shares_);
    }
    if (max_usdc > kFloatTol) {
        capped = std::min(capped, max_usdc / unit_cost);
    }
    return capped;
}

double LegInHedgeDetector::cap_shares(double shares, double balance, double unit_cost) const {
    return cap_shares_budget(shares, balance * kBalanceReserve, unit_cost);
}

double LegInHedgeDetector::hedge_fill_shares(
    const std::string& token_id, double gap, double px,
    double max_usdc, double max_matched_shares) const {
    if (gap <= kFloatTol || px <= kFloatTol) return 0.0;
    double fill = cap_shares_budget(std::min(gap, max_matched_shares), max_usdc, px);
    fill = shrink_for_rest_book(token_id, fill);
    if (fill * px + kFloatTol < kLegMinUsdc) return 0.0;
    return fill;
}

double LegInHedgeDetector::paired_fill_shares(
    const MarketInfo& market, double yes_p, double no_p,
    double max_usdc, double max_matched_shares) const {
    const double combined = yes_p + no_p;
    if (combined <= kFloatTol || max_matched_shares <= kFloatTol) return 0.0;
    double fill = cap_shares_budget(max_matched_shares, max_usdc, combined);
    if (store_.paper_depth_sim()) {
        fill = store_.walk_paired_fill(
            market.yes_token_id, market.no_token_id, fill, max_usdc, 1.0).shares;
    }
    if (fill * yes_p + kFloatTol < kLegMinUsdc) return 0.0;
    if (fill * no_p + kFloatTol < kLegMinUsdc) return 0.0;
    return fill;
}

void LegInHedgeDetector::log_rebalance_status(
    const MarketInfo& market, const std::string& key, double now_sec,
    const risk::LegInHedgePosition& pos, const Quote& q,
    double yes_avg, double no_avg, double gap) const {
    auto it = last_status_log_sec_.find(key);
    if (it != last_status_log_sec_.end() &&
        (now_sec - it->second) < kStatusLogIntervalSec) {
        return;
    }
    last_status_log_sec_[key] = now_sec;

    const double matched = std::min(pos.yes_shares, pos.no_shares);
    const double port_avg = (yes_avg > kFloatTol && no_avg > kFloatTol) ? yes_avg + no_avg : 0.0;
    const bool need_yes = pos.yes_shares < pos.no_shares;
    const double short_px = need_yes ? q.yes : q.no;
    const double long_avg = need_yes ? no_avg : yes_avg;
    const double marginal = long_avg + short_px;

    std::string msg = fmt::format(
        "[LIH DEBUG] rebalance | {} {}m | YES {:.2f}@{:.4f} NO {:.2f}@{:.4f} | matched {:.1f} gap {:.1f} | "
        "book {:.4f}/{:.4f} sum {:.4f} | port_avg {:.4f} target {:.2f} | marginal {:.4f} | {:.0f}s left",
        market.asset, market.window_minutes,
        pos.yes_shares, yes_avg, pos.no_shares, no_avg,
        matched, gap, q.yes, q.no, q.yes + q.no,
        port_avg, target_combined_, marginal,
        market.end_date_ts - now_sec);
    spdlog::info(msg);
    store_.push_telemetry(msg);
}

void LegInHedgeDetector::log_entry_status(
    const MarketInfo& market, const std::string& key, double now_sec,
    const Quote& q, const char* reason) const {
    if (shadow_window_recorder_) {
        shadow_window_recorder_->record_skip(market, now_sec, reason, q.yes, q.no);
    }
    auto it = last_entry_log_sec_.find(key);
    if (it != last_entry_log_sec_.end() &&
        (now_sec - it->second) < kStatusLogIntervalSec) {
        return;
    }
    last_entry_log_sec_[key] = now_sec;

    const double secs_left = market.end_date_ts - now_sec;
    std::string msg = fmt::format(
        "[LIH DEBUG] entry-wait | {} {}m | book {:.4f}/{:.4f} sum {:.4f} | leg1<={:.2f} | {:.0f}s left | {}",
        market.asset, market.window_minutes, q.yes, q.no, q.yes + q.no,
        leg1_max_price_, secs_left, reason);
    spdlog::info(msg);
    store_.push_telemetry(msg);
}

double LegInHedgeDetector::endgame_ladder_max_marginal(double secs_left) const {
    if (!endgame_ladder_enabled_ || endgame_ladder_secs_ <= kFloatTol) {
        return endgame_soft_cap_;
    }
    if (secs_left > endgame_ladder_secs_ + kFloatTol) {
        return target_combined_;
    }
    const double step = endgame_ladder_step_ > kFloatTol ? endgame_ladder_step_ : 0.01;
    const double start = std::max(endgame_ladder_start_, target_combined_ + step);
    const double end = std::min(endgame_ladder_end_, endgame_soft_cap_);
    if (end <= target_combined_ + kFloatTol) return target_combined_;

    const double elapsed = endgame_ladder_secs_ - secs_left;
    const double progress = std::clamp(elapsed / endgame_ladder_secs_, 0.0, 1.0);
    const int n_steps = static_cast<int>(std::round((end - start) / step));
    const int tier = static_cast<int>(
        std::floor(progress * static_cast<double>(n_steps + 1)));
    const int capped_tier = std::min(tier, n_steps);
    return std::min(start + capped_tier * step, end);
}

double LegInHedgeDetector::active_parallel_hedge_cap(double secs_left) const {
    // Upper bound only — no profit floor (comb 0.80 is taken; comb must be <= cap).
    const double start = parallel_hedge_max_combined_ > kFloatTol
        ? parallel_hedge_max_combined_ : 1.0;
    double cap = start;
    const double late_max = std::max(start, endgame_soft_cap_ > kFloatTol ? endgame_soft_cap_ : start);
    if (mid_soft_start_secs_ > kFloatTol && secs_left <= mid_soft_start_secs_ + kFloatTol
        && secs_left > endgame_secs_ + kFloatTol && late_max > start + kFloatTol) {
        const double span = mid_soft_start_secs_ - endgame_secs_;
        const double elapsed = mid_soft_start_secs_ - secs_left;
        if (span > kFloatTol) {
            const double t = std::clamp(elapsed / span, 0.0, 1.0);
            cap = start + t * (late_max - start);
        } else {
            cap = late_max;
        }
    }
    if (early_hedge_max_combined_ > kFloatTol) {
        cap = std::min(cap, early_hedge_max_combined_);
    }
    return cap;
}

double LegInHedgeDetector::gap_hedge_cap_for(double secs_left, double gap) const {
    const double base = active_parallel_hedge_cap(secs_left);
    if (max_gap_shares_ <= kFloatTol || gap <= max_gap_shares_ + kFloatTol) {
        return base;
    }
    const double relaxed = gap_hedge_max_combined_ > kFloatTol
        ? gap_hedge_max_combined_
        : (endgame_soft_cap_ > kFloatTol ? endgame_soft_cap_ : base);
    return std::max(base, relaxed);
}

std::optional<LegInAction> LegInHedgeDetector::evaluate(double now_ms, risk::RiskManager& rm) {
    const double now_sec = now_ms / 1000.0;
    rm.scrub_lih_inflight_locks(now_sec);
    const double max_leg_usdc = rm.get_max_leg_cost_usdc();
    const double max_matched_cap = rm.get_lih_max_matched_shares();
    std::optional<RegimeDecision> regime_once;

    for (const auto& market : markets_) {
        if (market.window_minutes == 5 && !store_.dh_enable_5m()) continue;
        if (market.window_minutes == 15 && !store_.dh_enable_15m()) continue;
        if (!store_.dh_asset_enabled(market.window_minutes, market.asset)) continue;

        const double secs_left = market.end_date_ts - now_sec;
        const double window_total_sec = market.window_minutes * 60.0;

        const std::string key = market.asset + "-" + std::to_string(market.window_minutes);

        auto open_lih = rm.find_open_lih_for_market(market);
        if (!open_lih) {
            // Only reuse by-asset when same gamma window (avoid hedging wrong tokens).
            auto sibling = rm.find_open_lih_by_asset(market.asset, market.window_minutes);
            if (sibling && std::abs(sibling->end_date_ts - market.end_date_ts) < 2.0) {
                open_lih = sibling;
            }
        }
        Quote q = quote_for(market);
        if (regime_gate_ && regime_gate_->enabled()
            && market.window_minutes == 5) {
            std::string asset_l = market.asset;
            std::transform(asset_l.begin(), asset_l.end(), asset_l.begin(), ::tolower);
            if (asset_l == "btc" && !regime_once && q.yes > kFloatTol && q.no > kFloatTol) {
                regime_once = regime_gate_->evaluate(market, now_sec, q.yes, q.no);
            }
        }
        if (shadow_window_recorder_) {
            shadow_window_recorder_->tick(market, now_sec, q.yes, q.no);
            if (regime_once) {
                shadow_window_recorder_->update_regime(market, now_sec, *regime_once);
            }
        }
        if (q.yes <= kFloatTol || q.no <= kFloatTol) {
            if (!open_lih) log_entry_status(market, key, now_sec, q, "no quote");
            continue;
        }

        if (!open_lih) {
            const double window_start = market.end_date_ts - window_total_sec;
            if (skip_partial_window_on_start_ && process_boot_sec_ > kFloatTol
                && window_start < process_boot_sec_ - kFloatTol) {
                log_entry_status(market, key, now_sec, q, "residual window — wait next round");
                continue;
            }
            const double elapsed = window_total_sec - secs_left;
            if (leg1_start_delay_sec_ > 0.0 && elapsed < leg1_start_delay_sec_) {
                log_entry_status(market, key, now_sec, q, "early window — wait volatility");
                continue;
            }
            if (secs_left < leg1_min_seconds_remaining_ && !mm2_mode_) {
                log_entry_status(market, key, now_sec, q, "late window — wait next round");
                continue;
            }
            if (leg1_cooldown_seconds_ > 0.0 &&
                last_leg1_time_.contains(key) &&
                (now_sec - last_leg1_time_.at(key)) < leg1_cooldown_seconds_) {
                log_entry_status(market, key, now_sec, q, "leg1 cooldown");
                continue;
            }
            if (rm.lih_has_open_or_inflight(market.asset, market.window_minutes)) {
                const char* busy = rm.lih_leg1_inflight_only(market.asset, market.window_minutes)
                    ? "leg1 in-flight" : "slot busy";
                log_entry_status(market, key, now_sec, q, busy);
                continue;
            }
            if (rm.lih_other_slot_busy(market.asset, market.window_minutes)) {
                log_entry_status(market, key, now_sec, q, "other slot active");
                continue;
            }
            if (regime_once && !regime_once->leg1_allowed) {
                log_entry_status(market, key, now_sec, q, regime_once->reason.c_str());
                continue;
            }
            // DEBUG single-round test: re-enable session leg cap gate (LIH_SESSION_MAX_LEGS).
            // if (rm.lih_session_leg1_blocked()) {
            //     log_entry_status(market, key, now_sec, q, "session leg cap");
            //     continue;
            // }

            if (mm2_mode_) {
                const int hour = window_start_utc_hour(market, window_total_sec);
                const int64_t window_start_ts =
                    static_cast<int64_t>(market.end_date_ts - window_total_sec);
                const double sec_in = std::max(0.0, window_total_sec - secs_left);
                if (const auto bps_track = window_spot_ret_bps(market, window_total_sec, secs_left)) {
                    mm2_touch_spot_track(key, sec_in, *bps_track);
                }

                // Replay: follow m2 leg1 cue (side + timing). Hedge still uses bot LIH.
                if (mm2_replay_leg1_) {
                    const auto it = mm2_replay_leg1_by_wts_.find(window_start_ts);
                    if (it == mm2_replay_leg1_by_wts_.end()) {
                        log_entry_status(market, key, now_sec, q, "mm2 replay no cue");
                        continue;
                    }
                    const auto& cue = it->second;
                    if (sec_in + kFloatTol < cue.sec_in - mm2_replay_lag_tol_sec_) {
                        log_entry_status(market, key, now_sec, q, "mm2 replay wait cue");
                        continue;
                    }
                    if (secs_left < mm2_entry_min_secs_left_ + kFloatTol) {
                        log_entry_status(market, key, now_sec, q, "mm2 replay too late");
                        continue;
                    }
                    const bool pick_yes_mm2 = cue.buy_yes;
                    const double px_mm2 = pick_yes_mm2 ? q.yes : q.no;
                    const double hmax = heavy_max_price_ > kFloatTol ? heavy_max_price_ : 0.75;
                    if (px_mm2 <= kFloatTol || px_mm2 > hmax + kFloatTol) {
                        log_entry_status(market, key, now_sec, q, "mm2 replay price band");
                        continue;
                    }
                    const auto& tok_mm2 = pick_yes_mm2 ? market.yes_token_id : market.no_token_id;
                    const double scale_clip = mm2_scale_clip_shares_ > kFloatTol
                        ? mm2_scale_clip_shares_ : light_clip_shares();
                    double shares_mm2 = std::min(scale_clip, leg1_shares_);
                    if (cue.shares > kFloatTol) {
                        shares_mm2 = std::min(shares_mm2, cue.shares);
                    }
                    shares_mm2 = cap_shares_budget(shares_mm2, max_leg_usdc, px_mm2);
                    shares_mm2 = shrink_for_rest_book(tok_mm2, shares_mm2);
                    if (shares_mm2 + kFloatTol < kClobMinOrderShares) {
                        log_entry_status(market, key, now_sec, q, "mm2 replay depth min");
                        continue;
                    }
                    if (shares_mm2 * px_mm2 + kFloatTol < kLegMinUsdc) {
                        log_entry_status(market, key, now_sec, q, "below min usdc");
                        continue;
                    }
                    const double cost_mm2 = shares_mm2 * px_mm2;
                    const auto [can_mm2, why_mm2] = rm.can_open_lih_leg(
                        cost_mm2, false, nullptr, 0.0, &market.asset, market.window_minutes);
                    if (!can_mm2) {
                        log_entry_status(market, key, now_sec, q, why_mm2.c_str());
                        continue;
                    }
                    LegInAction act;
                    act.kind = LegInAction::Kind::OpenLeg1;
                    act.market = market;
                    act.buy_yes = pick_yes_mm2;
                    act.price = px_mm2;
                    act.shares = shares_mm2;
                    act.note = fmt::format(
                        "mm2-replay {:.1f}sh @ {:.4f} side={} cue_sec={:.0f}",
                        shares_mm2, px_mm2, pick_yes_mm2 ? "YES" : "NO", cue.sec_in);
                    last_leg1_time_[key] = now_sec;
                    return act;
                }

                if (!mm2_session_hour_allowed(hour)) {
                    log_entry_status(market, key, now_sec, q, "mm2 session off");
                    continue;
                }
                if (mm2_obs_skips_leg1(window_start_ts)) {
                    const auto it = mm2_obs_skip_by_wts_.find(window_start_ts);
                    const std::string obs_skip_msg = fmt::format(
                        "mm2 obs skip {}",
                        it != mm2_obs_skip_by_wts_.end() ? it->second : "unknown");
                    log_entry_status(market, key, now_sec, q, obs_skip_msg.c_str());
                    continue;
                }
                const double entry_gate_max =
                    (mm2_early_entry_max_secs_left_ > mm2_entry_max_secs_left_ + kFloatTol)
                        ? mm2_early_entry_max_secs_left_
                        : mm2_entry_max_secs_left_;
                // Seed open tilt before clock gate so fav-early |Δtilt| can form while waiting.
                if ((mm2_tilt_entry_ || mm2_fav_early_bypass_) && q.yes > kFloatTol && q.no > kFloatTol) {
                    mm2_open_tilt_by_key_.emplace(key, q.yes - q.no);
                }
                bool fav_early_hit = false;
                if (mm2_fav_early_bypass_ && secs_left > entry_gate_max + kFloatTol
                    && q.yes > kFloatTol && q.no > kFloatTol) {
                    const bool fav_yes = q.yes + kFloatTol >= q.no;
                    const double fav_px = fav_yes ? q.yes : q.no;
                    const double spread = std::abs(q.yes - q.no);
                    double d_tilt_abs = 0.0;
                    const auto it_ot = mm2_open_tilt_by_key_.find(key);
                    if (it_ot != mm2_open_tilt_by_key_.end()) {
                        d_tilt_abs = std::abs((q.yes - q.no) - it_ot->second);
                    }
                    const bool shape_ok =
                        spread + kFloatTol >= mm2_fav_early_min_spread_
                        || d_tilt_abs + kFloatTol >= mm2_fav_early_min_dtilt_;
                    if (mm2_fav_early_mode_ == "C") {
                        fav_early_hit = fav_px + kFloatTol >= mm2_fav_early_fav_lo_
                            && fav_px <= mm2_fav_early_fav_hi_ + kFloatTol
                            && shape_ok;
                    } else {
                        // Mode B: buy favorite when book already shaped (spread or Δtilt).
                        fav_early_hit = fav_px + kFloatTol >= 0.48 && shape_ok;
                    }
                }
                if (secs_left > entry_gate_max + kFloatTol) {
                    if (!fav_early_hit) {
                        log_entry_status(market, key, now_sec, q, "mm2 wait entry window");
                        continue;
                    }
                }
                if (secs_left < mm2_entry_min_secs_left_ + kFloatTol) {
                    log_entry_status(market, key, now_sec, q, "mm2 too close to settle");
                    continue;
                }
                const bool early_lane = !fav_early_hit
                    && mm2_early_entry_max_secs_left_ > mm2_entry_max_secs_left_ + kFloatTol
                    && secs_left > mm2_entry_max_secs_left_ + kFloatTol;
                const auto bps_opt = window_spot_ret_bps(market, window_total_sec, secs_left);
                if (!bps_opt) {
                    log_entry_status(market, key, now_sec, q, "mm2 no spot");
                    continue;
                }
                const double spot_bps = *bps_opt;
                std::vector<double> vol_samples = mm2_spot_tracks_[key].bps;
                if (vol_samples.empty()
                    || std::abs(vol_samples.back() - spot_bps) > kFloatTol) {
                    vol_samples.push_back(spot_bps);
                }
                if (const auto vol_reason = mm2_vol_gate_reason(spot_bps, vol_samples)) {
                    log_entry_status(market, key, now_sec, q, vol_reason->c_str());
                    continue;
                }
                const bool regime_force_spot = regime_once && regime_once->force_spot_leg1;
                const bool strong_spot = regime_force_spot
                    || (mm2_min_spot_bps_ > kFloatTol
                        && std::abs(spot_bps) >= mm2_min_spot_bps_ - kFloatTol);
                if (mm2_flat_heuristic_skip(
                        q.yes, q.no, spot_bps, strong_spot, mm2_flat_max_spot_bps_,
                        mm2_flat_max_ask_sum_, mm2_skip_flat_)) {
                    log_entry_status(market, key, now_sec, q, "mm2 flat book skip");
                    continue;
                }
                // Daytime pack flat/liq proxy: thin either-side rest book.
                if (mm2_min_side_depth_ > kFloatTol) {
                    const int bj_h = window_start_bj_hour(market, window_total_sec);
                    const bool main_split_on = mm2_main_bj_end_ > mm2_main_bj_start_;
                    const bool in_main = !main_split_on
                        || (bj_h >= mm2_main_bj_start_ && bj_h < mm2_main_bj_end_);
                    if (!mm2_min_side_depth_main_only_ || in_main) {
                        const double yd = opposite_rest_depth(market.yes_token_id);
                        const double nd = opposite_rest_depth(market.no_token_id);
                        if (std::min(yd, nd) + kFloatTol < mm2_min_side_depth_) {
                            log_entry_status(market, key, now_sec, q, "mm2 low depth");
                            continue;
                        }
                    }
                }
                bool pick_yes_mm2;
                const char* mm2_pick_tag;
                // Seed open-book tilt for formation gate (first valid quote this window).
                if (mm2_tilt_entry_ && q.yes > kFloatTol && q.no > kFloatTol) {
                    mm2_open_tilt_by_key_.emplace(key, q.yes - q.no);
                }
                if (fav_early_hit) {
                    pick_yes_mm2 = q.yes + kFloatTol >= q.no;
                    mm2_pick_tag = (mm2_fav_early_mode_ == "C") ? "fav_early_c" : "fav_early_b";
                } else if (early_lane && !mm2_tilt_entry_) {
                    // Early: strong spot OR tilt fav (aligned + wide spread + high fav). Else wait.
                    const bool fav_yes = q.yes + kFloatTol >= q.no;
                    const double fav_px = fav_yes ? q.yes : q.no;
                    const double spread = std::abs(q.yes - q.no);
                    const bool aligned_fav =
                        (spot_bps > kFloatTol && fav_yes) || (spot_bps < -kFloatTol && !fav_yes);
                    if (strong_spot) {
                        pick_yes_mm2 = spot_bps > kFloatTol;
                        mm2_pick_tag = regime_force_spot ? "regime_spot" : "early_spot";
                    } else if (aligned_fav
                               && spread + kFloatTol >= mm2_early_tilt_min_spread_
                               && fav_px + kFloatTol >= mm2_early_tilt_min_fav_) {
                        pick_yes_mm2 = fav_yes;
                        mm2_pick_tag = "early_tilt";
                    } else {
                        log_entry_status(market, key, now_sec, q, "mm2 early skip — wait late");
                        continue;
                    }
                } else if (strong_spot) {
                    pick_yes_mm2 = spot_bps > kFloatTol;
                    mm2_pick_tag = regime_force_spot ? "regime_spot" : "spot";
                } else if (mm2_soft_spot_bps_ > kFloatTol
                           && std::abs(spot_bps) >= mm2_soft_spot_bps_ - kFloatTol) {
                    pick_yes_mm2 = spot_bps > kFloatTol;
                    mm2_pick_tag = "soft_spot";
                } else if (mm2_tilt_entry_ && mm2_tilt_delta_ > kFloatTol) {
                    // Research A: wait |Δtilt|>=delta then side pick.
                    const auto it_ot = mm2_open_tilt_by_key_.find(key);
                    if (it_ot == mm2_open_tilt_by_key_.end()) {
                        log_entry_status(market, key, now_sec, q, "mm2 wait tilt open");
                        continue;
                    }
                    const double tilt_now = q.yes - q.no;
                    const double d_tilt = tilt_now - it_ot->second;
                    if (std::abs(d_tilt) + kFloatTol < mm2_tilt_delta_) {
                        log_entry_status(market, key, now_sec, q, "mm2 wait tilt form");
                        continue;
                    }
                    if (mm2_tilt_side_follow_) {
                        // S2-D1: follow strengthened side (tilt↑ → YES dearer).
                        pick_yes_mm2 = d_tilt > kFloatTol;
                        mm2_pick_tag = "tilt_follow";
                    } else {
                        const bool fav_yes = q.yes + kFloatTol >= q.no;
                        const bool cheap_yes = q.yes + kFloatTol <= q.no;
                        const double fav_px = fav_yes ? q.yes : q.no;
                        const double spread = std::abs(q.yes - q.no);
                        if (spread + kFloatTol >= 0.40 || fav_px + kFloatTol >= 0.70) {
                            pick_yes_mm2 = cheap_yes;
                            mm2_pick_tag = "tilt_cheap_ext";
                        } else if (fav_px + kFloatTol >= 0.52 && fav_px <= 0.65 + kFloatTol) {
                            pick_yes_mm2 = fav_yes;
                            mm2_pick_tag = "tilt_fav_mid";
                        } else {
                            pick_yes_mm2 = cheap_yes;
                            mm2_pick_tag = "tilt_cheap_default";
                        }
                    }
                    // cheaper_later: arm at form, wait for ask dip (or entry_min deadline).
                    if (mm2_cheaper_later_) {
                        auto it_cl = mm2_cheaper_later_by_key_.find(key);
                        if (it_cl == mm2_cheaper_later_by_key_.end()) {
                            const double form_ask = pick_yes_mm2 ? q.yes : q.no;
                            mm2_cheaper_later_by_key_[key] = CheaperLaterArm{
                                pick_yes_mm2, form_ask, std::string(mm2_pick_tag)};
                            log_entry_status(market, key, now_sec, q, "mm2 wait cheaper later");
                            continue;
                        }
                        pick_yes_mm2 = it_cl->second.pick_yes;
                        const double cur_ask = pick_yes_mm2 ? q.yes : q.no;
                        const bool dipped = cur_ask + kFloatTol < it_cl->second.form_ask;
                        const bool deadline =
                            secs_left <= mm2_entry_min_secs_left_ + kFloatTol;
                        if (!dipped && !deadline) {
                            log_entry_status(market, key, now_sec, q, "mm2 wait cheaper later");
                            continue;
                        }
                        mm2_pick_tag = dipped ? "tilt_cheaper" : "tilt_deadline";
                    }
                } else if (mm2_v2j_adaptive_) {
                    // v2j: selective favorite (mom / soft-spread), else cheap. No BJ clock.
                    const bool fav_yes = q.yes + kFloatTol >= q.no;
                    const double fav_px = fav_yes ? q.yes : q.no;
                    const double spread = std::abs(q.yes - q.no);
                    bool mom_with_fav = false;
                    if (const auto past_bps = window_spot_ret_bps_ago(
                            market, window_total_sec, secs_left, mm2_v2j_mom_lookback_sec_)) {
                        const double d_spot = spot_bps - *past_bps;
                        mom_with_fav = (d_spot > kFloatTol && fav_yes)
                            || (d_spot < -kFloatTol && !fav_yes);
                    }
                    if (mom_with_fav
                        && fav_px <= mm2_v2j_mom_fav_max_ask_ + kFloatTol) {
                        pick_yes_mm2 = fav_yes;
                        mm2_pick_tag = "mom_fav";
                    } else if (mm2_v2j_spread_min_ > kFloatTol
                               && spread + kFloatTol >= mm2_v2j_spread_min_
                               && fav_px <= mm2_v2j_spread_fav_max_ask_ + kFloatTol) {
                        pick_yes_mm2 = fav_yes;
                        mm2_pick_tag = "spread_soft";
                    } else if (q.yes + kFloatTol < q.no) {
                        pick_yes_mm2 = true;
                        mm2_pick_tag = "cheap";
                    } else if (q.no + kFloatTol < q.yes) {
                        pick_yes_mm2 = false;
                        mm2_pick_tag = "cheap";
                    } else {
                        pick_yes_mm2 = true;
                        mm2_pick_tag = "cheap";
                    }
                } else if (mm2_late_tilt_min_ask_ > kFloatTol) {
                    const double mx = std::max(q.yes, q.no);
                    const double mn = std::min(q.yes, q.no);
                    if (mx >= mm2_late_tilt_min_ask_ - kFloatTol
                        && mx - mn >= mm2_late_tilt_min_spread_ - kFloatTol) {
                        pick_yes_mm2 = q.yes >= q.no;
                        mm2_pick_tag = "late_tilt";
                    } else if (q.yes + kFloatTol < q.no) {
                        pick_yes_mm2 = true;
                        mm2_pick_tag = "cheap";
                    } else if (q.no + kFloatTol < q.yes) {
                        pick_yes_mm2 = false;
                        mm2_pick_tag = "cheap";
                    } else {
                        pick_yes_mm2 = true;
                        mm2_pick_tag = "cheap";
                    }
                } else {
                    // mm2 flat windows: tracker ~65% leg1 on cheap (lower ask) side, not expensive.
                    if (q.yes + kFloatTol < q.no) {
                        pick_yes_mm2 = true;
                    } else if (q.no + kFloatTol < q.yes) {
                        pick_yes_mm2 = false;
                    } else {
                        pick_yes_mm2 = true;
                    }
                    mm2_pick_tag = "cheap";
                }
                // B′ hole-fix: early YES must itself be favorite + shaped
                // (spread + YES ask). Old check used max(yes,no), so cheap YES
                // vs high NO still passed — that was the underdog hole.
                if (early_lane && pick_yes_mm2 && mm2_early_yes_guard_
                    && q.yes > kFloatTol && q.no > kFloatTol) {
                    const double spread = std::abs(q.yes - q.no);
                    const bool yes_is_fav = q.yes + kFloatTol >= q.no;
                    if (!yes_is_fav
                        || spread + kFloatTol < mm2_early_tilt_min_spread_
                        || q.yes + kFloatTol < mm2_early_tilt_min_fav_) {
                        log_entry_status(market, key, now_sec, q, "mm2 early yes guard");
                        continue;
                    }
                }
                const double px_mm2 = pick_yes_mm2 ? q.yes : q.no;
                const double hmax = heavy_max_price_ > kFloatTol ? heavy_max_price_ : 0.75;
                if (px_mm2 + kFloatTol < mm2_favorite_min_px_) {
                    log_entry_status(market, key, now_sec, q, "mm2 side not favorite");
                    continue;
                }
                if (px_mm2 <= kFloatTol || px_mm2 > hmax + kFloatTol) {
                    log_entry_status(market, key, now_sec, q, "mm2 price band");
                    continue;
                }
                // Secondary (off BJ main field): skip cheap lottery asks.
                const bool main_split_on = mm2_main_bj_end_ > mm2_main_bj_start_;
                if (main_split_on && mm2_offhours_min_ask_ > kFloatTol) {
                    const int bj_h = window_start_bj_hour(market, window_total_sec);
                    const bool in_main = bj_h >= mm2_main_bj_start_ && bj_h < mm2_main_bj_end_;
                    if (!in_main && px_mm2 + kFloatTol < mm2_offhours_min_ask_) {
                        log_entry_status(market, key, now_sec, q, "mm2 offhours cheap px");
                        continue;
                    }
                } else if (!main_split_on || mm2_offhours_min_ask_ <= kFloatTol) {
                    // Rare: misconfig — emit once per process so we notice gates are off.
                    static std::atomic<bool> warned_offhours_off{false};
                    if (!warned_offhours_off.exchange(true)) {
                        spdlog::warn(
                            "LIH offhours px gate OFF (main_split={} min_ask={:.4f}) — "
                            "check LIH_MM2_OFFHOURS_MIN_ASK / MAIN_BJ_*",
                            main_split_on, mm2_offhours_min_ask_);
                    }
                }
                const auto& tok_mm2 = pick_yes_mm2 ? market.yes_token_id : market.no_token_id;
                // First fill targets leg1_shares; subsequent adds use SCALE_CLIP.
                double shares_mm2 = leg1_shares_;
                // v2i: strong-spot entries are usually expensive — cap notional, not raw shares.
                const bool spot_tagged = std::strcmp(mm2_pick_tag, "spot") == 0
                    || std::strcmp(mm2_pick_tag, "regime_spot") == 0
                    || std::strcmp(mm2_pick_tag, "soft_spot") == 0
                    || std::strcmp(mm2_pick_tag, "early_spot") == 0;
                if (spot_tagged && mm2_spot_leg1_max_usdc_ > kFloatTol && px_mm2 > kFloatTol) {
                    shares_mm2 = std::min(shares_mm2, mm2_spot_leg1_max_usdc_ / px_mm2);
                }
                shares_mm2 = cap_shares_budget(shares_mm2, max_leg_usdc, px_mm2);
                shares_mm2 = shrink_for_rest_book(tok_mm2, shares_mm2);
                if (shares_mm2 + kFloatTol < kClobMinOrderShares) {
                    log_entry_status(market, key, now_sec, q, "mm2 depth min");
                    continue;
                }
                if (shares_mm2 * px_mm2 + kFloatTol < kLegMinUsdc) {
                    log_entry_status(market, key, now_sec, q, "below min usdc");
                    continue;
                }
                const double cost_mm2 = shares_mm2 * px_mm2;
                const auto [can_mm2, why_mm2] = rm.can_open_lih_leg(
                    cost_mm2, false, nullptr, 0.0, &market.asset, market.window_minutes);
                if (!can_mm2) {
                    log_entry_status(market, key, now_sec, q, why_mm2.c_str());
                    continue;
                }
                LegInAction act;
                act.kind = LegInAction::Kind::OpenLeg1;
                act.market = market;
                act.buy_yes = pick_yes_mm2;
                act.price = px_mm2;
                act.shares = shares_mm2;
                act.note = fmt::format(
                    "mm2-heavy {:.1f}sh @ {:.4f} {} {:.1f}bps",
                    shares_mm2, px_mm2, mm2_pick_tag, spot_bps);
                last_leg1_time_[key] = now_sec;
                mm2_cheaper_later_by_key_.erase(key);
                return act;
            }

            if (open_gap_mode_) {
                const double hmax = heavy_max_price_ > kFloatTol ? heavy_max_price_ : 0.75;
                const bool yes_ok = q.yes > kFloatTol && q.yes <= hmax + kFloatTol;
                const bool no_ok = q.no > kFloatTol && q.no <= hmax + kFloatTol;
                if (!yes_ok && !no_ok) {
                    log_entry_status(market, key, now_sec, q, "open-gap no heavy price");
                    continue;
                }
                const bool pick_yes_og = yes_ok && (!no_ok || q.yes >= q.no);
                const double px_og = pick_yes_og ? q.yes : q.no;
                const auto& tok_og = pick_yes_og ? market.yes_token_id : market.no_token_id;
                double shares_og = std::min(heavy_clip_cap(), leg1_shares_);
                shares_og = cap_shares_budget(shares_og, max_leg_usdc, px_og);
                shares_og = shrink_for_rest_book(tok_og, shares_og);
                if (shares_og + kFloatTol < kClobMinOrderShares) {
                    log_entry_status(market, key, now_sec, q, "open-gap depth min");
                    continue;
                }
                if (shares_og * px_og + kFloatTol < kLegMinUsdc) {
                    log_entry_status(market, key, now_sec, q, "below min usdc");
                    continue;
                }
                const double cost_og = shares_og * px_og;
                const auto [can_og, why_og] = rm.can_open_lih_leg(
                    cost_og, false, nullptr, 0.0, &market.asset, market.window_minutes);
                if (!can_og) {
                    log_entry_status(market, key, now_sec, q, why_og.c_str());
                    continue;
                }
                LegInAction act;
                act.kind = LegInAction::Kind::OpenLeg1;
                act.market = market;
                act.buy_yes = pick_yes_og;
                act.price = px_og;
                act.shares = shares_og;
                act.note = fmt::format("open-gap-heavy {:.1f}sh @ {:.4f}", shares_og, px_og);
                last_leg1_time_[key] = now_sec;
                return act;
            }

            bool pick_yes = false;
            const char* entry_tag = "entry";

            if (leg1_trigger_mode_) {
                const bool cap_max = leg1_trigger_max_ > kFloatTol;
                const auto in_band = [&](double px) {
                    if (px + kFloatTol < leg1_trigger_min_) return false;
                    if (cap_max && px > leg1_trigger_max_ + kFloatTol) return false;
                    return true;
                };
                const bool yes_hit = in_band(q.yes);
                const bool no_hit = in_band(q.no);
                if (!yes_hit && !no_hit) {
                    const bool yes_lo = q.yes + kFloatTol < leg1_trigger_min_;
                    const bool no_lo = q.no + kFloatTol < leg1_trigger_min_;
                    const bool yes_hi = cap_max && q.yes > leg1_trigger_max_ + kFloatTol;
                    const bool no_hi = cap_max && q.no > leg1_trigger_max_ + kFloatTol;
                    const char* why = (yes_hi || no_hi) ? "above trigger max" : "below trigger";
                    if (yes_lo && no_lo) why = "below trigger";
                    log_entry_status(market, key, now_sec, q, why);
                    continue;
                }
                pick_yes = yes_hit && (!no_hit || q.yes >= q.no);
                entry_tag = q.from_mirror ? "mirror-trigger" : "trigger-entry";
            } else if (leg1_trend_mode_) {
                const bool yes_trend = spot_trend_favors(market, true);
                const bool no_trend = spot_trend_favors(market, false);
                if (yes_trend && no_trend) {
                    log_entry_status(market, key, now_sec, q, "trend ambiguous");
                    continue;
                }
                if (!yes_trend && !no_trend) {
                    log_entry_status(market, key, now_sec, q, "no clear trend");
                    continue;
                }
                pick_yes = yes_trend;
                const double trend_px = pick_yes ? q.yes : q.no;
                if (trend_px > leg1_trend_max_price_ + kFloatTol) {
                    log_entry_status(market, key, now_sec, q, "trend leg above max");
                    continue;
                }
                entry_tag = q.from_mirror ? "mirror-trend" : "trend-entry";
            } else {
                const bool yes_cheap = q.yes <= leg1_max_price_ + kFloatTol;
                const bool no_cheap = q.no <= leg1_max_price_ + kFloatTol;
                if (!yes_cheap && !no_cheap) {
                    log_entry_status(market, key, now_sec, q, "no cheap leg");
                    continue;
                }
                pick_yes = yes_cheap && (!no_cheap || q.yes <= q.no);
                if (!leg1_trend_allows(market, pick_yes)) {
                    log_entry_status(market, key, now_sec, q, pick_yes ? "trend blocks YES" : "trend blocks NO");
                    continue;
                }
                entry_tag = q.from_mirror ? "mirror" : "entry";
            }

            const double px = pick_yes ? q.yes : q.no;
            const double opposite_px = pick_yes ? q.no : q.yes;

            double shares = leg1_shares_;
            if (leg1_clip_shares_ > kFloatTol) {
                shares = std::min(leg1_clip_shares_, leg1_shares_);
            }
            if (vwap_entry_gate_) {
                const auto vr = eval_vwap_entry(market, shares, rm.get_fee_rate());
                if (!vr.ok) {
                    const double cap = vwap_entry_cap_ > target_combined_ + kFloatTol
                        ? vwap_entry_cap_
                        : (mid_soft_cap_ > target_combined_ + kFloatTol ? mid_soft_cap_
                                                                         : target_combined_);
                    std::string msg = fmt::format(
                        "[LIH DEBUG] entry-vwap-skip | {} {}m | vwap {:.4f}+{:.4f}={:.4f} cap {:.4f} "
                        "pair {:.1f}sh net ${:.2f} need ${:.2f}",
                        market.asset, market.window_minutes, vr.yes_vwap, vr.no_vwap, vr.comb,
                        cap, vr.paired_sh, vr.net_edge, vr.min_edge);
                    spdlog::info(msg);
                    store_.push_telemetry(msg);
                    log_entry_status(market, key, now_sec, q, "vwap entry skip");
                    continue;
                }
                shares = std::min(shares, vr.paired_sh);
            } else if (hedge_feasible_entry_ &&
                !passes_hedge_feasible_entry(market, pick_yes, px, shares, opposite_px)) {
                const double cap = hedge_feasible_cap_ > target_combined_ + kFloatTol
                    ? hedge_feasible_cap_
                    : (mid_soft_cap_ > target_combined_ + kFloatTol ? mid_soft_cap_ : 0.98);
                const double max_opp = cap - px;
                const auto& opp_tok = pick_yes ? market.no_token_id : market.yes_token_id;
                const double opp_depth = opposite_rest_depth(opp_tok);
                std::string msg = fmt::format(
                    "[LIH DEBUG] entry-feasible-skip | {} {}m | leg1 {:.4f} opp {:.4f} max_opp {:.4f} "
                    "depth {:.1f} need {:.1f}sh",
                    market.asset, market.window_minutes, px, opposite_px, max_opp,
                    opp_depth, shares * kEntryDepthFillRatio);
                spdlog::info(msg);
                store_.push_telemetry(msg);
                log_entry_status(market, key, now_sec, q, "hedge-feasible skip");
                continue;
            }

            if (max_matched_cap > kFloatTol) {
                shares = std::min(shares, max_matched_cap);
            }
            shares = cap_shares_budget(shares, max_leg_usdc, px);
            {
                const auto& tok = pick_yes ? market.yes_token_id : market.no_token_id;
                const double book_sh = shrink_for_rest_book(tok, shares);
                if (book_sh > kFloatTol) {
                    shares = book_sh;
                }
            }
            if (shares + kFloatTol < kClobMinOrderShares) {
                log_entry_status(market, key, now_sec, q, "depth below min order");
                continue;
            }
            if (shares <= kFloatTol) {
                log_entry_status(market, key, now_sec, q, "depth fill 0");
                continue;
            }
            if (shares * px + kFloatTol < kLegMinUsdc) {
                log_entry_status(market, key, now_sec, q, "below min usdc");
                continue;
            }

            const double entry_marginal = px + opposite_px;
            if (!vwap_entry_gate_ && max_entry_marginal_ > kFloatTol &&
                entry_marginal > max_entry_marginal_ + kFloatTol) {
                log_entry_status(market, key, now_sec, q, "entry marginal too wide");
                spdlog::info(
                    "[LIH DEBUG] entry-skip | {} {}m | leg1 {:.4f} opp {:.4f} marginal {:.4f} > {:.2f}",
                    market.asset, market.window_minutes, px, opposite_px, entry_marginal,
                    max_entry_marginal_);
                continue;
            }

            const double cost = shares * px;
            const auto [can_open, block_reason] = rm.can_open_lih_leg(
                cost, false, nullptr, 0.0, &market.asset, market.window_minutes);
            if (!can_open) {
                log_entry_status(market, key, now_sec, q, block_reason.c_str());
                continue;
            }

            LegInAction act;
            act.kind = LegInAction::Kind::OpenLeg1;
            act.market = market;
            act.buy_yes = pick_yes;
            act.price = px;
            act.shares = shares;
            act.note = entry_tag;
            last_leg1_time_[key] = now_sec;
            return act;
        }

        if (secs_left < min_seconds_remaining_ && secs_left > endgame_secs_) continue;

        const auto& pos = *open_lih;
        const double matched = std::min(pos.yes_shares, pos.no_shares);
        const double remaining_matched = rm.lih_remaining_matched_shares(pos.lih_id);
        const double yes_avg = pos.yes_shares > kFloatTol ? pos.yes_cost / pos.yes_shares : 0.0;
        const double no_avg = pos.no_shares > kFloatTol ? pos.no_cost / pos.no_shares : 0.0;
        const double gap = std::abs(pos.yes_shares - pos.no_shares);
        const double port_avg = (yes_avg > kFloatTol && no_avg > kFloatTol) ? yes_avg + no_avg : 0.0;

        if (gap > kLihBalancedGapShares) {
            const bool in_endgame_pre = secs_left <= endgame_secs_;

            // Parallel clip: buy light leg in 5sh steps whenever comb <= cap, even while leg1
            // is still scaling — do not wait for full leg1_shares before first hedge.
            const double since_open = now_sec - pos.opened_at;
            const bool mm2_hedge_delay = mm2_mode_
                && since_open < mm2_heavy_delay_sec_ - kFloatTol;
            if (parallel_clip_hedge_ && !mm2_hedge_delay && light_clip_shares() > kFloatTol
                && (!in_endgame_pre || open_gap_mode_ || mm2_mode_)) {
                const bool heavy_yes_p = pos.yes_shares > pos.no_shares + kFloatTol;
                const bool heavy_no_p = pos.no_shares > pos.yes_shares + kFloatTol;
                if (heavy_yes_p || heavy_no_p) {
                    const bool need_yes_p = pos.yes_shares < pos.no_shares - kFloatTol;
                    const double heavy_avg_p = heavy_yes_p ? yes_avg : no_avg;
                    const Quote hq_p = hedge_quote_for(market);
                    const double light_ask_p = need_yes_p ? hq_p.yes : hq_p.no;
                    const auto& light_tok_p = need_yes_p ? market.yes_token_id : market.no_token_id;
                    if (heavy_avg_p > kFloatTol && light_ask_p > kFloatTol) {
                        const double marginal_p = heavy_avg_p + light_ask_p;
                        const double cap_p = gap_hedge_cap_for(secs_left, gap);
                        const bool gap_over_max = max_gap_shares_ > kFloatTol
                            && gap > max_gap_shares_ + kFloatTol;
                        if (marginal_p <= cap_p + kFloatTol || gap_over_max) {
                            const double rebal_cd_p = rebalance_cooldown_seconds_;
                            if (!(rebal_cd_p > 0.0 && last_rebalance_time_.contains(key) &&
                                  (now_sec - last_rebalance_time_.at(key)) < rebal_cd_p) &&
                                !rm.lih_rebalance_inflight(pos.lih_id)) {
                                if (!(hedge_min_gap_trigger_ > kFloatTol
                                      && gap < hedge_min_gap_trigger_ - kFloatTol)) {
                                    const double target_gap_p = hedge_target_min_gap_ > kFloatTol
                                        ? hedge_target_min_gap_ : 0.0;
                                    const double hedge_room_p = std::max(0.0, gap - target_gap_p);
                                    if (hedge_room_p > kFloatTol) {
                                        double step_clip = (mm2_mode_
                                            && mm2_scale_clip_shares_ > kFloatTol)
                                            ? mm2_scale_clip_shares_
                                            : light_clip_shares();
                                        if (mm2_mode_
                                            && mm2_hedge_boost_ > 1.0 + kFloatTol
                                            && mm2_spot_against_heavy(
                                                market, heavy_yes_p, window_total_sec, secs_left)) {
                                            step_clip *= mm2_hedge_boost_;
                                        }
                                        const double step_p = std::min(step_clip, hedge_room_p);
                                        double fill_p = hedge_fill_shares(
                                            light_tok_p, step_p, light_ask_p,
                                            max_leg_usdc, remaining_matched);
                                        if (fill_p > kFloatTol) {
                                            const double cost_p = fill_p * light_ask_p;
                                            if (rm.can_open_lih_leg(cost_p, true, &pos.lih_id, fill_p).first) {
                                                LegInAction act;
                                                act.kind = LegInAction::Kind::CompleteHedge;
                                                act.market = market;
                                                act.buy_yes = need_yes_p;
                                                act.price = light_ask_p;
                                                act.shares = fill_p;
                                                act.lih_id = pos.lih_id;
                                                act.note = fmt::format(
                                                    "parallel-clip +{:.1f}/{:.1f} floor {:.0f} "
                                                    "sum {:.4f} cap {:.4f}",
                                                    fill_p, gap, target_gap_p, marginal_p, cap_p);
                                                last_rebalance_time_[key] = now_sec;
                                                return act;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // Scale heavy side (book-sized clips in open-gap mode).
            if (leg1_clip_shares_ > kFloatTol || open_gap_mode_ || mm2_mode_) {
                const bool heavy_yes = pos.yes_shares > pos.no_shares + kFloatTol;
                const bool heavy_no = pos.no_shares > pos.yes_shares + kFloatTol;
                const bool only_yes = pos.yes_shares > kFloatTol && pos.no_shares <= kFloatTol;
                const bool only_no = pos.no_shares > kFloatTol && pos.yes_shares <= kFloatTol;
                // Open-gap: build light-leg matched pairs before scaling heavy further.
                // mm2: stack heavy first (no defer until matched).
                if (!(open_gap_mode_ && !mm2_mode_ && matched <= kFloatTol)
                    && (heavy_yes || heavy_no || only_yes || only_no)) {
                    const bool scale_yes = heavy_yes || only_yes;
                    const double heavy_sh = scale_yes ? pos.yes_shares : pos.no_shares;
                    const double heavy_cap = mm2_mode_ ? mm2_heavy_scale_cap() : leg1_shares_;
                    if (heavy_sh + kFloatTol < heavy_cap) {
                        const double clip_cd = leg1_cooldown_seconds_;
                        if (clip_cd <= 0.0 || !last_leg1_time_.contains(key) ||
                            (now_sec - last_leg1_time_.at(key)) >= clip_cd) {
                            const double px = scale_yes ? q.yes : q.no;
                            if (px > kFloatTol) {
                                const double px_max = (open_gap_mode_ || mm2_mode_)
                                    ? (heavy_max_price_ > kFloatTol ? heavy_max_price_ : 0.75)
                                    : leg1_max_price_;
                                const bool price_ok = open_gap_mode_ || mm2_mode_
                                    || leg1_trigger_mode_ || leg1_trend_mode_
                                    || px <= leg1_max_price_ + kFloatTol;
                                // against_only: spot against heavy → skip same-side scale
                                // this tick (hedge path below still runs).
                                const bool scale_against_stop = mm2_mode_
                                    && mm2_scale_against_stop_bps_ > kFloatTol
                                    && mm2_spot_against_heavy(
                                        market, scale_yes, window_total_sec, secs_left,
                                        mm2_scale_against_stop_bps_);
                                // HF1-E′: once gap<=thr in late window, latch no further scale.
                                if (mm2_mode_ && mm2_hf1e_latch_gap_ > kFloatTol) {
                                    const double gap_now = std::abs(pos.yes_shares - pos.no_shares);
                                    if (gap_now <= mm2_hf1e_latch_gap_ + kFloatTol
                                        && secs_left <= mm2_hf1e_latch_secs_left_ + kFloatTol) {
                                        mm2_hf1e_latched_by_key_[key] = true;
                                    }
                                }
                                const auto it_hf1e = mm2_hf1e_latched_by_key_.find(key);
                                const bool hf1e_noscale = mm2_mode_
                                    && mm2_hf1e_latch_gap_ > kFloatTol
                                    && it_hf1e != mm2_hf1e_latched_by_key_.end()
                                    && it_hf1e->second;
                                if (scale_against_stop) {
                                    static thread_local double last_against_log_sec = 0.0;
                                    if (now_sec - last_against_log_sec >= 5.0) {
                                        last_against_log_sec = now_sec;
                                        const auto bps = window_spot_ret_bps(
                                            market, window_total_sec, secs_left);
                                        const auto msg = fmt::format(
                                            "[LIH DEBUG] scale-against-stop | {} {}m | "
                                            "heavy {} | spot {:.1f}bps thr {:.1f} — skip scale",
                                            market.asset, market.window_minutes,
                                            scale_yes ? "YES" : "NO",
                                            bps ? *bps : 0.0,
                                            mm2_scale_against_stop_bps_);
                                        spdlog::info(msg);
                                        store_.push_telemetry(msg);
                                    }
                                } else if (hf1e_noscale) {
                                    static thread_local double last_hf1e_log_sec = 0.0;
                                    if (now_sec - last_hf1e_log_sec >= 5.0) {
                                        last_hf1e_log_sec = now_sec;
                                        const double gap_now =
                                            std::abs(pos.yes_shares - pos.no_shares);
                                        const auto msg = fmt::format(
                                            "[LIH DEBUG] hf1e-noscale | {} {}m | "
                                            "gap {:.1f} left {:.0f}s thr_gap {:.0f} thr_left {:.0f} "
                                            "— skip same-side scale",
                                            market.asset, market.window_minutes, gap_now,
                                            secs_left, mm2_hf1e_latch_gap_,
                                            mm2_hf1e_latch_secs_left_);
                                        spdlog::info(msg);
                                        store_.push_telemetry(msg);
                                    }
                                } else if (price_ok && px <= px_max + kFloatTol) {
                                    double clip_lim = mm2_mode_
                                        ? (mm2_scale_clip_shares_ > kFloatTol
                                            ? mm2_scale_clip_shares_ : light_clip_shares())
                                        : (open_gap_mode_
                                            ? heavy_clip_cap() : leg1_clip_shares_);
                                    if (mm2_mode_
                                        && mm2_scale_boost_ > 1.0 + kFloatTol
                                        && mm2_spot_favors_heavy(
                                            market, scale_yes, window_total_sec, secs_left)) {
                                        clip_lim *= mm2_scale_boost_;
                                    }
                                    if (mm2_mode_) {
                                        clip_lim = std::min(clip_lim, heavy_clip_cap());
                                    }
                                    double fill = std::min(clip_lim, heavy_cap - heavy_sh);
                                    fill = cap_shares_budget(fill, max_leg_usdc, px);
                                    // Optional hard USDC cap while spot still favors heavy.
                                    if (mm2_mode_ && mm2_spot_heavy_max_usdc_ > kFloatTol
                                        && mm2_min_spot_bps_ > kFloatTol) {
                                        if (mm2_spot_favors_heavy(
                                                market, scale_yes, window_total_sec, secs_left)) {
                                            const double heavy_cost = heavy_sh * px;
                                            const double room =
                                                mm2_spot_heavy_max_usdc_ - heavy_cost;
                                            if (room <= kFloatTol) {
                                                fill = 0.0;
                                            } else {
                                                fill = std::min(fill, room / px);
                                            }
                                        }
                                    }
                                    fill = shrink_for_rest_book(
                                        scale_yes ? market.yes_token_id : market.no_token_id, fill);
                                    if (fill + kFloatTol < kClobMinOrderShares) {
                                        continue;
                                    }
                                    if (fill > kFloatTol && fill * px + kFloatTol >= kLegMinUsdc) {
                                        const double cost = fill * px;
                                        if (rm.can_open_lih_leg(
                                                cost, true, &pos.lih_id, fill).first
                                            && !rm.lih_rebalance_inflight(pos.lih_id)) {
                                            LegInAction act;
                                            act.kind = LegInAction::Kind::AddLeg1;
                                            act.market = market;
                                            act.buy_yes = scale_yes;
                                            act.price = px;
                                            act.shares = fill;
                                            act.lih_id = pos.lih_id;
                                            const char* clip_tag = mm2_mode_
                                                ? "mm2-heavy"
                                                : (open_gap_mode_ ? "og-heavy" : "leg1-clip");
                                            act.note = fmt::format(
                                                "{} +{:.1f}/{:.1f} @ {:.4f}",
                                                clip_tag, fill, heavy_cap, px);
                                            last_leg1_time_[key] = now_sec;
                                            return act;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            const bool in_endgame = secs_left <= endgame_secs_;
            const bool endgame_override = in_endgame && secs_left <= endgame_override_secs_;
            const double rebal_cd = endgame_override
                ? endgame_override_cooldown_
                : rebalance_cooldown_seconds_;

            if (rebal_cd > 0.0 &&
                last_rebalance_time_.contains(key) &&
                (now_sec - last_rebalance_time_.at(key)) < rebal_cd) {
                log_rebalance_status(market, key, now_sec, pos, q, yes_avg, no_avg, gap);
                continue;
            }
            if (rm.lih_rebalance_inflight(pos.lih_id)) {
                log_rebalance_status(market, key, now_sec, pos, q, yes_avg, no_avg, gap);
                continue;
            }
            log_rebalance_status(market, key, now_sec, pos, q, yes_avg, no_avg, gap);

            if (unwind_enabled_ && unwind_secs_ > kFloatTol) {
                const double elapsed = now_sec - pos.opened_at;
                const bool in_endgame = secs_left <= endgame_secs_;
                bool skip_unwind_hold = false;
                if (in_endgame && !endgame_minimize_gap_) {
                    const bool heavy_yes = pos.yes_shares > pos.no_shares + kFloatTol;
                    const Quote hq = hedge_quote_for(market);
                    const double heavy_ask = heavy_yes ? hq.yes : hq.no;
                    if (heavy_ask >= endgame_hold_ask_ - kFloatTol
                        && heavy_ask > endgame_resume_hedge_ask_ + kFloatTol
                        && (mm2_mode_
                            ? mm2_spot_favors_heavy(market, heavy_yes, window_total_sec, secs_left)
                            : spot_trend_favors(market, heavy_yes))) {
                        skip_unwind_hold = true;
                    }
                }
                if (!skip_unwind_hold
                    && elapsed >= unwind_secs_ - kFloatTol
                    && secs_left > min_seconds_remaining_ + kFloatTol) {
                    if (unwind_cooldown_ > 0.0
                        && last_unwind_time_.contains(key)
                        && (now_sec - last_unwind_time_.at(key)) < unwind_cooldown_) {
                        continue;
                    }
                    const bool sell_yes = pos.yes_shares > pos.no_shares + kFloatTol;
                    const auto& tok = sell_yes ? market.yes_token_id : market.no_token_id;
                    double bid = 0.0;
                    if (auto b = store_.get_official_mark_bid(tok); b && *b > kFloatTol) {
                        bid = *b;
                    }
                    if (bid <= kFloatTol) continue;
                    const double sell_sh = gap;
                    if (sell_sh <= kFloatTol) continue;
                    LegInAction act;
                    act.kind = LegInAction::Kind::UnwindLeg1;
                    act.market = market;
                    act.buy_yes = sell_yes;
                    act.price = bid;
                    act.shares = sell_sh;
                    act.lih_id = pos.lih_id;
                    act.note = fmt::format("unwind {:.0f}s gap {:.1f} bid {:.4f}",
                                           elapsed, gap, bid);
                    last_unwind_time_[key] = now_sec;
                    spdlog::info(
                        "[LIH DEBUG] unwind-trigger | {} {}m | {:.0f}s since leg1 | sell {} "
                        "{:.1f}sh @ {:.4f}",
                        market.asset, market.window_minutes, elapsed,
                        sell_yes ? "YES" : "NO", sell_sh, bid);
                    return act;
                }
            }

            const Quote hq = hedge_quote_for(market);
            const bool heavy_yes = pos.yes_shares > pos.no_shares + kFloatTol;
            const bool need_yes = pos.yes_shares < pos.no_shares - kFloatTol;
            const double heavy_avg = heavy_yes ? yes_avg : no_avg;
            const double heavy_ask = heavy_yes ? hq.yes : hq.no;
            const double light_ask = need_yes ? hq.yes : hq.no;
            const auto& light_token = need_yes ? market.yes_token_id : market.no_token_id;

            if (heavy_avg <= kFloatTol || light_ask <= kFloatTol) continue;

            const double marginal = heavy_avg + light_ask;
            const bool at_target = marginal <= target_combined_ + kFloatTol;
            const bool force = secs_left <= force_balance_secs_ && !in_endgame;
            const bool in_mid_soft_window = !in_endgame
                && mid_soft_cap_ > target_combined_ + kFloatTol
                && (mid_soft_start_secs_ <= kFloatTol
                    || secs_left <= mid_soft_start_secs_ + kFloatTol);

            auto try_light_hedge = [&](double max_fill, bool require_full_gap,
                                       const char* mode) -> std::optional<LegInAction> {
                if (hedge_min_gap_trigger_ > kFloatTol && gap < hedge_min_gap_trigger_ - kFloatTol) {
                    return std::nullopt;
                }
                const double target_gap = hedge_target_min_gap_ > kFloatTol ? hedge_target_min_gap_ : 0.0;
                const double hedge_room = std::max(0.0, gap - target_gap);
                double adj_fill = max_fill;
                if (mm2_mode_ && mm2_hedge_boost_ > 1.0 + kFloatTol
                    && mm2_spot_against_heavy(market, heavy_yes, window_total_sec, secs_left)) {
                    adj_fill *= mm2_hedge_boost_;
                }
                const double capped_gap = std::min({hedge_room, adj_fill, remaining_matched});
                if (capped_gap <= kFloatTol) return std::nullopt;
                const double fill = hedge_fill_shares(
                    light_token, capped_gap, light_ask, max_leg_usdc, remaining_matched);
                if (fill <= kFloatTol) return std::nullopt;
                if (require_full_gap && fill + kFloatTol < capped_gap) return std::nullopt;
                const double cost = fill * light_ask;
                if (!rm.can_open_lih_leg(cost, true, &pos.lih_id, fill).first) return std::nullopt;
                LegInAction act;
                act.kind = LegInAction::Kind::CompleteHedge;
                act.market = market;
                act.buy_yes = need_yes;
                act.price = light_ask;
                act.shares = fill;
                act.lih_id = pos.lih_id;
                act.note = fmt::format("{} +{:.1f}/{:.1f} sum {:.4f} port {:.4f}",
                                       mode, fill, gap, marginal, port_avg);
                last_rebalance_time_[key] = now_sec;
                return act;
            };

            if (in_endgame) {
                if (!endgame_minimize_gap_) {
                    const bool on_trend = mm2_mode_
                        ? mm2_spot_favors_heavy(market, heavy_yes, window_total_sec, secs_left)
                        : spot_trend_favors(market, heavy_yes);
                    const bool hold_win = heavy_ask >= endgame_hold_ask_ - kFloatTol
                        && heavy_ask > endgame_resume_hedge_ask_ + kFloatTol
                        && on_trend;
                    if (hold_win
                        && !(max_gap_shares_ > kFloatTol
                             && gap > max_gap_shares_ + kFloatTol)) {
                        std::string msg = fmt::format(
                            "[LIH DEBUG] endgame-hold | {} {}m | heavy {:.4f} on-trend | {:.0f}s left — skip hedge",
                            market.asset, market.window_minutes, heavy_ask, secs_left);
                        spdlog::info(msg);
                        store_.push_telemetry(msg);
                        continue;
                    }
                }

                const double step = gap >= endgame_gap_large_ - kFloatTol
                    ? endgame_step_large_ : endgame_step_small_;
                const double ladder_max = endgame_ladder_max_marginal(secs_left);
                const double abs_max = endgame_ladder_enabled_
                    ? std::min(endgame_ladder_end_, endgame_soft_cap_)
                    : endgame_soft_cap_;
                const bool can_profit = at_target;
                const bool can_ladder = endgame_ladder_enabled_
                    && secs_left <= endgame_ladder_secs_ + kFloatTol
                    && marginal <= ladder_max + kFloatTol;
                const bool can_soft = !endgame_ladder_enabled_
                    && marginal <= endgame_soft_cap_ + kFloatTol;
                const bool can_override = endgame_override
                    && marginal <= abs_max + kFloatTol;
                const bool gap_over_max = max_gap_shares_ > kFloatTol
                    && gap > max_gap_shares_ + kFloatTol;
                const bool can_gap_force = gap_over_max
                    && marginal <= gap_hedge_cap_for(secs_left, gap) + kFloatTol;
                const bool can_gap_emergency = gap_over_max && !can_gap_force;
                if (!can_profit && !can_ladder && !can_soft && !can_override
                    && !can_gap_force && !can_gap_emergency) {
                    continue;
                }

                std::string mode_str = endgame_override ? "endgame-override"
                    : (can_gap_emergency ? "endgame-gap-emergency"
                    : (can_gap_force ? fmt::format("endgame-gap-cap@{:.2f}", gap_hedge_cap_for(secs_left, gap))
                        : (can_profit ? "endgame-profit"
                            : (can_ladder
                                ? fmt::format("endgame-ladder@{:.2f}", ladder_max)
                                : "endgame-gap"))));
                if (auto act = try_light_hedge(step, false, mode_str.c_str())) return act;
                continue;
            }

            const double budget_step = cap_shares_budget(leg1_shares_, max_leg_usdc, light_ask);

            if (open_gap_mode_
                && marginal <= gap_hedge_cap_for(secs_left, gap) + kFloatTol) {
                if (auto act = try_light_hedge(light_clip_shares(), false, "open-gap")) {
                    return act;
                }
            }

            if (at_target || (force && !open_gap_mode_)) {
                const char* mode = at_target ? "hedge" : "force";
                if (auto act = try_light_hedge(gap, force, mode)) return act;
                if (!force && budget_step > kFloatTol) {
                    if (auto act = try_light_hedge(budget_step, false, mode)) return act;
                }
                continue;
            }

            const bool at_mid_soft = in_mid_soft_window
                && marginal <= mid_soft_cap_ + kFloatTol;
            if (at_mid_soft) {
                if (auto act = try_light_hedge(gap, false, "mid-soft")) return act;
                if (budget_step > kFloatTol) {
                    if (auto act = try_light_hedge(budget_step, false, "mid-soft")) return act;
                }
                continue;
            }

            if (flex_rebalance_) {
                if (matched <= kFloatTol && (at_target || force) && budget_step > kFloatTol) {
                    const char* mode = force ? "force" : "flex-hedge";
                    if (auto act = try_light_hedge(budget_step, force, mode)) return act;
                    continue;
                }
                // Mid-window flex: light-leg hedge only when sum≤target (README 利润对冲).
                if (matched > kFloatTol && budget_step > kFloatTol) {
                    if (auto act = try_light_hedge(budget_step, false, "flex-hedge")) return act;
                }
                continue;
            }

            if (open_gap_mode_) {
                if (max_gap_shares_ > kFloatTol && gap > max_gap_shares_ + kFloatTol) {
                    const double excess = gap - max_gap_shares_;
                    const double step = std::min({light_clip_shares(), excess, gap});
                    const char* mode = marginal <= gap_hedge_cap_for(secs_left, gap) + kFloatTol
                        ? "gap-cap" : "gap-cap-force";
                    if (auto act = try_light_hedge(step, false, mode)) {
                        return act;
                    }
                }
                continue;
            }
            if (!allow_over_target_) continue;
            if (auto act = try_light_hedge(gap, true, "over-target")) return act;
            continue;
        }

        // Paired scale/dilute removed from mid-window — 压成本仅在末段有 gap 时通过
        // endgame try_light_hedge + 软顶完成，不在平时对已配平仓位双边加仓。
    }
    return std::nullopt;
}

} // namespace trading
