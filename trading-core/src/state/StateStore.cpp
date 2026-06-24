#include "StateStore.h"
#include <mutex>
#include <fstream>
#include <unordered_map>
#include <boost/json.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>

namespace trading {

void StateStore::set_token_fee_params(const std::string& token_id, double rate, double exponent) {
    std::unique_lock lock(token_mutex_);
    token_fee_params_[token_id] = TokenFeeParams{rate, exponent, rate > 0.0};
}

StateStore::TokenFeeParams StateStore::get_token_fee_params(std::string_view token_id) const {
    std::shared_lock lock(token_mutex_);
    auto it = token_fee_params_.find(std::string(token_id));
    if (it != token_fee_params_.end()) return it->second;
    return {};
}

namespace {
std::string normalize_asset_key(std::string asset) {
    std::transform(asset.begin(), asset.end(), asset.begin(), ::tolower);
    return asset;
}
} // namespace

void StateStore::set_dh_asset_enabled(int window_minutes, const std::string& asset, bool enabled) {
    const std::string a = normalize_asset_key(asset);
    if (window_minutes == 5) {
        if (a == "btc") dh_5m_btc_ = enabled;
        else if (a == "eth") dh_5m_eth_ = enabled;
        else if (a == "sol") dh_5m_sol_ = enabled;
    } else if (window_minutes == 15) {
        if (a == "btc") dh_15m_btc_ = enabled;
        else if (a == "eth") dh_15m_eth_ = enabled;
    }
}

bool StateStore::dh_asset_enabled(int window_minutes, const std::string& asset) const {
    const std::string a = normalize_asset_key(asset);
    if (window_minutes == 5) {
        if (a == "btc") return dh_5m_btc_;
        if (a == "eth") return dh_5m_eth_;
        if (a == "sol") return dh_5m_sol_;
    } else if (window_minutes == 15) {
        if (a == "btc") return dh_15m_btc_;
        if (a == "eth") return dh_15m_eth_;
    }
    return true;
}

void StateStore::push_telemetry(const std::string& line) {
    std::unique_lock lock(log_mutex_);
    telemetry_log_.push_back(line);
    if (telemetry_log_.size() > MAX_LOG_LINES) telemetry_log_.pop_front();
}

void StateStore::push_signal(const std::string& line) {
    std::unique_lock lock(log_mutex_);
    signal_log_.push_back(line);
    if (signal_log_.size() > MAX_LOG_LINES) signal_log_.pop_front();
}

void StateStore::update_btc_price(const PriceTick& tick) {
    std::unique_lock lock(btc_mutex_);
    latest_btc_tick_ = tick;
    btc_tick_count_++;
    btc_history_.push_back(tick);
    if (btc_history_.size() > 5000) btc_history_.pop_front();
}

std::optional<PriceTick> StateStore::get_latest_btc_price() const {
    std::shared_lock lock(btc_mutex_);
    return latest_btc_tick_;
}

void StateStore::update_eth_price(const PriceTick& tick) {
    std::unique_lock lock(eth_mutex_);
    latest_eth_tick_ = tick;
    eth_tick_count_++;
    eth_history_.push_back(tick);
    if (eth_history_.size() > 5000) eth_history_.pop_front();
}

std::optional<PriceTick> StateStore::get_latest_eth_price() const {
    std::shared_lock lock(eth_mutex_);
    return latest_eth_tick_;
}

void StateStore::update_sol_price(const PriceTick& tick) {
    std::unique_lock lock(sol_mutex_);
    latest_sol_tick_ = tick;
    sol_tick_count_++;
    sol_history_.push_back(tick);
    if (sol_history_.size() > 5000) sol_history_.pop_front();
}

std::optional<PriceTick> StateStore::get_latest_sol_price() const {
    std::shared_lock lock(sol_mutex_);
    return latest_sol_tick_;
}

std::optional<double> StateStore::get_price_at(const std::string& asset, double seconds_ago) const {
    const std::deque<PriceTick>* history = nullptr;
     std::shared_mutex* mutex = nullptr;

    if (asset == "eth") { history = &eth_history_; mutex = &eth_mutex_; }
    else if (asset == "sol") { history = &sol_history_; mutex = &sol_mutex_; }
    else { history = &btc_history_; mutex = &btc_mutex_; }

    std::shared_lock lock(*mutex);
    if (history->empty()) return std::nullopt;
    
    double now = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    double target = now - seconds_ago;
    
    for (auto it = history->rbegin(); it != history->rend(); ++it) {
        if (it->received_at <= target) return it->price;
    }
    return history->front().price;
}

PriceTick StateStore::get_latest_price(const std::string& asset) const {
    if (asset == "eth") { std::shared_lock lock(eth_mutex_); return latest_eth_tick_; }
    else if (asset == "sol") { std::shared_lock lock(sol_mutex_); return latest_sol_tick_; }
    else { std::shared_lock lock(btc_mutex_); return latest_btc_tick_; }
}

void StateStore::update_token_price(std::string_view token_id, const TokenPrice& price) {
    std::unique_lock lock(token_mutex_);
    token_prices_[std::string(token_id)] = price;
}

std::optional<TokenPrice> StateStore::get_token_price(std::string_view token_id) const {
    std::shared_lock lock(token_mutex_);
    auto it = token_prices_.find(std::string(token_id));
    if (it != token_prices_.end()) return it->second;
    return std::nullopt;
}

void StateStore::update_ws_book_ask(std::string_view token_id, const TokenPrice& price) {
    std::unique_lock lock(token_mutex_);
    ws_book_asks_[std::string(token_id)] = price;
}

void StateStore::update_rest_book_ask(std::string_view token_id, double price, double depth_shares) {
    std::unique_lock lock(token_mutex_);
    const std::string key(token_id);
    TokenPrice tp;
    tp.price = price;
    tp.side = "BUY";
    tp.ts = std::chrono::duration<double>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    rest_book_asks_[key] = tp;
    rest_book_depth_[key] = depth_shares;
}

void StateStore::update_rest_ask_ladder(std::string_view token_id, std::vector<BookLevel> levels) {
    std::unique_lock lock(token_mutex_);
    const std::string key(token_id);
    std::sort(levels.begin(), levels.end(),
              [](const BookLevel& a, const BookLevel& b) { return a.price < b.price; });
    rest_ask_ladders_[key] = std::move(levels);
}

StateStore::WalkFillResult StateStore::walk_ask_fill(
    std::string_view token_id, double max_shares, double rest_max_age_sec) const {
    WalkFillResult out;
    if (max_shares <= 0.0) return out;

    std::shared_lock lock(token_mutex_);
    const std::string key(token_id);
    const double now = std::chrono::duration<double>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    const double max_age = paper_realism_enabled_ ? paper_book_max_age_secs_ : rest_max_age_sec;

    auto rest = rest_book_asks_.find(key);
    if (rest == rest_book_asks_.end() || rest->second.price <= 0.0
        || (now - rest->second.ts) > max_age) {
        return out;
    }

    std::vector<BookLevel> ladder;
    auto lad = rest_ask_ladders_.find(key);
    if (lad != rest_ask_ladders_.end() && !lad->second.empty()) {
        ladder = lad->second;
    } else {
        auto dep = rest_book_depth_.find(key);
        const double depth = dep != rest_book_depth_.end() ? dep->second : 0.0;
        if (depth > 0.0) {
            ladder.push_back({rest->second.price, depth});
        }
    }
    if (ladder.empty()) return out;

    const double take_ratio = paper_realism_enabled_
        ? std::clamp(paper_liquidity_take_ratio_, 0.05, 1.0) : 1.0;
    const double slip = paper_slippage_pct_;
    double remaining = max_shares;
    for (const auto& level : ladder) {
        if (level.price <= 0.0 || level.size <= 0.0) continue;
        const double px = level.price * (1.0 + slip);
        const double avail = level.size * take_ratio;
        const double take = std::min(remaining, avail);
        out.cost_usdc += take * px;
        out.shares += take;
        ++out.levels_used;
        remaining -= take;
        if (remaining <= 1e-6) break;
    }
    if (out.shares > 0.0) {
        out.avg_price = out.cost_usdc / out.shares;
    }
    if (paper_realism_enabled_ && paper_min_fill_ratio_ > 0.0 && max_shares > 0.0
        && out.shares + 1e-6 < max_shares * paper_min_fill_ratio_) {
        return {};
    }
    return out;
}

StateStore::WalkFillResult StateStore::walk_paired_fill(
    std::string_view yes_token, std::string_view no_token,
    double max_shares, double balance_usdc, double balance_reserve,
    double rest_max_age_sec) const {
    WalkFillResult out;
    if (max_shares <= 0.0 || balance_usdc <= 0.0) return out;

    const double budget = balance_usdc * balance_reserve;
    auto paired_cost = [&](double target_shares) -> double {
        if (target_shares <= 0.0) return 0.0;
        const auto yes_f = walk_ask_fill(yes_token, target_shares, rest_max_age_sec);
        const auto no_f = walk_ask_fill(no_token, target_shares, rest_max_age_sec);
        const double paired = std::min(yes_f.shares, no_f.shares);
        if (paired <= 0.0) return 1e18;
        const auto yes_final = walk_ask_fill(yes_token, paired, rest_max_age_sec);
        const auto no_final = walk_ask_fill(no_token, paired, rest_max_age_sec);
        return yes_final.cost_usdc + no_final.cost_usdc;
    };

    double lo = 0.0;
    double hi = max_shares;
    for (int i = 0; i < 24; ++i) {
        const double mid = (lo + hi) * 0.5;
        if (paired_cost(mid) <= budget + 1e-6) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    if (lo <= 1e-6) return out;

    const auto yes_f = walk_ask_fill(yes_token, lo, rest_max_age_sec);
    const auto no_f = walk_ask_fill(no_token, lo, rest_max_age_sec);
    out.shares = std::min(yes_f.shares, no_f.shares);
    if (out.shares <= 0.0) return out;

    const auto yes_final = walk_ask_fill(yes_token, out.shares, rest_max_age_sec);
    const auto no_final = walk_ask_fill(no_token, out.shares, rest_max_age_sec);
    out.shares = std::min(yes_final.shares, no_final.shares);
    if (out.shares <= 0.0) return out;

    const auto yes_done = walk_ask_fill(yes_token, out.shares, rest_max_age_sec);
    const auto no_done = walk_ask_fill(no_token, out.shares, rest_max_age_sec);
    out.cost_usdc = yes_done.cost_usdc + no_done.cost_usdc;
    out.levels_used = yes_done.levels_used + no_done.levels_used;
    if (out.shares > 0.0 && out.cost_usdc > 0.0) {
        out.avg_price = out.cost_usdc / out.shares;
    }
    return out;
}

void StateStore::update_rest_book_bid(std::string_view token_id, double price) {
    std::unique_lock lock(token_mutex_);
    const std::string key(token_id);
    TokenPrice tp;
    tp.price = price;
    tp.side = "SELL";
    tp.ts = std::chrono::duration<double>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    rest_book_bids_[key] = tp;
}

std::optional<double> StateStore::get_official_mark_bid(
    std::string_view token_id, double rest_max_age_sec) const {
    std::shared_lock lock(token_mutex_);
    const std::string key(token_id);
    const double now = std::chrono::duration<double>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    auto rest = rest_book_bids_.find(key);
    if (rest != rest_book_bids_.end() && rest->second.price > 0.0
        && (now - rest->second.ts) <= rest_max_age_sec) {
        return rest->second.price;
    }
    auto ws = token_bids_.find(key);
    if (ws != token_bids_.end() && ws->second.price > 0.0) {
        return ws->second.price;
    }
    return std::nullopt;
}

std::optional<double> StateStore::get_official_buy_ask(
    std::string_view token_id, double rest_max_age_sec) const {
    std::shared_lock lock(token_mutex_);
    const std::string key(token_id);
    const double now = std::chrono::duration<double>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    auto rest = rest_book_asks_.find(key);
    if (rest != rest_book_asks_.end() && rest->second.price > 0.0
        && (now - rest->second.ts) <= rest_max_age_sec) {
        return rest->second.price;
    }
    auto ws = ws_book_asks_.find(key);
    if (ws != ws_book_asks_.end() && ws->second.price > 0.0) {
        return ws->second.price;
    }
    auto ask = token_prices_.find(key);
    if (ask != token_prices_.end() && ask->second.price > 0.0) {
        return ask->second.price;
    }
    return std::nullopt;
}

std::optional<StateStore::DetectionAsk> StateStore::get_detection_ask(
    std::string_view token_id, double rest_max_age_sec) const {
    std::shared_lock lock(token_mutex_);
    const std::string key(token_id);
    const double now = std::chrono::duration<double>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    DetectionAsk out;
    auto ws_it = ws_book_asks_.find(key);
    if (ws_it != ws_book_asks_.end() && ws_it->second.price > 0.0) {
        out.ws_book_ask = ws_it->second.price;
        out.ws_ok = (now - ws_it->second.ts) <= 2.0;
    }
    auto rest_it = rest_book_asks_.find(key);
    if (rest_it != rest_book_asks_.end() && rest_it->second.price > 0.0) {
        out.rest_book_ask = rest_it->second.price;
        out.rest_ok = (now - rest_it->second.ts) <= rest_max_age_sec;
        auto dep = rest_book_depth_.find(key);
        if (dep != rest_book_depth_.end()) out.rest_depth_shares = dep->second;
    }

    if (!book_aware_detect_) {
        auto ask = token_prices_.find(key);
        if (ask != token_prices_.end() && ask->second.price > 0.0) {
            out.conservative_ask = ask->second.price;
            out.ws_ok = true;
            return out;
        }
        return std::nullopt;
    }

    if (out.ws_ok && out.rest_ok) {
        out.conservative_ask = std::max(out.ws_book_ask, out.rest_book_ask);
    } else if (out.rest_ok) {
        out.conservative_ask = out.rest_book_ask;
    } else if (out.ws_ok) {
        out.conservative_ask = out.ws_book_ask;
    } else {
        return std::nullopt;
    }
    return out;
}

void StateStore::update_markets(const std::vector<MarketInfo>& markets) {
    std::unique_lock lock(market_mutex_);
    markets_ = markets;
}

std::string StateStore::get_dashboard_json() const {
    boost::json::object root;
    
    auto add_asset_data = [&](const std::string& sym, double price, uint64_t count) {
        boost::json::object obj;
        obj["price"] = price;
        obj["count"] = count;
        auto p27 = get_price_at(sym, 2.7);
        auto p60 = get_price_at(sym, 60.0);
        obj["delta27"] = p27 ? (price - *p27) : 0.0;
        obj["delta60"] = p60 ? (price - *p60) : 0.0;
        root[sym + "Data"] = std::move(obj);
        root[sym + "Price"] = price; // Compat
    };

    { 
        std::shared_lock lock(btc_mutex_); 
        double p = latest_btc_tick_.price; 
        uint64_t c = btc_tick_count_; 
        lock.unlock(); // Release lock before calling helpers that take the lock
        add_asset_data("btc", p, c); 
    }
    { 
        std::shared_lock lock(eth_mutex_); 
        double p = latest_eth_tick_.price; 
        uint64_t c = eth_tick_count_; 
        lock.unlock();
        add_asset_data("eth", p, c); 
    }
    { 
        std::shared_lock lock(sol_mutex_); 
        double p = latest_sol_tick_.price; 
        uint64_t c = sol_tick_count_; 
        lock.unlock();
        add_asset_data("sol", p, c); 
    }

    root["strategy"] = strategy_.c_str();
    root["dhEnable5m"] = dh_enable_5m_;
    root["dhEnable15m"] = dh_enable_15m_;
    root["dhEnable5mBtc"] = dh_5m_btc_;
    root["dhEnable5mEth"] = dh_5m_eth_;
    root["dhEnable5mSol"] = dh_5m_sol_;
    root["dhEnable15mBtc"] = dh_15m_btc_;
    root["dhEnable15mEth"] = dh_15m_eth_;
    root["binanceFeedEnabled"] = binance_feed_enabled_;
    root["lihEnabled"] = lih_enabled_;
    root["lihLeg1MaxPrice"] = lih_leg1_max_price_;
    root["lihTargetCombined"] = lih_target_combined_;
    root["lihUseMirror"] = lih_use_mirror_;
    root["liveLihDryRun"] = live_lih_dry_run_;
    if (trades_baseline_ts_ > 0) root["tradesBaselineTs"] = trades_baseline_ts_;
    {
        std::shared_lock lock(mirror_mutex_);
        root["mirrorLoadedAt"] = mirror_loaded_at_;
        root["mirrorAssetCount"] = static_cast<int64_t>(mirror_by_asset_.size());
    }
    
    if (risk_manager_) {
        double balance = risk_manager_->get_current_balance();
        double start = risk_manager_->get_starting_balance();
        double daily_start = risk_manager_->get_daily_starting_balance();
        double peak = risk_manager_->get_peak_balance();
        double daily_pnl = balance - daily_start;
        double total_pnl = balance - start;
        if (!paper_mode_ && trades_baseline_ts_ > 0) {
            total_pnl = risk_manager_->get_lih_pnl() + risk_manager_->get_la_pnl();
        }
        double drawdown = peak > 0 ? (peak - balance) / peak * 100.0 : 0.0;

        root["balance"] = balance;
        root["dailyStartingBalance"] = daily_start;
        root["peakBalance"] = peak;
        root["dailyPnl"] = daily_pnl;
        root["totalPnl"] = total_pnl;
        root["maxDrawdownPct"] = drawdown;
        root["openCount"] = risk_manager_->get_open_position_count();
        root["totalTrades"] = risk_manager_->get_total_trades();
        root["totalDhTrades"] = 0;
        root["totalLihTrades"] = risk_manager_->get_total_lih_trades();
        root["winRate"] = risk_manager_->get_win_rate() * 100.0;
        root["laPnl"] = risk_manager_->get_la_pnl();
        root["dhPnl"] = 0.0;
        root["lihPnl"] = risk_manager_->get_lih_pnl();
        root["status"] = static_cast<int>(risk_manager_->get_status());
        if (auto reason = risk_manager_->get_status_reason()) {
            root["statusReason"] = reason->c_str();
        } else {
            root["statusReason"] = "";
        }
        root["isPaperMode"] = paper_mode_;
        root["startingBalance"] = start;
        root["feeRate"] = fee_rate_;
        root["feeModel"] = "polymarket_v2_curve";
        root["useDynamicFees"] = [&]() {
            std::shared_lock lock(token_mutex_);
            for (const auto& [_, p] : token_fee_params_) {
                if (p.from_api) return true;
            }
            return false;
        }();
        root["riskMaxPositionFraction"] = risk_manager_->get_max_position_fraction();
        root["riskDailyLossLimit"] = risk_manager_->get_daily_loss_limit();
        root["riskTotalDrawdownKill"] = risk_manager_->get_total_drawdown_kill();
        root["riskMaxConcurrentPositions"] = risk_manager_->get_max_concurrent_positions();
        root["lihOneSlotGlobal"] = risk_manager_->get_lih_one_slot_global();
        root["lihSessionMaxLegs"] = risk_manager_->get_lih_session_max_legs();
        root["lihSessionLegsUsed"] = risk_manager_->get_lih_session_legs_used();
        root["lihMinBalanceUsdc"] = risk_manager_->get_lih_min_balance_usdc();

        std::vector<MarketInfo> markets_snapshot;
        { std::shared_lock lock(market_mutex_); markets_snapshot = markets_; }
        std::unordered_map<std::string, TokenPrice> tokens_snapshot;
        std::unordered_map<std::string, TokenPrice> bids_snapshot;
        { std::shared_lock lock(token_mutex_); tokens_snapshot = token_prices_; bids_snapshot = token_bids_; }

        auto token_mark = [&](const std::string& tid) -> double {
            if (paper_mode_ && paper_official_book_) {
                if (auto bid = get_official_mark_bid(tid)) return *bid;
            }
            auto bid = bids_snapshot.find(tid);
            if (bid != bids_snapshot.end() && bid->second.price > 0.0) return bid->second.price;
            auto ask = tokens_snapshot.find(tid);
            return ask != tokens_snapshot.end() ? ask->second.price : 0.0;
        };
        auto token_buy = [&](const std::string& tid) -> double {
            if (paper_mode_ && paper_official_book_) {
                if (auto ask = get_official_buy_ask(tid)) return *ask;
            }
            auto ask = tokens_snapshot.find(tid);
            return ask != tokens_snapshot.end() ? ask->second.price : 0.0;
        };
        auto find_market_for_token = [&](const std::string& tid) -> const MarketInfo* {
            for (const auto& m : markets_snapshot) {
                if (m.yes_token_id == tid || m.no_token_id == tid) return &m;
            }
            return nullptr;
        };

        boost::json::array pos_arr;
        for (const auto& [id, p] : risk_manager_->get_open_positions()) {
            if (p.strategy == "LA") continue;
            boost::json::object po;
            po["asset"] = p.asset.c_str();
            po["side"] = p.side.c_str();
            po["entryPrice"] = p.entry_price;
            po["size"] = p.size_shares;
            po["cost"] = p.cost_usdc;
            po["strategy"] = p.strategy.c_str();
            po["question"] = p.market_question.c_str();
            po["endDateTs"] = p.end_date_ts;
            po["entryFee"] = p.cost_usdc * fee_rate_;

            const MarketInfo* m = find_market_for_token(p.token_id);
            bool is_yes = m && p.token_id == m->yes_token_id;
            po["heldSide"] = is_yes ? "YES" : "NO";
            po["direction"] = p.direction.empty() ? (is_yes ? "UP" : "DOWN") : p.direction.c_str();

            if (m) {
                po["yesLivePrice"] = token_mark(m->yes_token_id);
                po["noLivePrice"] = token_mark(m->no_token_id);
                po["yesBuyPrice"] = token_buy(m->yes_token_id);
                po["noBuyPrice"] = token_buy(m->no_token_id);
            } else {
                po["yesLivePrice"] = is_yes ? token_mark(p.token_id) : 0.0;
                po["noLivePrice"] = !is_yes ? token_mark(p.token_id) : 0.0;
                po["yesBuyPrice"] = is_yes ? token_buy(p.token_id) : 0.0;
                po["noBuyPrice"] = !is_yes ? token_buy(p.token_id) : 0.0;
            }

            if (is_yes) {
                po["yesEntryPrice"] = p.entry_price;
                po["yesSize"] = p.size_shares;
                po["yesCost"] = p.cost_usdc;
                po["noEntryPrice"] = 0.0;
                po["noSize"] = 0.0;
                po["noCost"] = 0.0;
            } else {
                po["noEntryPrice"] = p.entry_price;
                po["noSize"] = p.size_shares;
                po["noCost"] = p.cost_usdc;
                po["yesEntryPrice"] = 0.0;
                po["yesSize"] = 0.0;
                po["yesCost"] = 0.0;
            }

            auto live_px = token_mark(p.token_id);
            double unrealised = live_px > 0.0 ? (live_px - p.entry_price) * p.size_shares : 0.0;
            po["pnl"] = unrealised;
            pos_arr.push_back(po);
        }
        for (const auto& [id, p] : risk_manager_->get_open_lih_positions()) {
            if (!paper_mode_ && (p.paper_mode || p.is_shadow)) continue;
            boost::json::object po;
            const double yes_avg = p.yes_shares > 0 ? p.yes_cost / p.yes_shares : 0.0;
            const double no_avg = p.no_shares > 0 ? p.no_cost / p.no_shares : 0.0;
            const double matched = std::min(p.yes_shares, p.no_shares);
            po["asset"] = p.asset.c_str();
            po["side"] = "LIH";
            po["entryPrice"] = yes_avg + no_avg;
            po["size"] = matched > 0 ? matched : std::max(p.yes_shares, p.no_shares);
            po["cost"] = p.yes_cost + p.no_cost;
            po["strategy"] = live_lih_dry_run_ ? "LIH-SHADOW" : "LIH";
            po["isShadow"] = live_lih_dry_run_;
            po["windowMinutes"] = p.window_minutes;
            po["question"] = p.market_question.c_str();
            po["endDateTs"] = p.end_date_ts;
            po["heldSide"] = (p.yes_shares > p.no_shares + 1e-6) ? "YES"
                : (p.no_shares > p.yes_shares + 1e-6) ? "NO" : "BOTH";
            po["yesEntryPrice"] = yes_avg;
            po["noEntryPrice"] = no_avg;
            po["yesSize"] = p.yes_shares;
            po["noSize"] = p.no_shares;
            po["yesCost"] = p.yes_cost;
            po["noCost"] = p.no_cost;
            po["rebalanceCount"] = p.rebalance_count;
            po["gap"] = std::abs(p.yes_shares - p.no_shares);
            po["yesLivePrice"] = token_mark(p.yes_token_id);
            po["noLivePrice"] = token_mark(p.no_token_id);
            po["yesBuyPrice"] = token_buy(p.yes_token_id);
            po["noBuyPrice"] = token_buy(p.no_token_id);
            po["entryFee"] = (p.yes_cost + p.no_cost) * fee_rate_;
            const double excess_yes = std::max(0.0, p.yes_shares - matched);
            const double excess_no = std::max(0.0, p.no_shares - matched);
            double unrealised = 0.0;
            if (matched > 0.0) {
                unrealised += matched * (1.0 - yes_avg - no_avg);
            }
            if (excess_yes > 0.0 && po["yesLivePrice"].as_double() > 0.0) {
                unrealised += excess_yes * (po["yesLivePrice"].as_double() - yes_avg);
            }
            if (excess_no > 0.0 && po["noLivePrice"].as_double() > 0.0) {
                unrealised += excess_no * (po["noLivePrice"].as_double() - no_avg);
            }
            po["pnl"] = unrealised;
            pos_arr.push_back(po);
        }
        root["openPositions"] = std::move(pos_arr);

        // Structured trade history (open + closed, paper & live)
        struct HistRow { double sort_ts; boost::json::object obj; };
        std::vector<HistRow> hist_rows;
        const double fr = fee_rate_;
        const double baseline = trades_baseline_ts_;

        auto after_baseline = [&](double ts) {
            return baseline <= 0 || ts <= 0 || ts >= baseline;
        };

        auto push_hist = [&](HistRow row) { hist_rows.push_back(std::move(row)); };

        for (const auto& p : risk_manager_->get_closed_positions()) {
            const double ts = p.closed_at.value_or(p.opened_at);
            if (!after_baseline(ts)) continue;
            boost::json::object h;
            h["id"] = p.order_id.c_str();
            h["strategy"] = p.strategy.c_str();
            h["asset"] = p.asset.c_str();
            h["status"] = "closed";
            h["market"] = p.market_question.c_str();
            h["side"] = p.side.c_str();
            h["direction"] = p.direction.c_str();
            h["entryPrice"] = p.entry_price;
            h["exitPrice"] = p.exit_price.value_or(0.0);
            h["size"] = p.size_shares;
            h["costUsdc"] = p.cost_usdc;
            h["entryFee"] = p.cost_usdc * fr;
            h["exitFee"] = p.exit_price.value_or(0.0) * p.size_shares * fr;
            h["pnlUsdc"] = p.pnl_usdc.value_or(0.0);
            h["openedAt"] = p.opened_at;
            h["closedAt"] = p.closed_at.value_or(0.0);
            h["exitReason"] = "CLOSED";
            h["isPaperMode"] = p.paper_mode;
            push_hist({p.closed_at.value_or(p.opened_at), std::move(h)});
        }

        for (const auto& p : risk_manager_->get_closed_lih_positions()) {
            if (p.is_shadow) continue;
            const double ts = p.closed_at.value_or(p.opened_at);
            if (!after_baseline(ts)) continue;
            const double yes_avg = p.yes_entry_price > 0 ? p.yes_entry_price
                : (p.yes_shares > 0 ? p.yes_cost / p.yes_shares : 0.0);
            const double no_avg = p.no_entry_price > 0 ? p.no_entry_price
                : (p.no_shares > 0 ? p.no_cost / p.no_shares : 0.0);
            const double matched = std::min(p.yes_shares, p.no_shares);
            const double yes_exit = p.yes_exit_price.value_or(0.0);
            const double no_exit = p.no_exit_price.value_or(0.0);
            boost::json::object h;
            h["id"] = p.lih_id.c_str();
            h["strategy"] = "LIH";
            h["asset"] = p.asset.c_str();
            h["status"] = "closed";
            h["market"] = p.market_question.c_str();
            h["side"] = "LIH";
            h["direction"] = "LEG-IN";
            h["yesEntryPrice"] = yes_avg;
            h["noEntryPrice"] = no_avg;
            h["entryPrice"] = yes_avg + no_avg;
            h["yesExitPrice"] = yes_exit;
            h["noExitPrice"] = no_exit;
            h["exitPrice"] = yes_exit + no_exit;
            h["size"] = matched > 0 ? matched : std::max(p.yes_shares, p.no_shares);
            h["costUsdc"] = p.yes_cost + p.no_cost;
            h["entryFee"] = p.entry_fees > 0 ? p.entry_fees : (p.yes_cost + p.no_cost) * fr;
            const double gross = yes_exit * p.yes_shares + no_exit * p.no_shares;
            h["exitFee"] = gross * fr;
            h["pnlUsdc"] = p.pnl_usdc.value_or(0.0);
            h["openedAt"] = p.opened_at;
            h["closedAt"] = p.closed_at.value_or(0.0);
            h["endDateTs"] = p.end_date_ts;
            h["exitReason"] = p.exit_reason.c_str();
            h["isPaperMode"] = p.paper_mode;
            h["windowMinutes"] = p.window_minutes;
            push_hist({p.closed_at.value_or(p.opened_at), std::move(h)});
        }

        for (const auto& [id, p] : risk_manager_->get_open_positions()) {
            if (p.strategy == "LA") continue;
            if (!after_baseline(p.opened_at)) continue;
            boost::json::object h;
            h["id"] = id.c_str();
            h["strategy"] = p.strategy.c_str();
            h["asset"] = p.asset.c_str();
            h["status"] = "open";
            h["market"] = p.market_question.c_str();
            h["side"] = p.side.c_str();
            h["direction"] = p.direction.c_str();
            h["entryPrice"] = p.entry_price;
            h["size"] = p.size_shares;
            h["costUsdc"] = p.cost_usdc;
            h["entryFee"] = p.cost_usdc * fr;
            h["exitFee"] = 0.0;
            auto live = get_token_price(p.token_id);
            double unreal = live ? (live->price - p.entry_price) * p.size_shares : 0.0;
            h["pnlUsdc"] = unreal;
            h["openedAt"] = p.opened_at;
            h["closedAt"] = 0.0;
            h["endDateTs"] = p.end_date_ts;
            h["exitReason"] = "";
            h["isPaperMode"] = p.paper_mode;
            if (live) h["exitPrice"] = live->price;
            push_hist({p.opened_at, std::move(h)});
        }

        for (const auto& [id, p] : risk_manager_->get_open_lih_positions()) {
            if (!after_baseline(p.opened_at)) continue;
            if (p.is_shadow) continue;
            const double yes_avg = p.yes_entry_price > 0 ? p.yes_entry_price
                : (p.yes_shares > 0 ? p.yes_cost / p.yes_shares : 0.0);
            const double no_avg = p.no_entry_price > 0 ? p.no_entry_price
                : (p.no_shares > 0 ? p.no_cost / p.no_shares : 0.0);
            const double matched = std::min(p.yes_shares, p.no_shares);
            boost::json::object h;
            h["id"] = id.c_str();
            h["strategy"] = "LIH";
            h["asset"] = p.asset.c_str();
            h["status"] = "open";
            h["market"] = p.market_question.c_str();
            h["side"] = "LIH";
            h["direction"] = "LEG-IN";
            h["yesEntryPrice"] = yes_avg;
            h["noEntryPrice"] = no_avg;
            h["entryPrice"] = yes_avg + no_avg;
            h["size"] = matched > 0 ? matched : std::max(p.yes_shares, p.no_shares);
            h["costUsdc"] = p.yes_cost + p.no_cost;
            h["entryFee"] = p.entry_fees > 0 ? p.entry_fees : (p.yes_cost + p.no_cost) * fr;
            h["exitFee"] = 0.0;
            const double yes_bid = token_mark(p.yes_token_id);
            const double no_bid = token_mark(p.no_token_id);
            double unreal = 0.0;
            if (matched > 0.0) unreal += matched * (1.0 - yes_avg - no_avg);
            const double excess_yes = std::max(0.0, p.yes_shares - matched);
            const double excess_no = std::max(0.0, p.no_shares - matched);
            if (excess_yes > 0.0 && yes_bid > 0.0) unreal += excess_yes * (yes_bid - yes_avg);
            if (excess_no > 0.0 && no_bid > 0.0) unreal += excess_no * (no_bid - no_avg);
            h["pnlUsdc"] = unreal;
            h["openedAt"] = p.opened_at;
            h["closedAt"] = 0.0;
            h["endDateTs"] = p.end_date_ts;
            h["exitReason"] = "";
            h["isPaperMode"] = p.paper_mode;
            h["windowMinutes"] = p.window_minutes;
            push_hist({p.opened_at, std::move(h)});
        }

        // Shadow simulated trades (LIVE_LIH_DRY_RUN) — separate from live tradeHistory
        std::vector<HistRow> shadow_rows;
        auto push_shadow = [&](HistRow row) { shadow_rows.push_back(std::move(row)); };
        double shadow_pnl_sum = 0.0;

        auto lih_shadow_hist = [&](const risk::LegInHedgePosition& p, const char* status) {
            if (!p.is_shadow) return;
            const double yes_avg = p.yes_entry_price > 0 ? p.yes_entry_price
                : (p.yes_shares > 0 ? p.yes_cost / p.yes_shares : 0.0);
            const double no_avg = p.no_entry_price > 0 ? p.no_entry_price
                : (p.no_shares > 0 ? p.no_cost / p.no_shares : 0.0);
            const double matched = std::min(p.yes_shares, p.no_shares);
            const double gap = std::abs(p.yes_shares - p.no_shares);
            const bool fully_hedged = matched >= 1.0 && gap <= 0.5;
            boost::json::object h;
            h["id"] = p.lih_id.c_str();
            h["strategy"] = "LIH-SHADOW";
            h["isShadow"] = true;
            h["asset"] = p.asset.c_str();
            h["status"] = status;
            h["market"] = p.market_question.c_str();
            h["windowMinutes"] = p.window_minutes;
            h["yesEntryPrice"] = yes_avg;
            h["noEntryPrice"] = no_avg;
            h["entryPrice"] = yes_avg + no_avg;
            h["yesSize"] = p.yes_shares;
            h["noSize"] = p.no_shares;
            h["size"] = matched > 0 ? matched : std::max(p.yes_shares, p.no_shares);
            h["costUsdc"] = p.yes_cost + p.no_cost;
            h["entryFee"] = p.entry_fees > 0 ? p.entry_fees : (p.yes_cost + p.no_cost) * fr;
            h["gap"] = gap;
            h["fullyHedged"] = fully_hedged;
            h["rebalanceCount"] = p.rebalance_count;
            h["openedAt"] = p.opened_at;
            h["endDateTs"] = p.end_date_ts;
            if (std::string(status) == "closed") {
                h["yesExitPrice"] = p.yes_exit_price.value_or(0.0);
                h["noExitPrice"] = p.no_exit_price.value_or(0.0);
                h["exitPrice"] = p.yes_exit_price.value_or(0.0) + p.no_exit_price.value_or(0.0);
                h["exitFee"] = (p.yes_shares * p.yes_exit_price.value_or(0.0)
                    + p.no_shares * p.no_exit_price.value_or(0.0)) * fr;
                h["pnlUsdc"] = p.pnl_usdc.value_or(0.0);
                h["closedAt"] = p.closed_at.value_or(0.0);
                h["exitReason"] = p.exit_reason.c_str();
                shadow_pnl_sum += p.pnl_usdc.value_or(0.0);
            } else {
                const double yes_bid = token_mark(p.yes_token_id);
                const double no_bid = token_mark(p.no_token_id);
                double unreal = 0.0;
                if (matched > 0.0) unreal += matched * (1.0 - yes_avg - no_avg);
                const double excess_yes = std::max(0.0, p.yes_shares - matched);
                const double excess_no = std::max(0.0, p.no_shares - matched);
                if (excess_yes > 0.0 && yes_bid > 0.0) unreal += excess_yes * (yes_bid - yes_avg);
                if (excess_no > 0.0 && no_bid > 0.0) unreal += excess_no * (no_bid - no_avg);
                h["pnlUsdc"] = unreal;
                h["closedAt"] = 0.0;
                h["exitReason"] = "";
            }
            const double sort_ts = std::string(status) == "closed"
                ? p.closed_at.value_or(p.opened_at) : p.opened_at;
            push_shadow({sort_ts, std::move(h)});
        };

        for (const auto& p : risk_manager_->get_closed_lih_positions()) {
            lih_shadow_hist(p, "closed");
        }
        for (const auto& [id, p] : risk_manager_->get_open_lih_positions()) {
            (void)id;
            lih_shadow_hist(p, "open");
        }

        std::sort(shadow_rows.begin(), shadow_rows.end(),
                  [](const HistRow& a, const HistRow& b) { return a.sort_ts > b.sort_ts; });
        int64_t shadow_open = 0;
        for (const auto& [id, p] : risk_manager_->get_open_lih_positions()) {
            (void)id;
            if (p.is_shadow) ++shadow_open;
        }
        boost::json::array shadow_arr;
        for (auto& row : shadow_rows) shadow_arr.push_back(std::move(row.obj));
        root["shadowTradeHistory"] = std::move(shadow_arr);
        root["shadowLihPnl"] = shadow_pnl_sum;
        root["shadowOpenCount"] = shadow_open;

        std::sort(hist_rows.begin(), hist_rows.end(),
                  [](const HistRow& a, const HistRow& b) { return a.sort_ts > b.sort_ts; });
        // Dedupe by trade id (keep newest row per id).
        std::unordered_map<std::string, boost::json::object> hist_by_id;
        std::vector<std::string> hist_order;
        for (auto& row : hist_rows) {
            const auto id_it = row.obj.find("id");
            if (id_it == row.obj.end() || !id_it->value().is_string()) {
                hist_order.push_back("__anon_" + std::to_string(hist_order.size()));
                hist_by_id[hist_order.back()] = std::move(row.obj);
                continue;
            }
            const std::string id = std::string(id_it->value().as_string());
            if (!hist_by_id.count(id)) hist_order.push_back(id);
            hist_by_id[id] = std::move(row.obj);
        }
        boost::json::array hist_arr;
        for (const auto& id : hist_order) {
            auto it = hist_by_id.find(id);
            if (it != hist_by_id.end()) hist_arr.push_back(it->second);
        }
        root["tradeHistory"] = std::move(hist_arr);
    } else {
        root["balance"] = 1000.0;
        root["dailyStartingBalance"] = 1000.0;
        root["peakBalance"] = 1000.0;
        root["dailyPnl"] = 0.0;
        root["totalPnl"] = 0.0;
        root["maxDrawdownPct"] = 0.0;
        root["openCount"] = 0;
        root["totalTrades"] = 0;
        root["totalDhTrades"] = 0;
        root["totalLihTrades"] = 0;
        root["winRate"] = 0.0;
        root["laPnl"] = 0.0;
        root["dhPnl"] = 0.0;
        root["lihPnl"] = 0.0;
        root["status"] = 0;
        root["openPositions"] = boost::json::array{};
        root["tradeHistory"] = boost::json::array{};
        root["shadowTradeHistory"] = boost::json::array{};
        root["shadowLihPnl"] = 0.0;
        root["shadowOpenCount"] = 0;
    }

    boost::json::array opps;
    { 
        std::vector<MarketInfo> markets_snapshot;
        { std::shared_lock lock(market_mutex_); markets_snapshot = markets_; }
        
        std::unordered_map<std::string, TokenPrice> tokens_snapshot;
        { std::shared_lock lock(token_mutex_); tokens_snapshot = token_prices_; }
        
        for (const auto& m : markets_snapshot) {
            boost::json::object mo;
            mo["asset"] = m.asset.c_str();
            mo["windowMinutes"] = m.window_minutes;
            mo["question"] = m.question.c_str();
            auto it_y = tokens_snapshot.find(m.yes_token_id);
            auto it_n = tokens_snapshot.find(m.no_token_id);
            double yes = 0.0;
            double no = 0.0;
            double combined = 1.0;
            double discountPct = 0.0;

            if (it_y != tokens_snapshot.end() && it_n != tokens_snapshot.end()) {
                yes = it_y->second.price;
                no = it_n->second.price;
                combined = yes + no;
                if (combined < 1.0 && combined > 0.0) {
                    discountPct = (1.0 - combined) * 100.0;
                }
            }

            mo["yesPrice"] = yes; 
            mo["noPrice"] = no;
            mo["combined"] = combined;
            mo["discountPct"] = discountPct;
            mo["endDateTs"] = m.end_date_ts;
            mo["endDate"] = m.end_date_iso.c_str();
            opps.push_back(mo);
        }
    }
    root["marketsScanned"] = static_cast<int>(opps.size());
    root["dhOpportunities"] = opps;
    root["marketOpportunities"] = opps;

    // Telemetry & signal logs
    {
        std::shared_lock lock(log_mutex_);
        boost::json::array tlog;
        for (const auto& l : telemetry_log_) tlog.push_back(l.c_str());
        root["telemetryLog"] = std::move(tlog);

        boost::json::array slog;
        for (const auto& l : signal_log_) slog.push_back(l.c_str());
        root["signalLog"] = std::move(slog);
    }

    // Per-asset tick rates (ticks/sec over last second approximation)
    {
        std::shared_lock lb(btc_mutex_);
        root["btcTickRate"] = static_cast<double>(btc_tick_count_);
    }
    {
        std::shared_lock le(eth_mutex_);
        root["ethTickRate"] = static_cast<double>(eth_tick_count_);
    }
    {
        std::shared_lock ls(sol_mutex_);
        root["solTickRate"] = static_cast<double>(sol_tick_count_);
    }

    root["timestamp"] = static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());

    return boost::json::serialize(root);
}


void StateStore::update_token_bid(std::string_view token_id, const TokenPrice& price) {
    std::unique_lock lock(token_mutex_);
    token_bids_[std::string(token_id)] = price;
}

std::optional<TokenPrice> StateStore::get_token_bid(std::string_view token_id) const {
    std::shared_lock lock(token_mutex_);
    auto it = token_bids_.find(std::string(token_id));
    if (it != token_bids_.end()) return it->second;
    return std::nullopt;
}

void StateStore::reload_live_mirror(double max_age_sec) {
    if (mirror_path_.empty()) return;
    std::ifstream in(mirror_path_);
    if (!in.is_open()) return;
    std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (body.empty()) return;
    try {
        auto jv = boost::json::parse(body);
        if (!jv.is_object()) return;
        const auto& root = jv.as_object();
        const double updated = root.contains("updated_at") ? root.at("updated_at").as_double() : 0.0;
        const double now = std::chrono::duration<double>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        if (updated > 0 && (now - updated) > max_age_sec) return;

        std::unordered_map<std::string, MirrorAssetQuote> parsed;
        if (root.contains("assets") && root.at("assets").is_object()) {
            for (const auto& kv : root.at("assets").as_object()) {
                if (!kv.value().is_object()) continue;
                const auto& o = kv.value().as_object();
                MirrorAssetQuote q;
                if (o.contains("book_yes")) q.book_yes = o.at("book_yes").as_double();
                if (o.contains("book_no")) q.book_no = o.at("book_no").as_double();
                if (o.contains("ws_yes")) q.ws_yes = o.at("ws_yes").as_double();
                if (o.contains("ws_no")) q.ws_no = o.at("ws_no").as_double();
                q.updated_at = updated;
                q.fresh = (q.book_yes > 0 || q.ws_yes > 0) && (q.book_no > 0 || q.ws_no > 0);
                parsed[std::string(kv.key())] = q;
            }
        }
        std::unique_lock lock(mirror_mutex_);
        mirror_by_asset_ = std::move(parsed);
        mirror_loaded_at_ = now;
    } catch (...) {
        return;
    }
}

std::optional<StateStore::MirrorAssetQuote> StateStore::get_mirror_quote(const std::string& asset) const {
    std::shared_lock lock(mirror_mutex_);
    std::string key = asset;
    std::transform(key.begin(), key.end(), key.begin(), ::tolower);
    auto it = mirror_by_asset_.find(key);
    if (it == mirror_by_asset_.end()) return std::nullopt;
    return it->second;
}

} // namespace trading
