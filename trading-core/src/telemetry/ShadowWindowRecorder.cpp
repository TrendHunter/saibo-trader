#include "ShadowWindowRecorder.h"
#include "../signals/RegimeGate.h"
#include "../risk/RiskManager.h"
#include <boost/json.hpp>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>

namespace trading {

namespace {
constexpr double kFloatTol = 1e-6;
} // namespace

void ShadowWindowRecorder::set_asset_filter(std::string asset, int window_minutes) {
    std::transform(asset.begin(), asset.end(), asset.begin(), ::tolower);
    asset_filter_ = std::move(asset);
    window_minutes_filter_ = window_minutes;
}

bool ShadowWindowRecorder::matches_filter(const MarketInfo& market) const {
    if (!enabled_ || !store_) return false;
    std::string asset = market.asset;
    std::transform(asset.begin(), asset.end(), asset.begin(), ::tolower);
    return asset == asset_filter_ && market.window_minutes == window_minutes_filter_;
}

std::string ShadowWindowRecorder::slot_key(const MarketInfo& market) const {
    return market.asset + "-" + std::to_string(market.window_minutes);
}

int64_t ShadowWindowRecorder::window_start_ts_for(const MarketInfo& market) const {
    return static_cast<int64_t>(
        market.end_date_ts - static_cast<double>(market.window_minutes) * 60.0);
}

bool ShadowWindowRecorder::window_finalized(int64_t start) const {
    return start > 0 && finalized_ts_.count(start) > 0;
}

std::string ShadowWindowRecorder::iso_utc(int64_t ts) {
    time_t t = static_cast<time_t>(ts);
    tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &t);
#else
    gmtime_r(&t, &utc);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buf;
}

std::string ShadowWindowRecorder::normalize_skip_reason(const char* raw) {
    if (!raw || !*raw) return "unknown";
    const std::string r(raw);
    if (r.find("session off") != std::string::npos) return "session_off";
    if (r.find("mm2 obs skip") != std::string::npos) {
        const std::string prefix = "mm2 obs skip ";
        const auto pos = r.find(prefix);
        if (pos != std::string::npos) {
            const std::string reason = r.substr(pos + prefix.size());
            if (!reason.empty()) return reason;
        }
        return "mm2_obs_skip";
    }
    if (r.find("flat book") != std::string::npos) return "flat_book";
    if (r.find("mm2 vol low") != std::string::npos) return "too_late";
    if (r.find("mm2 vol high") != std::string::npos) return "flat_book";
    if (r.find("mm2 choppy") != std::string::npos) return "flat_book";
    if (r.find("mm2 wait late") != std::string::npos) return "wait_late";
    if (r.find("mm2 replay no cue") != std::string::npos) return "replay_wait";
    if (r.find("mm2 replay wait") != std::string::npos) return "replay_wait";
    if (r.find("mm2 replay too late") != std::string::npos) return "replay_late";
    if (r.find("too close") != std::string::npos) return "too_late";
    if (r.find("late window") != std::string::npos) return "too_late";
    if (r.find("not favorite") != std::string::npos) return "flat_book";
    if (r.find("price band") != std::string::npos) return "flat_book";
    if (r.find("depth min") != std::string::npos) return "flat_book";
    if (r.find("depth below") != std::string::npos) return "flat_book";
    if (r.find("flat_book") != std::string::npos) return "flat_book";
    if (r.find("marginal too wide") != std::string::npos) return "wide_spread";
    if (r.find("no quote") != std::string::npos) return "no_quote";
    if (r.find("no spot") != std::string::npos) return "no_spot";
    if (r.find("residual window") != std::string::npos) return "residual";
    if (r.find("early window") != std::string::npos) return "early";
    if (r.find("in-flight") != std::string::npos) return "busy";
    if (r.find("slot busy") != std::string::npos) return "busy";
    if (r.find("other slot") != std::string::npos) return "busy";
    if (r.find("cooldown") != std::string::npos) return "cooldown";
    if (r.find("vwap") != std::string::npos) return "vwap_skip";
    if (r.find("regime") != std::string::npos || r.find("pre pause") != std::string::npos) {
        return "regime_pause";
    }
    if (r.find("regime ride") != std::string::npos) return "regime_ride";
    if (r.find("feasible") != std::string::npos) return "feasible_skip";
    return "gate_blocked";
}

std::string ShadowWindowRecorder::side_label(bool buy_yes) {
    return buy_yes ? "Up" : "Down";
}

std::optional<double> ShadowWindowRecorder::spot_ret_bps(
    const MarketInfo& market, double window_total_sec, double elapsed_sec) const {
    if (!store_ || elapsed_sec <= kFloatTol) return std::nullopt;
    const std::string asset = market.asset;
    const auto open_px = store_->get_price_at(asset, elapsed_sec);
    const PriceTick latest = store_->get_latest_price(asset);
    if (!open_px || latest.price <= kFloatTol || *open_px <= kFloatTol) return std::nullopt;
    return (latest.price - *open_px) / *open_px * 10000.0;
}

void ShadowWindowRecorder::fill_snap(
    SnapFields& snap, const MarketInfo& market, double elapsed_sec,
    double yes_ask, double no_ask) const {
    snap.taken = true;
    snap.yes_ask = yes_ask;
    snap.no_ask = no_ask;
    if (store_) {
        const auto yd = store_->get_detection_ask(market.yes_token_id);
        const auto nd = store_->get_detection_ask(market.no_token_id);
        if (yd && yd->rest_ok) snap.yes_depth = yd->rest_depth_shares;
        if (nd && nd->rest_ok) snap.no_depth = nd->rest_depth_shares;
        const PriceTick latest = store_->get_latest_price(market.asset);
        if (latest.price > kFloatTol) snap.btc_spot = latest.price;
    }
    const double wm = market.window_minutes * 60.0;
    if (auto bps = spot_ret_bps(market, wm, elapsed_sec)) snap.spot_bps = *bps;
    if (yes_ask + kFloatTol >= no_ask) {
        snap.expensive_side = "Up";
        snap.expensive_ask = yes_ask;
    } else {
        snap.expensive_side = "Down";
        snap.expensive_ask = no_ask;
    }
}

ShadowWindowRecorder::WindowState& ShadowWindowRecorder::ensure_window(
    const MarketInfo& market, double now_sec) const {
    const double wm = market.window_minutes * 60.0;
    const int64_t start = static_cast<int64_t>(market.end_date_ts - wm);
    const int64_t end = static_cast<int64_t>(market.end_date_ts);
    const std::string key = slot_key(market);
    auto it = active_.find(key);
    if (it != active_.end() && it->second.window_start_ts == start) {
        return it->second;
    }
    if (it != active_.end()) {
        if (!window_finalized(it->second.window_start_ts)) {
            finalize(it->second);
        }
        active_.erase(it);
    }
    WindowState w;
    w.window_start_ts = start;
    w.window_end_ts = end;
    w.asset = market.asset;
    w.window_minutes = market.window_minutes;
    if (store_) {
        const auto open_px = store_->get_price_at(market.asset, wm);
        if (open_px) w.spot_open = *open_px;
        else {
            const PriceTick latest = store_->get_latest_price(market.asset);
            if (latest.price > kFloatTol) w.spot_open = latest.price;
        }
    }
    auto [ins, _] = active_.emplace(key, std::move(w));
    return ins->second;
}

void ShadowWindowRecorder::maybe_snapshots(
    WindowState& w, const MarketInfo& market, double now_sec,
    double yes_ask, double no_ask) const {
    if (yes_ask <= kFloatTol || no_ask <= kFloatTol) return;
    const double elapsed = now_sec - static_cast<double>(w.window_start_ts);
    for (std::size_t i = 0; i < WindowState::kSnapCount; ++i) {
        const int offset = WindowState::kSnapOffsets[i];
        if (w.snaps[i].taken) continue;
        if (elapsed + 2.0 >= static_cast<double>(offset)) {
            fill_snap(w.snaps[i], market, elapsed, yes_ask, no_ask);
        }
    }
}

void ShadowWindowRecorder::tick(
    const MarketInfo& market, double now_sec, double yes_ask, double no_ask) const {
    if (!matches_filter(market)) return;
    const int64_t start = window_start_ts_for(market);
    if (window_finalized(start)) return;

    auto& w = ensure_window(market, now_sec);
    maybe_snapshots(w, market, now_sec, yes_ask, no_ask);

    if (now_sec >= static_cast<double>(w.window_end_ts) - kFloatTol && !w.traded) {
        finalize(w);
        active_.erase(slot_key(market));
    }
}

void ShadowWindowRecorder::record_skip(
    const MarketInfo& market, double now_sec, const char* reason,
    double yes_ask, double no_ask) const {
    if (!matches_filter(market)) return;
    const int64_t start = window_start_ts_for(market);
    if (window_finalized(start)) return;

    auto& w = ensure_window(market, now_sec);
    w.last_skip_reason = normalize_skip_reason(reason);
    if (!w.traded) w.skip_reason = w.last_skip_reason;
    maybe_snapshots(w, market, now_sec, yes_ask, no_ask);
}

void ShadowWindowRecorder::on_leg1(
    const risk::LegInHedgePosition& pos, bool buy_yes, double price, double shares,
    double now_sec) {
    if (!enabled_ || !pos.is_shadow) return;
    if (pos.window_minutes != window_minutes_filter_) return;
    std::string asset = pos.asset;
    std::transform(asset.begin(), asset.end(), asset.begin(), ::tolower);
    if (asset != asset_filter_) return;

    const int64_t start = static_cast<int64_t>(
        pos.end_date_ts - static_cast<double>(pos.window_minutes) * 60.0);
    if (window_finalized(start)) return;

    const std::string key = asset + "-" + std::to_string(pos.window_minutes);
    auto it = active_.find(key);
    if (it == active_.end() || it->second.window_start_ts != start) {
        WindowState w;
        w.window_start_ts = start;
        w.window_end_ts = static_cast<int64_t>(pos.end_date_ts);
        w.asset = pos.asset;
        w.window_minutes = pos.window_minutes;
        it = active_.emplace(key, std::move(w)).first;
    }
    WindowState& st = it->second;
    st.traded = true;
    st.skip_reason = "traded";
    st.leg1_side = side_label(buy_yes);
    st.leg1_px = price;
    st.leg1_shares = shares;
    st.leg1_sec_in = now_sec - static_cast<double>(start);
    st.total_fills = std::max(st.total_fills, 1);
}

void ShadowWindowRecorder::on_hedge_fill(const risk::LegInHedgePosition& pos) {
    if (!enabled_ || !pos.is_shadow) return;
    std::string asset = pos.asset;
    std::transform(asset.begin(), asset.end(), asset.begin(), ::tolower);
    if (asset != asset_filter_ || pos.window_minutes != window_minutes_filter_) return;
    const int64_t start = static_cast<int64_t>(
        pos.end_date_ts - static_cast<double>(pos.window_minutes) * 60.0);
    if (window_finalized(start)) return;

    const std::string key = asset + "-" + std::to_string(pos.window_minutes);
    auto it = active_.find(key);
    if (it == active_.end() || it->second.window_start_ts != start) return;
    ++it->second.total_fills;
}

void ShadowWindowRecorder::on_closed(
    const risk::LegInHedgePosition& pos, double pnl_usdc) {
    if (!enabled_ || !pos.is_shadow) return;
    std::string asset = pos.asset;
    std::transform(asset.begin(), asset.end(), asset.begin(), ::tolower);
    if (asset != asset_filter_ || pos.window_minutes != window_minutes_filter_) return;

    const int64_t start = static_cast<int64_t>(
        pos.end_date_ts - static_cast<double>(pos.window_minutes) * 60.0);
    if (window_finalized(start)) return;

    const std::string key = asset + "-" + std::to_string(pos.window_minutes);
    WindowState st;
    auto it = active_.find(key);
    if (it != active_.end() && it->second.window_start_ts == start) {
        st = it->second;
        active_.erase(it);
    } else {
        st.window_start_ts = start;
        st.window_end_ts = static_cast<int64_t>(pos.end_date_ts);
        st.asset = pos.asset;
        st.window_minutes = pos.window_minutes;
    }

    st.traded = true;
    st.skip_reason = "traded";
    st.total_round_pnl = pnl_usdc;
    st.final_gap = std::abs(pos.yes_shares - pos.no_shares);
    if (st.leg1_side.empty()) {
        if (pos.yes_shares > pos.no_shares + kFloatTol) st.leg1_side = "Up";
        else if (pos.no_shares > pos.yes_shares + kFloatTol) st.leg1_side = "Down";
    }
    if (pos.yes_shares > pos.no_shares + kFloatTol) st.winning_side = "Up";
    else if (pos.no_shares > pos.yes_shares + kFloatTol) st.winning_side = "Down";
    if (st.total_fills <= 0) st.total_fills = std::max(1, pos.rebalance_count + 1);

    finalize(st);
}

void ShadowWindowRecorder::update_regime(
    const MarketInfo& market, double now_sec, const RegimeDecision& decision) const {
    if (!matches_filter(market)) return;
    const int64_t start = window_start_ts_for(market);
    if (window_finalized(start)) return;
    auto& w = ensure_window(market, now_sec);
    w.regime_state = decision.state_str();
    w.regime_reason = decision.reason;
    w.regime_score_b = decision.score_b;
    w.regime_score_c = decision.score_c;
    w.regime_pre_reason = decision.pre_reason;
}

void ShadowWindowRecorder::flush_expired(double now_sec) const {
    if (!enabled_) return;
    std::vector<std::string> done;
    for (auto& [key, w] : active_) {
        if (now_sec < static_cast<double>(w.window_end_ts) - kFloatTol) continue;
        if (window_finalized(w.window_start_ts)) {
            done.push_back(key);
            continue;
        }
        if (!w.traded) {
            if (!w.last_skip_reason.empty()) w.skip_reason = w.last_skip_reason;
            else if (w.skip_reason == "no_attempt") w.skip_reason = "no_quote";
        }
        finalize(w);
        done.push_back(key);
    }
    for (const auto& k : done) active_.erase(k);
}

void ShadowWindowRecorder::finalize(WindowState& w) const {
    if (w.window_start_ts <= 0) return;
    if (window_finalized(w.window_start_ts)) return;

    if (!w.traded) {
        if (!w.last_skip_reason.empty()) w.skip_reason = w.last_skip_reason;
        else if (w.skip_reason == "no_attempt") w.skip_reason = "unknown";
    }

    write_row(w);
    finalized_ts_.insert(w.window_start_ts);
    w.window_start_ts = 0;
}

void ShadowWindowRecorder::write_row(const WindowState& w) const {
    try {
        boost::json::object row;
        row["event"] = "WINDOW";
        row["schema_version"] = "bot-window-v1";
        row["account"] = "bot-shadow";
        row["asset"] = w.asset;
        row["window_minutes"] = w.window_minutes;
        row["window_start_ts"] = w.window_start_ts;
        row["window_end_ts"] = w.window_end_ts;
        row["window_start_utc"] = iso_utc(w.window_start_ts);
        row["bot_traded"] = w.traded;
        row["skip_reason"] = w.skip_reason;
        row["regime_state"] = w.regime_state;
        if (!w.regime_reason.empty()) row["regime_reason"] = w.regime_reason;
        row["regime_score_b"] = w.regime_score_b;
        row["regime_score_c"] = w.regime_score_c;
        if (!w.regime_pre_reason.empty()) row["regime_pre_reason"] = w.regime_pre_reason;
        if (w.traded) {
            row["leg1_side"] = w.leg1_side;
            row["leg1_px"] = w.leg1_px;
            row["leg1_shares"] = w.leg1_shares;
            row["leg1_sec_in"] = w.leg1_sec_in;
            row["total_fills"] = w.total_fills;
            row["total_round_pnl"] = w.total_round_pnl;
            row["final_gap"] = w.final_gap;
            if (!w.winning_side.empty()) row["winning_side"] = w.winning_side;
        }
        for (std::size_t i = 0; i < WindowState::kSnapCount; ++i) {
            const auto& s = w.snaps[i];
            if (!s.taken) continue;
            const int offset = WindowState::kSnapOffsets[i];
            const std::string p = "snap_" + std::to_string(offset) + "_";
            row[p + "yes_ask"] = s.yes_ask;
            row[p + "no_ask"] = s.no_ask;
            row[p + "ask_sum"] = s.yes_ask + s.no_ask;
            row[p + "spread"] = (s.yes_ask + s.no_ask) - 1.0;
            row[p + "yes_depth"] = s.yes_depth;
            row[p + "no_depth"] = s.no_depth;
            row[p + "btc_spot"] = s.btc_spot;
            row[p + "spot_bps"] = s.spot_bps;
            if (!s.expensive_side.empty()) row[p + "expensive_side"] = s.expensive_side;
            row[p + "expensive_ask"] = s.expensive_ask;
        }
        auto duration = std::chrono::system_clock::now().time_since_epoch();
        row["recorded_ts"] = std::chrono::duration<double>(duration).count();

        std::filesystem::create_directories("logs");
        std::ofstream out("logs/shadow_windows.jsonl", std::ios::app);
        if (!out) return;
        out << boost::json::serialize(row) << '\n';
    } catch (const std::exception& e) {
        spdlog::warn("ShadowWindowRecorder write failed: {}", e.what());
    }
}

} // namespace trading
