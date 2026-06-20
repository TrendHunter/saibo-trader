#include "RiskManager.h"
#include <spdlog/spdlog.h>
#include <numeric>
#include <cmath>
#include <algorithm>
#include <ctime>
#include <unordered_map>
#include <boost/json.hpp>

namespace risk {

namespace {
constexpr double kFloatTol = 1e-6;
} // namespace

double RiskManager::now() {
    auto duration = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration<double>(duration).count();
}

double RiskManager::next_midnight() {
    auto now_t = std::chrono::system_clock::now();
    time_t t_now = std::chrono::system_clock::to_time_t(now_t);
    tm* utc_tm = gmtime(&t_now);
    
    utc_tm->tm_hour = 0;
    utc_tm->tm_min = 0;
    utc_tm->tm_sec = 0;
    
#ifdef _WIN32
    time_t midnight = _mkgmtime(utc_tm);
#else
    time_t midnight = timegm(utc_tm);
#endif
    
    midnight += 86400; // Next day
    return static_cast<double>(midnight);
}

RiskManager::RiskManager(
    double starting_balance,
    double max_position_fraction,
    double daily_loss_limit,
    double total_drawdown_kill,
    int max_concurrent_positions,
    bool circuit_breaker_enabled,
    int circuit_breaker_min_losses,
    int circuit_breaker_window,
    double circuit_breaker_loss_pct,
    double circuit_breaker_pause_seconds,
    double min_order_size
) : max_position_fraction_(max_position_fraction),
    daily_loss_limit_(daily_loss_limit),
    total_drawdown_kill_(total_drawdown_kill),
    max_concurrent_positions_(max_concurrent_positions),
    circuit_breaker_enabled_(circuit_breaker_enabled),
    circuit_breaker_min_losses_(circuit_breaker_min_losses),
    circuit_breaker_loss_pct_(circuit_breaker_loss_pct),
    circuit_breaker_pause_seconds_(circuit_breaker_pause_seconds),
    min_order_size_(min_order_size),
    starting_balance_(starting_balance),
    current_balance_(starting_balance),
    peak_balance_(starting_balance),
    daily_starting_balance_(starting_balance),
    daily_reset_time_(next_midnight()),
    status_(TradingStatus::PAUSED),
    kill_reason_(std::nullopt),
    circuit_breaker_window_(circuit_breaker_window)
{
    spdlog::info(
        "RiskManager initialized | Balance: ${:.2f} | "
        "Max position: {:.0f}% | Daily limit: -{:.0f}% | Kill switch: -{:.0f}%",
        starting_balance_,
        max_position_fraction_ * 100.0,
        daily_loss_limit_ * 100.0,
        total_drawdown_kill_ * 100.0
    );
}

TradingStatus RiskManager::get_status() const {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    return status_;
}

bool RiskManager::is_trading_allowed() {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    check_daily_reset();
    check_circuit_breaker_resume();
    return status_ == TradingStatus::ACTIVE;
}

double RiskManager::get_current_balance() const {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    return current_balance_;
}

double RiskManager::get_total_equity() const {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    return compute_equity_unlocked();
}

std::optional<std::string> RiskManager::get_status_reason() const {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    return kill_reason_;
}

double RiskManager::get_daily_starting_balance() const {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    return daily_starting_balance_;
}

double RiskManager::get_peak_balance() const {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    return peak_balance_;
}

double RiskManager::get_starting_balance() const {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    return starting_balance_;
}

int RiskManager::get_open_position_count() const {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    int n = 0;
    for (const auto& [id, p] : open_positions_) {
        (void)id;
        if (!p.paper_mode) ++n;
    }
    for (const auto& [id, p] : open_dh_positions_) {
        (void)id;
        if (!p.paper_mode) ++n;
    }
    for (const auto& [id, p] : open_lih_positions_) {
        (void)id;
        if (!p.paper_mode && !p.is_shadow) ++n;
    }
    return n;
}

double RiskManager::get_win_rate() const {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    // Use persisted trade counters — closed_* arrays may be truncated on paper-state export.
    int closed = total_trades_ + total_dh_trades_ + total_lih_trades_;
    if (closed == 0) return 0.0;
    double rate = static_cast<double>(winning_trades_) / closed;
    return std::min(rate, 1.0);
}

double RiskManager::get_min_order_size() const {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    return min_order_size_;
}

double RiskManager::get_la_pnl() const {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    return la_pnl_;
}

double RiskManager::get_dh_pnl() const {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    return dh_pnl_;
}

int RiskManager::get_total_trades() const {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    return total_trades_;
}

int RiskManager::get_total_dh_trades() const {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    return total_dh_trades_;
}

int RiskManager::get_total_lih_trades() const {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    return total_lih_trades_;
}

int RiskManager::get_winning_trades() const {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    return winning_trades_;
}

const std::unordered_map<std::string, int>& RiskManager::get_asset_trades() const {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    return asset_trades_;
}

const std::unordered_map<std::string, int>& RiskManager::get_asset_wins() const {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    return asset_wins_;
}

const std::unordered_map<std::string, double>& RiskManager::get_asset_pnl() const {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    return asset_pnl_;
}

std::unordered_map<std::string, Position> RiskManager::get_open_positions() const {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    return open_positions_;
}

std::unordered_map<std::string, DumpHedgePosition> RiskManager::get_open_dh_positions() const {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    return open_dh_positions_;
}

std::vector<Position> RiskManager::get_closed_positions() const {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    return closed_positions_;
}

std::vector<DumpHedgePosition> RiskManager::get_closed_dh_positions() const {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    return closed_dh_positions_;
}

std::vector<LegInHedgePosition> RiskManager::get_closed_lih_positions() const {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    return closed_lih_positions_;
}

double RiskManager::get_lih_pnl() const {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    return lih_pnl_;
}

void RiskManager::update_peak_price(const std::string& order_id, double peak_price) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    auto it = open_positions_.find(order_id);
    if (it != open_positions_.end()) {
        it->second.peak_price = peak_price;
    }
}

bool RiskManager::is_trading_allowed_no_lock() {
    check_daily_reset();
    check_circuit_breaker_resume();
    return status_ == TradingStatus::ACTIVE;
}

std::pair<bool, std::string> RiskManager::can_open_position(double position_size_usdc) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    if (!is_trading_allowed()) {
        return {false, "Trading halted: " + kill_reason_.value_or("N/A")};
    }

    if (get_open_position_count() >= max_concurrent_positions_) {
        return {false, "Max concurrent positions reached (" + std::to_string(max_concurrent_positions_) + ")"};
    }

    double max_allowed = current_balance_ * max_position_fraction_;
    if (position_size_usdc > max_allowed) {
        return {false, "Position size exceeds max allowed (" + std::to_string(max_position_fraction_ * 100.0) + "% of balance)"};
    }

    if (position_size_usdc > current_balance_) {
        return {false, "Insufficient balance"};
    }

    if (position_size_usdc < min_order_size_) {
        return {false, "Position size $" + std::to_string(position_size_usdc) + " below minimum $" + std::to_string(min_order_size_)};
    }

    return {true, "OK"};
}

void RiskManager::register_trade_open(const Position& position) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    open_positions_[position.order_id] = position;
    double entry_fee = position.cost_usdc * fee_rate_;
    current_balance_ -= (position.cost_usdc + entry_fee);

    spdlog::info("Position OPENED | {} | ${:.2f} USDC (+ ${:.2f} fee) | Balance: ${:.2f}",
                 position.order_id, position.cost_usdc, entry_fee, current_balance_);
}

std::optional<Position> RiskManager::register_trade_close(
    const std::string& order_id,
    double exit_price,
    std::optional<double> exit_timestamp,
    std::optional<double> actual_proceeds_usdc
) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    auto it = open_positions_.find(order_id);
    if (it == open_positions_.end()) {
        spdlog::warn("register_trade_close: order_id {} not found", order_id);
        return std::nullopt;
    }

    Position pos = it->second;
    open_positions_.erase(it);

    if (exit_price < 0.0 || exit_price > 1.0) {
        spdlog::error("Invalid exit_price {:.3f} for {} | Expected: 0.000-1.000", exit_price, order_id);
        open_positions_[order_id] = pos;
        return std::nullopt;
    }

    pos.closed_at = exit_timestamp.value_or(now());
    pos.exit_price = exit_price;

    double pnl = 0.0;
    if (actual_proceeds_usdc.has_value()) {
        pnl = actual_proceeds_usdc.value() - pos.cost_usdc - (pos.cost_usdc * fee_rate_);
        current_balance_ += actual_proceeds_usdc.value();
    } else {
        double gross = exit_price * pos.size_shares;
        double exit_fee = gross * fee_rate_;
        double net = gross - exit_fee;
        double entry_fee = pos.cost_usdc * fee_rate_;
        pnl = net - pos.cost_usdc - entry_fee;
        current_balance_ += net;
    }

    pos.pnl_usdc = pnl;
    total_pnl_ += pnl;
    la_pnl_ += pnl;
    total_trades_++;
    
    bool won = pnl > 0.0;
    if (won) winning_trades_++;

    record_asset_close(pos.asset, pnl, won);

    if (current_balance_ > peak_balance_) {
        peak_balance_ = current_balance_;
    }

    closed_positions_.push_back(pos);
    if (closed_positions_.size() > 1000) {
        closed_positions_.erase(closed_positions_.begin());
    }

    spdlog::info("Position CLOSED | {} | PnL: ${:+.2f} | Balance: ${:.2f} | Win rate: {:.1f}%",
                 order_id, pnl, current_balance_, get_win_rate() * 100.0);

    check_risk_thresholds();
    
    recent_la_pnls_.push_back(pnl);
    if(recent_la_pnls_.size() > static_cast<size_t>(circuit_breaker_window_)) {
        recent_la_pnls_.pop_front();
    }
    
    check_circuit_breaker();

    return pos;
}

std::pair<bool, std::string> RiskManager::can_open_dh_position(double combined_cost_usdc) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    if (!is_trading_allowed()) {
        return {false, "Trading halted: " + kill_reason_.value_or("N/A")};
    }

    if (get_open_position_count() >= max_concurrent_positions_) {
        return {false, "Max concurrent positions reached (" + std::to_string(max_concurrent_positions_) + ")"};
    }

    double max_allowed = current_balance_ * max_position_fraction_;
    if (combined_cost_usdc > max_allowed) {
        return {false, "DH cost exceeds max allowed"};
    }

    if (combined_cost_usdc > current_balance_) {
        return {false, "Insufficient balance"};
    }

    return {true, "OK"};
}

void RiskManager::register_dh_open(const DumpHedgePosition& position) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    open_dh_positions_[position.dh_id] = position;
    double entry_fee = position.combined_cost_usdc * fee_rate_;
    current_balance_ -= (position.combined_cost_usdc + entry_fee);

    spdlog::info("DH Position OPENED | {} | ${:.2f} USDC (+ ${:.2f} fee) | Locked: ${:.2f} | Balance: ${:.2f}",
                 position.dh_id, position.combined_cost_usdc, entry_fee, position.locked_profit_usdc, current_balance_);
}

std::optional<DumpHedgePosition> RiskManager::register_dh_close(
    const std::string& dh_id,
    double yes_exit_price,
    double no_exit_price,
    const std::string& exit_reason,
    std::optional<double> exit_timestamp,
    std::optional<double> actual_proceeds_usdc
) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    auto it = open_dh_positions_.find(dh_id);
    if (it == open_dh_positions_.end()) {
        spdlog::warn("register_dh_close: dh_id {} not found", dh_id);
        return std::nullopt;
    }

    DumpHedgePosition pos = it->second;
    open_dh_positions_.erase(it);

    pos.closed_at = exit_timestamp.value_or(now());
    pos.yes_exit_price = yes_exit_price;
    pos.no_exit_price = no_exit_price;
    pos.exit_reason = exit_reason;

    double pnl = 0.0;
    if (actual_proceeds_usdc.has_value()) {
        pnl = actual_proceeds_usdc.value() - pos.combined_cost_usdc - (pos.combined_cost_usdc * fee_rate_);
        current_balance_ += actual_proceeds_usdc.value();
    } else {
        double gross = (yes_exit_price + no_exit_price) * pos.size_shares;
        double exit_fee = gross * fee_rate_;
        double net = gross - exit_fee;
        double entry_fee = pos.combined_cost_usdc * fee_rate_;
        pnl = net - pos.combined_cost_usdc - entry_fee;
        current_balance_ += net;
    }

    pos.pnl_usdc = pnl;
    total_pnl_ += pnl;
    dh_pnl_ += pnl;
    total_dh_trades_++;

    bool won = pnl > 0.0;
    if (won) winning_trades_++;

    record_asset_close(pos.asset, pnl, won);

    if (current_balance_ > peak_balance_) {
        peak_balance_ = current_balance_;
    }

    closed_dh_positions_.push_back(pos);
    if (closed_dh_positions_.size() > 1000) {
        closed_dh_positions_.erase(closed_dh_positions_.begin());
    }

    spdlog::info("DH Position CLOSED | {} | PnL: ${:+.2f} | Reason: {} | Balance: ${:.2f}",
                 dh_id, pnl, exit_reason, current_balance_);

    check_risk_thresholds();
    
    recent_dh_pnls_.push_back(pnl);
    if(recent_dh_pnls_.size() > static_cast<size_t>(circuit_breaker_window_)) {
        recent_dh_pnls_.pop_front();
    }
    
    check_circuit_breaker();

    return pos;
}

std::pair<bool, std::string> RiskManager::can_open_lih_leg(
    double leg_cost_usdc,
    bool add_to_existing_lih,
    const std::string* lih_id,
    double add_matched_shares,
    const std::string* slot_asset,
    int slot_window_minutes) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    if (!is_trading_allowed()) {
        const bool hedge_existing = add_to_existing_lih && lih_id && open_lih_positions_.count(*lih_id);
        if (!hedge_existing) {
            return {false, "Trading halted: " + kill_reason_.value_or("N/A")};
        }
    }
    if (!add_to_existing_lih && get_open_position_count() >= max_concurrent_positions_) {
        return {false, "Max concurrent positions reached"};
    }
    // DEBUG single-round test: re-enable to cap leg1 count per Web resume (LIH_SESSION_MAX_LEGS).
    // if (!add_to_existing_lih && lih_session_max_legs_ > 0 &&
    //     lih_session_legs_used_ >= lih_session_max_legs_) {
    //     return {false, "LIH session leg cap reached (" + std::to_string(lih_session_max_legs_) + ")"};
    // }
    if (!add_to_existing_lih && lih_min_balance_usdc_ > 0.0 &&
        current_balance_ + 1e-6 < lih_min_balance_usdc_) {
        return {false, "Balance below LIH minimum ($" +
                       std::to_string(lih_min_balance_usdc_) + ", have $" +
                       std::to_string(current_balance_) + ")"};
    }

    const double max_allowed = current_balance_ * max_position_fraction_;
    const double leg_cap = std::min(max_allowed, current_balance_);
    if (leg_cost_usdc > leg_cap + 1e-6) {
        return {false, "LIH leg cost exceeds max allowed (" +
                       std::to_string(max_position_fraction_ * 100.0) + "% of balance)"};
    }
    if (leg_cost_usdc > current_balance_) {
        return {false, "Insufficient balance"};
    }
    // LIH per-leg exchange minimum is ~$1 (detector enforces); MIN_ORDER_SIZE is for DH whole tickets.
    constexpr double kLihMinLegUsdc = 1.0;
    if (leg_cost_usdc + 1e-6 < kLihMinLegUsdc) {
        return {false, "LIH leg below $1 exchange minimum"};
    }

    std::string asset;
    int window = 0;
    if (slot_asset && !slot_asset->empty() && slot_window_minutes > 0) {
        asset = *slot_asset;
        window = slot_window_minutes;
    } else if (lih_id) {
        auto it = open_lih_positions_.find(*lih_id);
        if (it != open_lih_positions_.end()) {
            asset = it->second.asset;
            window = it->second.window_minutes;
        }
    }
    if (!asset.empty() && window > 0) {
        if (!add_to_existing_lih && lih_other_slot_busy_unlocked(asset, window)) {
            return {false, "Another LIH slot is active (one-slot mode)"};
        }
        const double slot_cap = lih_slot_cap_usdc_unlocked();
        const double deployed = lih_slot_deployed_usdc_unlocked(asset, window);
        if (deployed + leg_cost_usdc > slot_cap + 1e-6) {
            return {false, "LIH slot budget exceeded ($" +
                           std::to_string(deployed + leg_cost_usdc) + " > $" +
                           std::to_string(slot_cap) + " for " + asset + "|" +
                           std::to_string(window) + "m)"};
        }
    }

    if (lih_id && lih_max_matched_shares_ > 0.0 && add_matched_shares > 0.0) {
        auto it = open_lih_positions_.find(*lih_id);
        if (it != open_lih_positions_.end()) {
            const double matched = std::min(it->second.yes_shares, it->second.no_shares);
            if (matched + add_matched_shares > lih_max_matched_shares_ + 1e-6) {
                return {false, "LIH matched shares would exceed cap"};
            }
        }
    }
    return {true, "OK"};
}

double RiskManager::lih_slot_cap_usdc_unlocked() const {
    if (lih_max_usdc_per_slot_ > 0.0) return lih_max_usdc_per_slot_;
    return std::min(current_balance_ * max_position_fraction_, current_balance_);
}

double RiskManager::lih_slot_deployed_usdc_unlocked(const std::string& asset, int window_minutes) const {
    double total = 0.0;
    for (const auto& [id, p] : open_lih_positions_) {
        if (p.asset == asset && p.window_minutes == window_minutes) {
            total += p.yes_cost + p.no_cost + p.entry_fees;
        }
    }
    return total;
}

double RiskManager::get_lih_slot_cap_usdc() const {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    return lih_slot_cap_usdc_unlocked();
}

void RiskManager::set_lih_max_usdc_per_slot(double v) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    lih_max_usdc_per_slot_ = std::max(0.0, v);
    spdlog::info("Risk config updated | lih_max_usdc_per_slot={:.2f}", lih_max_usdc_per_slot_);
}

double RiskManager::get_lih_max_usdc_per_slot() const {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    return lih_max_usdc_per_slot_;
}

double RiskManager::lih_slot_deployed_usdc(const std::string& asset, int window_minutes) const {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    return lih_slot_deployed_usdc_unlocked(asset, window_minutes);
}

double RiskManager::get_max_leg_cost_usdc() const {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    const double cap = current_balance_ * max_position_fraction_;
    return std::min(cap, current_balance_);
}

double RiskManager::get_lih_max_matched_shares() const {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    return lih_max_matched_shares_;
}

double RiskManager::lih_remaining_matched_shares(const std::string& lih_id) const {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    if (lih_max_matched_shares_ <= 0.0) return 1e18;
    auto it = open_lih_positions_.find(lih_id);
    if (it == open_lih_positions_.end()) return lih_max_matched_shares_;
    const double matched = std::min(it->second.yes_shares, it->second.no_shares);
    return std::max(0.0, lih_max_matched_shares_ - matched);
}

void RiskManager::set_lih_max_matched_shares(double v) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    lih_max_matched_shares_ = std::max(0.0, v);
    spdlog::info("Risk config updated | lih_max_matched_shares={:.1f}", lih_max_matched_shares_);
}

std::optional<LegInHedgePosition> RiskManager::find_open_lih_by_asset(
    const std::string& asset, int window_minutes) const {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    std::string want = asset;
    std::transform(want.begin(), want.end(), want.begin(), ::tolower);
    for (const auto& [id, p] : open_lih_positions_) {
        std::string have = p.asset;
        std::transform(have.begin(), have.end(), have.begin(), ::tolower);
        if (have == want && p.window_minutes == window_minutes) return p;
    }
    return std::nullopt;
}

std::optional<LegInHedgePosition> RiskManager::find_open_lih_for_market(
    const trading::MarketInfo& market) const {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    std::string want = market.asset;
    std::transform(want.begin(), want.end(), want.begin(), ::tolower);
    for (const auto& [id, p] : open_lih_positions_) {
        std::string have = p.asset;
        std::transform(have.begin(), have.end(), have.begin(), ::tolower);
        if (have != want || p.window_minutes != market.window_minutes) continue;

        const bool end_match = p.end_date_ts > 0 && market.end_date_ts > 0 &&
                                 std::abs(p.end_date_ts - market.end_date_ts) < 2.0;
        const bool yes_match = !p.yes_token_id.empty() &&
                               (p.yes_token_id == market.yes_token_id ||
                                p.yes_token_id == market.no_token_id);
        const bool no_match = !p.no_token_id.empty() &&
                              (p.no_token_id == market.yes_token_id ||
                               p.no_token_id == market.no_token_id);

        if (end_match || yes_match || no_match) return p;
    }
    return std::nullopt;
}

namespace {
std::string lih_slot_key(const std::string& asset, int window_minutes) {
    std::string a = asset;
    std::transform(a.begin(), a.end(), a.begin(), ::tolower);
    return a + "|" + std::to_string(window_minutes);
}

void drop_lih_leg1_slot(
    std::unordered_set<std::string>& inflight,
    std::unordered_map<std::string, double>& since,
    const std::string& key) {
    inflight.erase(key);
    since.erase(key);
}

constexpr double kLeg1InflightMaxSec = 120.0;
} // namespace

bool RiskManager::lih_has_open_or_inflight(const std::string& asset, int window_minutes) const {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    const std::string key = lih_slot_key(asset, window_minutes);
    if (lih_leg1_inflight_.count(key)) return true;
    std::string want = asset;
    std::transform(want.begin(), want.end(), want.begin(), ::tolower);
    for (const auto& [id, p] : open_lih_positions_) {
        (void)id;
        if (p.asset == want && p.window_minutes == window_minutes && !p.paper_mode && !p.is_shadow) {
            return true;
        }
    }
    return false;
}

bool RiskManager::lih_leg1_inflight_only(const std::string& asset, int window_minutes) const {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    const std::string key = lih_slot_key(asset, window_minutes);
    if (!lih_leg1_inflight_.count(key)) return false;
    // Stale inflight after register/hedge — open position means leg1 already landed.
    std::string want = asset;
    std::transform(want.begin(), want.end(), want.begin(), ::tolower);
    for (const auto& [id, p] : open_lih_positions_) {
        (void)id;
        if (p.paper_mode || p.is_shadow) continue;
        std::string have = p.asset;
        std::transform(have.begin(), have.end(), have.begin(), ::tolower);
        if (have == want && p.window_minutes == window_minutes) return false;
    }
    return true;
}

bool RiskManager::lih_other_slot_busy_unlocked(const std::string& asset, int window_minutes) const {
    if (!lih_one_slot_global_) return false;
    const std::string key = lih_slot_key(asset, window_minutes);
    for (const auto& [id, p] : open_lih_positions_) {
        (void)id;
        if (p.paper_mode || p.is_shadow) continue;
        if (lih_slot_key(p.asset, p.window_minutes) != key) return true;
    }
    for (const auto& inflight_key : lih_leg1_inflight_) {
        if (inflight_key != key) return true;
    }
    return false;
}

bool RiskManager::lih_other_slot_busy(const std::string& asset, int window_minutes) const {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    return lih_other_slot_busy_unlocked(asset, window_minutes);
}

bool RiskManager::lih_session_leg1_blocked() const {
    // DEBUG single-round test: re-enable session leg cap gate (LIH_SESSION_MAX_LEGS).
    // std::lock_guard<std::recursive_mutex> lock(mtx_);
    // return lih_session_max_legs_ > 0 && lih_session_legs_used_ >= lih_session_max_legs_;
    return false;
}

void RiskManager::set_lih_one_slot_global(bool v) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    lih_one_slot_global_ = v;
    spdlog::info("Risk config updated | lih_one_slot_global={}", v);
}

bool RiskManager::get_lih_one_slot_global() const {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    return lih_one_slot_global_;
}

void RiskManager::set_lih_session_max_legs(int v) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    lih_session_max_legs_ = std::max(0, v);
    spdlog::info("Risk config updated | lih_session_max_legs={}", lih_session_max_legs_);
}

int RiskManager::get_lih_session_max_legs() const {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    return lih_session_max_legs_;
}

int RiskManager::get_lih_session_legs_used() const {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    return lih_session_legs_used_;
}

void RiskManager::reset_lih_session() {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    lih_session_legs_used_ = 0;
    spdlog::info("LIH session reset | legs_used=0");
}

void RiskManager::scrub_lih_inflight_locks(double now_sec) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    std::unordered_set<std::string> live_slots;
    for (const auto& [id, p] : open_lih_positions_) {
        (void)id;
        if (p.paper_mode || p.is_shadow) continue;
        live_slots.insert(lih_slot_key(p.asset, p.window_minutes));
    }

    for (auto it = lih_rebalance_inflight_.begin(); it != lih_rebalance_inflight_.end(); ) {
        if (!open_lih_positions_.count(*it)) {
            spdlog::warn("[LIH] scrub stale rebalance lock | {}", *it);
            it = lih_rebalance_inflight_.erase(it);
        } else {
            ++it;
        }
    }

    for (auto it = lih_leg1_inflight_.begin(); it != lih_leg1_inflight_.end(); ) {
        const std::string& key = *it;
        bool drop = live_slots.count(key) > 0;
        if (!drop) {
            const auto sit = lih_leg1_inflight_since_.find(key);
            const double since = sit != lih_leg1_inflight_since_.end() ? sit->second : 0.0;
            if (since <= 0.0 || now_sec - since >= kLeg1InflightMaxSec) {
                drop = true;
            }
        }
        if (drop) {
            if (live_slots.count(key)) {
                spdlog::debug("[LIH] scrub leg1 inflight tail (position open) | {}", key);
            } else {
                spdlog::warn("[LIH] scrub orphan leg1 inflight tail | {}", key);
            }
            lih_leg1_inflight_since_.erase(key);
            it = lih_leg1_inflight_.erase(it);
        } else {
            ++it;
        }
    }
}

void RiskManager::clear_open_lih_positions() {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    const size_t n = open_lih_positions_.size();
    open_lih_positions_.clear();
    lih_leg1_inflight_.clear();
    lih_leg1_inflight_since_.clear();
    lih_rebalance_inflight_.clear();
    if (n > 0) {
        spdlog::info("Cleared {} open LIH position(s) from memory", n);
    }
}

void RiskManager::clear_closed_lih_positions() {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    const size_t n = closed_lih_positions_.size();
    closed_lih_positions_.clear();
    if (n > 0) {
        spdlog::info("Cleared {} closed LIH record(s) from memory", n);
    }
}

void RiskManager::purge_paper_positions() {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    size_t dropped = 0;
    for (auto it = open_positions_.begin(); it != open_positions_.end(); ) {
        if (it->second.paper_mode) {
            it = open_positions_.erase(it);
            ++dropped;
        } else {
            ++it;
        }
    }
    for (auto it = open_dh_positions_.begin(); it != open_dh_positions_.end(); ) {
        if (it->second.paper_mode) {
            it = open_dh_positions_.erase(it);
            ++dropped;
        } else {
            ++it;
        }
    }
    for (auto it = open_lih_positions_.begin(); it != open_lih_positions_.end(); ) {
        if (it->second.paper_mode || it->second.is_shadow) {
            lih_rebalance_inflight_.erase(it->first);
            it = open_lih_positions_.erase(it);
            ++dropped;
        } else {
            ++it;
        }
    }
    closed_lih_positions_.erase(
        std::remove_if(closed_lih_positions_.begin(), closed_lih_positions_.end(),
                       [](const LegInHedgePosition& p) { return p.paper_mode || p.is_shadow; }),
        closed_lih_positions_.end());
    lih_leg1_inflight_.clear();
    if (dropped > 0) {
        spdlog::info("Purged {} paper/shadow open position(s) — live-only", dropped);
    }
}

namespace {
constexpr double kLihConsolidateTol = 1e-4;
constexpr double kLihPairSec = 180.0;

char lih_dominant_side(const LegInHedgePosition& p) {
    const double matched = std::min(p.yes_shares, p.no_shares);
    if (matched > kLihConsolidateTol) return 'B';
    if (p.yes_shares > p.no_shares + kLihConsolidateTol) return 'Y';
    if (p.no_shares > p.yes_shares + kLihConsolidateTol) return 'N';
    return ' ';
}

std::string lih_asset_key(const std::string& asset) {
    std::string a = asset;
    std::transform(a.begin(), a.end(), a.begin(), ::tolower);
    return a;
}

bool lih_should_pair_closed(const LegInHedgePosition& a, const LegInHedgePosition& b) {
    const char sa = lih_dominant_side(a);
    const char sb = lih_dominant_side(b);
    if (sa == ' ' || sb == ' ') return false;
    if (lih_asset_key(a.asset) != lih_asset_key(b.asset)) return false;
    if (a.window_minutes != b.window_minutes) return false;
    // Duplicate close rows (same round, different lih_id e.g. -recon).
    if (sa == 'B' && sb == 'B' && a.end_date_ts > 0 && b.end_date_ts > 0 &&
        std::abs(a.end_date_ts - b.end_date_ts) < 2.0) {
        return true;
    }
    if (sa != sb && sa != 'B' && sb != 'B') return true;
    // Orphan YES/NO leg + row that already has both sides (split -recon history).
    if ((sa == 'Y' || sa == 'N') && sb == 'B') return true;
    if ((sb == 'Y' || sb == 'N') && sa == 'B') return true;
    return false;
}

std::string lih_pick_merged_id(const LegInHedgePosition& a, const LegInHedgePosition& b) {
    for (const auto& p : {a, b}) {
        if (p.lih_id.find("-recon") == std::string::npos) return p.lih_id;
    }
    const auto& first = a.opened_at <= b.opened_at ? a : b;
    return fmt::format("LIH-{}-{}", lih_asset_key(first.asset),
                       static_cast<uint64_t>(first.opened_at * 1000.0));
}

LegInHedgePosition merge_closed_lih_pair(const LegInHedgePosition& a, const LegInHedgePosition& b) {
    const auto& leg1 = a.opened_at <= b.opened_at ? a : b;
    const auto& leg2 = a.opened_at <= b.opened_at ? b : a;
    LegInHedgePosition out;
    out.lih_id = lih_pick_merged_id(a, b);
    out.asset = leg1.asset;
    out.window_minutes = leg1.window_minutes;
    out.market_question = leg1.market_question;
    out.yes_token_id = !leg1.yes_token_id.empty() ? leg1.yes_token_id : leg2.yes_token_id;
    out.no_token_id = !leg1.no_token_id.empty() ? leg1.no_token_id : leg2.no_token_id;
    if (out.yes_token_id.empty()) out.yes_token_id = leg2.yes_token_id;
    if (out.no_token_id.empty()) out.no_token_id = leg2.no_token_id;
    out.condition_id = !leg1.condition_id.empty() ? leg1.condition_id : leg2.condition_id;
    out.yes_shares = a.yes_shares + b.yes_shares;
    out.no_shares = a.no_shares + b.no_shares;
    out.yes_cost = a.yes_cost + b.yes_cost;
    out.no_cost = a.no_cost + b.no_cost;
    // Duplicate full hedge rows: same round closed twice — keep larger leg, not sum.
    if (lih_dominant_side(a) == 'B' && lih_dominant_side(b) == 'B') {
        out.yes_shares = std::max(a.yes_shares, b.yes_shares);
        out.no_shares = std::max(a.no_shares, b.no_shares);
        out.yes_cost = a.yes_shares >= b.yes_shares ? a.yes_cost : b.yes_cost;
        out.no_cost = a.no_shares >= b.no_shares ? a.no_cost : b.no_cost;
        if (a.yes_shares > b.yes_shares + kLihConsolidateTol) {
            out.yes_entry_price = a.yes_entry_price > 0 ? a.yes_entry_price
                : (a.yes_shares > kLihConsolidateTol ? a.yes_cost / a.yes_shares : 0.0);
        } else if (b.yes_shares > a.yes_shares + kLihConsolidateTol) {
            out.yes_entry_price = b.yes_entry_price > 0 ? b.yes_entry_price
                : (b.yes_shares > kLihConsolidateTol ? b.yes_cost / b.yes_shares : 0.0);
        }
        if (a.no_shares > b.no_shares + kLihConsolidateTol) {
            out.no_entry_price = a.no_entry_price > 0 ? a.no_entry_price
                : (a.no_shares > kLihConsolidateTol ? a.no_cost / a.no_shares : 0.0);
        } else if (b.no_shares > a.no_shares + kLihConsolidateTol) {
            out.no_entry_price = b.no_entry_price > 0 ? b.no_entry_price
                : (b.no_shares > kLihConsolidateTol ? b.no_cost / b.no_shares : 0.0);
        }
        out.pnl_usdc = a.pnl_usdc.value_or(0.0);
        if (b.pnl_usdc.has_value()) out.pnl_usdc = b.pnl_usdc.value();
        out.entry_fees = std::max(a.entry_fees, b.entry_fees);
        out.rebalance_count = std::max(a.rebalance_count, b.rebalance_count);
    }
    if (a.yes_shares > kLihConsolidateTol && a.yes_entry_price > 0) {
        out.yes_entry_price = a.yes_entry_price;
    } else if (b.yes_shares > kLihConsolidateTol && b.yes_entry_price > 0) {
        out.yes_entry_price = b.yes_entry_price;
    } else if (out.yes_shares > kLihConsolidateTol) {
        out.yes_entry_price = out.yes_cost / out.yes_shares;
    }
    if (a.no_shares > kLihConsolidateTol && a.no_entry_price > 0) {
        out.no_entry_price = a.no_entry_price;
    } else if (b.no_shares > kLihConsolidateTol && b.no_entry_price > 0) {
        out.no_entry_price = b.no_entry_price;
    } else if (out.no_shares > kLihConsolidateTol) {
        out.no_entry_price = out.no_cost / out.no_shares;
    }
    const bool both_dup = lih_dominant_side(a) == 'B' && lih_dominant_side(b) == 'B';
    if (!both_dup) {
        out.entry_fees = a.entry_fees + b.entry_fees;
        out.rebalance_count = a.rebalance_count + b.rebalance_count;
    }
    out.opened_at = std::min(a.opened_at, b.opened_at);
    out.end_date_ts = leg1.end_date_ts > 0 ? leg1.end_date_ts : leg2.end_date_ts;
    out.is_neg_risk = leg1.is_neg_risk || leg2.is_neg_risk;
    out.paper_mode = leg1.paper_mode || leg2.paper_mode;
    out.is_shadow = false;
    if (a.yes_shares > kLihConsolidateTol) out.yes_exit_price = a.yes_exit_price;
    else if (b.yes_shares > kLihConsolidateTol) out.yes_exit_price = b.yes_exit_price;
    if (a.no_shares > kLihConsolidateTol) out.no_exit_price = a.no_exit_price;
    else if (b.no_shares > kLihConsolidateTol) out.no_exit_price = b.no_exit_price;
    out.closed_at = std::max(a.closed_at.value_or(0.0), b.closed_at.value_or(0.0));
    if (both_dup) {
        out.pnl_usdc = leg2.pnl_usdc.value_or(leg1.pnl_usdc.value_or(0.0));
    } else {
        out.pnl_usdc = a.pnl_usdc.value_or(0.0) + b.pnl_usdc.value_or(0.0);
    }
    out.exit_reason = leg2.exit_reason.empty() ? leg1.exit_reason : leg1.exit_reason;
    if (out.exit_reason.find("hedged") == std::string::npos &&
        (leg1.exit_reason.find("hedged") != std::string::npos ||
         leg2.exit_reason.find("hedged") != std::string::npos)) {
        out.exit_reason = "Market resolved (hedged)";
    }
    return out;
}
} // namespace

void RiskManager::consolidate_closed_lih_positions() {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    const size_t before = closed_lih_positions_.size();
    if (before == 0) return;

    std::vector<LegInHedgePosition> sorted = closed_lih_positions_;
    std::sort(sorted.begin(), sorted.end(),
              [](const LegInHedgePosition& a, const LegInHedgePosition& b) {
                  return a.opened_at < b.opened_at;
              });

    std::vector<bool> used(sorted.size(), false);
    std::vector<LegInHedgePosition> merged;
    merged.reserve(sorted.size());

    for (size_t i = 0; i < sorted.size(); ++i) {
        if (used[i] || sorted[i].is_shadow) continue;
        LegInHedgePosition cur = sorted[i];

        if (sorted.size() >= 2 && lih_dominant_side(cur) != ' ') {
            int best_j = -1;
            double best_dt = kLihPairSec + 1.0;
            for (size_t j = i + 1; j < sorted.size(); ++j) {
                if (used[j] || sorted[j].is_shadow) continue;
                const auto& other = sorted[j];
                if (lih_asset_key(other.asset) != lih_asset_key(cur.asset)) continue;
                if (other.window_minutes != cur.window_minutes) continue;
                if (!lih_should_pair_closed(cur, other)) continue;
                double dt = std::abs(cur.opened_at - other.opened_at);
                if (cur.end_date_ts > 0 && other.end_date_ts > 0 &&
                    lih_dominant_side(cur) == 'B' && lih_dominant_side(other) == 'B') {
                    dt = std::abs(cur.end_date_ts - other.end_date_ts);
                }
                if (dt <= kLihPairSec && dt < best_dt) {
                    best_dt = dt;
                    best_j = static_cast<int>(j);
                }
            }
            if (best_j >= 0) {
                used[static_cast<size_t>(best_j)] = true;
                cur = merge_closed_lih_pair(cur, sorted[static_cast<size_t>(best_j)]);
                spdlog::info("[LIH] consolidated split closed rows -> {} | Y={:.1f} N={:.1f} pnl ${:+.2f}",
                             cur.lih_id, cur.yes_shares, cur.no_shares, cur.pnl_usdc.value_or(0.0));
            }
        }

        used[i] = true;
        merged.push_back(std::move(cur));
    }

    // Dedupe by lih_id (keep newest closed_at).
    std::unordered_map<std::string, LegInHedgePosition> by_id;
    std::vector<std::string> id_order;
    for (auto& p : merged) {
        if (p.is_shadow) continue;
        auto it = by_id.find(p.lih_id);
        if (it == by_id.end()) {
            id_order.push_back(p.lih_id);
            by_id[p.lih_id] = std::move(p);
            continue;
        }
        const double keep_ts = it->second.closed_at.value_or(0.0);
        const double new_ts = p.closed_at.value_or(0.0);
        if (new_ts >= keep_ts) it->second = std::move(p);
    }
    merged.clear();
    merged.reserve(by_id.size());
    for (const auto& id : id_order) {
        auto it = by_id.find(id);
        if (it != by_id.end()) merged.push_back(std::move(it->second));
    }

    double lih_sum = 0.0;
    for (const auto& p : merged) {
        lih_sum += p.pnl_usdc.value_or(0.0);
    }
    lih_pnl_ = lih_sum;
    total_pnl_ = lih_pnl_ + dh_pnl_ + la_pnl_;

    if (merged.size() != before) {
        spdlog::info("[LIH] closed history consolidated {} -> {} row(s), lih_pnl ${:+.2f}",
                     before, merged.size(), lih_pnl_);
    }
    closed_lih_positions_ = std::move(merged);
}

void RiskManager::set_lih_pause_after_round(bool v) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    lih_pause_after_round_ = v;
    spdlog::info("Risk config updated | lih_pause_after_round={}", v);
}

bool RiskManager::get_lih_pause_after_round() const {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    return lih_pause_after_round_;
}

void RiskManager::maybe_pause_after_lih_round(const std::string& trigger) {
    (void)trigger;
    // DEBUG single-round test: re-enable to auto-pause after each round (LIH_PAUSE_AFTER_ROUND).
    // if (!lih_pause_after_round_ || status_ != TradingStatus::ACTIVE) return;
    // pause("LIH round complete — " + trigger);
    // spdlog::info("[LIH] Auto-pause after round | {}", trigger);
}

void RiskManager::set_lih_min_balance_usdc(double v) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    lih_min_balance_usdc_ = std::max(0.0, v);
    spdlog::info("Risk config updated | lih_min_balance_usdc={:.2f}", lih_min_balance_usdc_);
}

double RiskManager::get_lih_min_balance_usdc() const {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    return lih_min_balance_usdc_;
}

bool RiskManager::try_begin_lih_leg1(const std::string& asset, int window_minutes) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    const std::string key = lih_slot_key(asset, window_minutes);
    std::string want = asset;
    std::transform(want.begin(), want.end(), want.begin(), ::tolower);
    for (const auto& [id, p] : open_lih_positions_) {
        (void)id;
        std::string have = p.asset;
        std::transform(have.begin(), have.end(), have.begin(), ::tolower);
        if (have == want && p.window_minutes == window_minutes) {
            lih_leg1_inflight_.erase(key);
            return false;
        }
    }
    if (lih_leg1_inflight_.count(key)) return false;
    if (lih_other_slot_busy_unlocked(asset, window_minutes)) {
        spdlog::info("[LIH] LEG1 blocked {} {}m — another slot is active/in-flight", asset, window_minutes);
        return false;
    }
    // DEBUG single-round test: re-enable session leg cap (LIH_SESSION_MAX_LEGS).
    // if (lih_session_max_legs_ > 0 && lih_session_legs_used_ >= lih_session_max_legs_) {
    //     spdlog::info("[LIH] LEG1 blocked {} {}m — session leg cap {} reached",
    //                  asset, window_minutes, lih_session_max_legs_);
    //     return false;
    // }
    if (lih_min_balance_usdc_ > 0.0 && current_balance_ + 1e-6 < lih_min_balance_usdc_) {
        spdlog::info("[LIH] LEG1 blocked {} {}m — balance ${:.2f} < min ${:.2f}",
                     asset, window_minutes, current_balance_, lih_min_balance_usdc_);
        return false;
    }
    if (get_open_position_count() >= max_concurrent_positions_) return false;
    lih_leg1_inflight_.insert(key);
    lih_leg1_inflight_since_[key] = static_cast<double>(std::time(nullptr));
    return true;
}

void RiskManager::end_lih_leg1_inflight(const std::string& asset, int window_minutes) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    const std::string key = lih_slot_key(asset, window_minutes);
    lih_leg1_inflight_.erase(key);
    lih_leg1_inflight_since_.erase(key);
}

bool RiskManager::lih_rebalance_inflight(const std::string& lih_id) const {
    if (lih_id.empty()) return false;
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    return lih_rebalance_inflight_.count(lih_id) > 0;
}

bool RiskManager::try_begin_lih_rebalance(const std::string& lih_id) {
    if (lih_id.empty()) return false;
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    if (lih_rebalance_inflight_.count(lih_id)) return false;
    if (!open_lih_positions_.count(lih_id)) return false;
    lih_rebalance_inflight_.insert(lih_id);
    return true;
}

void RiskManager::end_lih_rebalance_inflight(const std::string& lih_id) {
    if (lih_id.empty()) return;
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    lih_rebalance_inflight_.erase(lih_id);
}

std::unordered_map<std::string, LegInHedgePosition> RiskManager::get_open_lih_positions() const {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    return open_lih_positions_;
}

LegInHedgePosition RiskManager::register_lih_open_leg1(
    const trading::MarketInfo& market, bool buy_yes, double price, double shares, double now_sec,
    bool is_paper, bool debit_balance, bool is_shadow) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    LegInHedgePosition pos;
    pos.lih_id = "LIH-" + market.asset + "-" + std::to_string(static_cast<uint64_t>(now_sec * 1000.0));
    pos.asset = market.asset;
    std::transform(pos.asset.begin(), pos.asset.end(), pos.asset.begin(), ::tolower);
    pos.market_question = market.question;
    pos.yes_token_id = market.yes_token_id;
    pos.no_token_id = market.no_token_id;
    pos.condition_id = market.condition_id;
    pos.end_date_ts = market.end_date_ts;
    pos.window_minutes = market.window_minutes;
    pos.is_neg_risk = market.is_neg_risk;
    pos.opened_at = now_sec;
    pos.paper_mode = is_paper;
    pos.is_shadow = is_shadow;

    const double cost = price * shares;
    const double fee = cost * fee_rate_;
    if (buy_yes) {
        pos.yes_shares = shares;
        pos.yes_cost = cost;
        pos.yes_entry_price = price;
    } else {
        pos.no_shares = shares;
        pos.no_cost = cost;
        pos.no_entry_price = price;
    }
    pos.entry_fees = fee;
    if (debit_balance) {
        current_balance_ -= (cost + fee);
    }
    open_lih_positions_[pos.lih_id] = pos;
    {
        const std::string key = lih_slot_key(market.asset, market.window_minutes);
        lih_leg1_inflight_.erase(key);
        lih_leg1_inflight_since_.erase(key);
    }
    if (!is_paper && debit_balance) {
        ++lih_session_legs_used_;
    }
    if (!is_shadow) {
        total_lih_trades_++;
    }
    const char* mode_tag = is_shadow ? "SHADOW" : "LIVE";
    spdlog::info("[LIH {}] LEG1 {} | {} {:.2f}sh @ {:.4f} | cost ${:.2f} | bal ${:.2f}",
                 mode_tag,
                 pos.lih_id, buy_yes ? "YES" : "NO", shares, price, cost, current_balance_);
    return pos;
}

void RiskManager::register_lih_add_leg(
    const std::string& lih_id, bool buy_yes, double price, double shares, bool is_paper,
    bool debit_balance) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    auto it = open_lih_positions_.find(lih_id);
    if (it == open_lih_positions_.end()) return;
    lih_rebalance_inflight_.erase(lih_id);
    {
        const std::string key = lih_slot_key(it->second.asset, it->second.window_minutes);
        lih_leg1_inflight_.erase(key);
        lih_leg1_inflight_since_.erase(key);
    }
    ++it->second.rebalance_count;
    const int n = it->second.rebalance_count;
    const double cost = price * shares;
    const double fee = cost * fee_rate_;
    if (buy_yes) {
        const double prev = it->second.yes_shares;
        it->second.yes_entry_price = prev > kFloatTol
            ? (it->second.yes_entry_price * prev + price * shares) / (prev + shares)
            : price;
        it->second.yes_shares += shares;
        it->second.yes_cost += cost;
    } else {
        const double prev = it->second.no_shares;
        it->second.no_entry_price = prev > kFloatTol
            ? (it->second.no_entry_price * prev + price * shares) / (prev + shares)
            : price;
        it->second.no_shares += shares;
        it->second.no_cost += cost;
    }
    it->second.entry_fees += fee;
    if (debit_balance) {
        current_balance_ -= (cost + fee);
    }
    if (!is_paper && debit_balance) {
        ++lih_session_legs_used_;
        // DEBUG single-round test: re-enable auto-pause when session cap hit on hedge.
        // if (lih_session_max_legs_ > 0 && lih_session_legs_used_ >= lih_session_max_legs_) {
        //     maybe_pause_after_lih_round("hedge complete");
        // }
    }
    const char* mode_tag = !debit_balance ? "SHADOW" : "LIVE";
    spdlog::info("[LIH {}] HEDGE {} | {} +{:.2f}sh @ {:.4f} | YES {:.2f} NO {:.2f} | #{:d} | bal ${:.2f}",
                 mode_tag,
                 lih_id, buy_yes ? "YES" : "NO", shares, price,
                 it->second.yes_shares, it->second.no_shares, n, current_balance_);
}

void RiskManager::register_lih_add_paired(
    const std::string& lih_id, double yes_price, double no_price, double shares, bool is_paper,
    bool debit_balance) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    auto it = open_lih_positions_.find(lih_id);
    if (it == open_lih_positions_.end()) return;
    lih_rebalance_inflight_.erase(lih_id);
    ++it->second.rebalance_count;
    const int n = it->second.rebalance_count;
    const double yes_cost = yes_price * shares;
    const double no_cost = no_price * shares;
    const double fee = (yes_cost + no_cost) * fee_rate_;
    it->second.yes_shares += shares;
    it->second.no_shares += shares;
    it->second.yes_cost += yes_cost;
    it->second.no_cost += no_cost;
    it->second.entry_fees += fee;
    if (debit_balance) {
        current_balance_ -= (yes_cost + no_cost + fee);
    }
    const char* mode_tag = !debit_balance ? "SHADOW" : "LIVE";
    spdlog::info("[LIH {}] SCALE {} | +{:.2f} paired | YES {:.2f} NO {:.2f} | #{:d} | bal ${:.2f}",
                 mode_tag,
                 lih_id, shares, it->second.yes_shares, it->second.no_shares, n, current_balance_);
}

std::optional<LegInHedgePosition> RiskManager::register_lih_close(
    const std::string& lih_id,
    double yes_exit,
    double no_exit,
    const std::string& exit_reason,
    std::optional<double> exit_timestamp) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    for (const auto& closed : closed_lih_positions_) {
        if (closed.lih_id == lih_id) {
            spdlog::debug("[LIH] close skip {} — already in closed history", lih_id);
            return std::nullopt;
        }
    }
    auto it = open_lih_positions_.find(lih_id);
    if (it == open_lih_positions_.end()) return std::nullopt;

    LegInHedgePosition pos = it->second;
    if (pos.is_shadow) {
        open_lih_positions_.erase(it);
        lih_rebalance_inflight_.erase(lih_id);
        spdlog::info("[LIH SHADOW] discarded {} | {} (no trade record)", lih_id, exit_reason);
        return std::nullopt;
    }

    const double matched = std::min(pos.yes_shares, pos.no_shares);
    const double yes_proceeds = pos.yes_shares * yes_exit;
    const double no_proceeds = pos.no_shares * no_exit;
    const double total_cost = pos.yes_cost + pos.no_cost;
    const double exit_fee = (yes_proceeds + no_proceeds) * fee_rate_;
    const double proceeds = yes_proceeds + no_proceeds - exit_fee;
    const double pnl = proceeds - total_cost - pos.entry_fees;

    pos.closed_at = exit_timestamp.value_or(now());
    pos.yes_exit_price = yes_exit;
    pos.no_exit_price = no_exit;
    pos.pnl_usdc = pnl;
    pos.exit_reason = exit_reason;
    current_balance_ += proceeds;
    lih_pnl_ += pnl;
    total_pnl_ += pnl;
    if (pnl > 0) winning_trades_++;
    record_asset_close(pos.asset, pnl, pnl > 0);

    open_lih_positions_.erase(it);
    lih_rebalance_inflight_.erase(lih_id);
    {
        const std::string key = lih_slot_key(pos.asset, pos.window_minutes);
        lih_leg1_inflight_.erase(key);
        lih_leg1_inflight_since_.erase(key);
    }
    closed_lih_positions_.push_back(pos);
    if (closed_lih_positions_.size() > 500) {
        closed_lih_positions_.erase(closed_lih_positions_.begin());
    }

    spdlog::info("[LIH {}] CLOSED {} | matched {:.2f} | PnL ${:+.2f} | rebal #{:d} | {} | bal ${:.2f}",
                 pos.paper_mode ? "PAPER" : "LIVE",
                 lih_id, matched, pnl, pos.rebalance_count, exit_reason, current_balance_);
    if (!pos.paper_mode) {
        // DEBUG single-round test: re-enable auto-pause on round close (LIH_PAUSE_AFTER_ROUND).
        // maybe_pause_after_lih_round(exit_reason);
        reset_lih_session();
    }
    check_risk_thresholds();
    consolidate_closed_lih_positions();
    return pos;
}

void RiskManager::sync_lih_from_markets(const std::vector<trading::MarketInfo>& markets) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    for (auto& [id, p] : open_lih_positions_) {
        (void)id;
        for (const auto& m : markets) {
            if (p.asset != m.asset || p.window_minutes != m.window_minutes) continue;

            bool token_match = false;
            if (!p.yes_token_id.empty() &&
                (p.yes_token_id == m.yes_token_id || p.yes_token_id == m.no_token_id)) {
                token_match = true;
            }
            if (!p.no_token_id.empty() &&
                (p.no_token_id == m.yes_token_id || p.no_token_id == m.no_token_id)) {
                token_match = true;
            }
            const bool end_match = p.end_date_ts > 0 && m.end_date_ts > 0 &&
                                   std::abs(p.end_date_ts - m.end_date_ts) < 2.0;
            if (!token_match && !end_match) continue;

            if (p.end_date_ts <= 0 && m.end_date_ts > 0) p.end_date_ts = m.end_date_ts;
            if (p.condition_id.empty() && !m.condition_id.empty()) p.condition_id = m.condition_id;
            if (p.yes_token_id.empty()) p.yes_token_id = m.yes_token_id;
            if (p.no_token_id.empty()) p.no_token_id = m.no_token_id;
            if (p.market_question.empty()) p.market_question = m.question;
            break;
        }
    }
}

int RiskManager::purge_expired_lih_open(double now_sec, double grace_sec) {
    std::vector<std::string> ids;
    {
        std::lock_guard<std::recursive_mutex> lock(mtx_);
        for (const auto& [id, p] : open_lih_positions_) {
            if (p.end_date_ts > 0 && now_sec > p.end_date_ts + grace_sec) {
                ids.push_back(id);
            }
        }
    }
    int closed = 0;
    for (const auto& id : ids) {
        bool shadow = false;
        {
            std::lock_guard<std::recursive_mutex> lock(mtx_);
            auto it = open_lih_positions_.find(id);
            if (it != open_lih_positions_.end()) shadow = it->second.is_shadow;
        }
        if (shadow) {
            std::lock_guard<std::recursive_mutex> lock(mtx_);
            auto it = open_lih_positions_.find(id);
            if (it != open_lih_positions_.end()) {
                const std::string key = lih_slot_key(it->second.asset, it->second.window_minutes);
                lih_leg1_inflight_.erase(key);
                lih_leg1_inflight_since_.erase(key);
                open_lih_positions_.erase(it);
            }
            lih_rebalance_inflight_.erase(id);
            closed++;
            continue;
        }
        if (register_lih_close(id, 0.5, 0.5, "Expired window (purged)", now_sec)) {
            closed++;
        }
    }
    if (closed > 0) {
        spdlog::info("Purged {} expired LIH open position(s) from dashboard", closed);
    }
    return closed;
}

void RiskManager::update_balance(double new_balance) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    double old = current_balance_;
    current_balance_ = new_balance;
    if (new_balance > peak_balance_) {
        peak_balance_ = new_balance;
    }
    spdlog::debug("Balance updated: ${:.2f} -> ${:.2f} (Δ${:+.2f})", old, new_balance, new_balance - old);
    check_risk_thresholds();
}

void RiskManager::set_daily_starting_balance(double balance) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    daily_starting_balance_ = balance;
    spdlog::debug("Daily starting balance set to ${:.2f}", balance);
}

void RiskManager::set_live_starting_balance(double balance) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    starting_balance_ = balance;
    current_balance_ = balance;
    peak_balance_ = balance;
    daily_starting_balance_ = balance;

    if (status_ == TradingStatus::DAILY_HALT || status_ == TradingStatus::KILLED) {
        status_ = TradingStatus::ACTIVE;
        kill_reason_ = std::nullopt;
    }

    spdlog::info("Live baseline balances reset to ${:.2f}", balance);
}

double RiskManager::get_max_position_fraction() const {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    return max_position_fraction_;
}

double RiskManager::get_daily_loss_limit() const {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    return daily_loss_limit_;
}

double RiskManager::get_total_drawdown_kill() const {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    return total_drawdown_kill_;
}

int RiskManager::get_max_concurrent_positions() const {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    return max_concurrent_positions_;
}

void RiskManager::set_max_position_fraction(double v) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    max_position_fraction_ = v;
    spdlog::info("Risk config updated | max_position_fraction={:.2f}", v);
}

void RiskManager::set_daily_loss_limit(double v) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    daily_loss_limit_ = v;
    spdlog::info("Risk config updated | daily_loss_limit={:.2f}", v);
}

void RiskManager::set_total_drawdown_kill(double v) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    total_drawdown_kill_ = v;
    spdlog::info("Risk config updated | total_drawdown_kill={:.2f}", v);
}

void RiskManager::set_max_concurrent_positions(int v) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    max_concurrent_positions_ = v;
    spdlog::info("Risk config updated | max_concurrent_positions={}", v);
}

void RiskManager::pause(const std::string& reason) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    if (status_ == TradingStatus::KILLED) return;
    circuit_breaker_resume_at_ = 0.0;
    const bool was_active = status_ == TradingStatus::ACTIVE;
    status_ = TradingStatus::PAUSED;
    kill_reason_ = reason;
    if (was_active) {
        spdlog::warn("Trading PAUSED: {}", reason);
    }
}

bool RiskManager::resume() {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    if (status_ == TradingStatus::KILLED) {
        spdlog::error("Cannot resume: kill switch has been triggered.");
        return false;
    }
    if (status_ == TradingStatus::PAUSED || status_ == TradingStatus::DAILY_HALT) {
        const bool was_daily = status_ == TradingStatus::DAILY_HALT;
        status_ = TradingStatus::ACTIVE;
        kill_reason_ = std::nullopt;
        if (was_daily) {
            daily_starting_balance_ = current_balance_;
        }
        spdlog::info("Trading RESUMED.");
        return true;
    }
    return status_ == TradingStatus::ACTIVE;
}

bool RiskManager::reset_kill_switch(bool confirm) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    if (!confirm) {
        spdlog::error("reset_kill_switch requires confirm=true.");
        return false;
    }
    if (status_ == TradingStatus::KILLED) {
        status_ = TradingStatus::ACTIVE;
        kill_reason_ = std::nullopt;
        daily_starting_balance_ = current_balance_;
        spdlog::warn("KILL SWITCH RESET manually. Trading resumed. Balance: ${:.2f}", current_balance_);
        return true;
    }
    return false;
}

double RiskManager::net_lih_round_pnl(const LegInHedgePosition& p) const {
    if (!p.pnl_usdc) return 0.0;
    // New closes store net pnl (proceeds - cost - entry_fees) with entry_fees populated.
    return *p.pnl_usdc;
}

double RiskManager::compute_equity_unlocked() const {
    double equity = current_balance_;
    for (const auto& [id, p] : open_positions_) {
        (void)id;
        equity += p.cost_usdc;
    }
    for (const auto& [id, p] : open_dh_positions_) {
        (void)id;
        equity += p.combined_cost_usdc;
    }
    for (const auto& [id, p] : open_lih_positions_) {
        (void)id;
        equity += p.yes_cost + p.no_cost;
    }
    return equity;
}

void RiskManager::reconcile_paper_balance(bool reset_trading_halt) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    double lih_sum = 0.0;
    bool all_closed_have_entry_fees = !closed_lih_positions_.empty();
    for (const auto& p : closed_lih_positions_) {
        if (p.pnl_usdc) lih_sum += *p.pnl_usdc;
        if (p.entry_fees <= 0.0) all_closed_have_entry_fees = false;
    }
    lih_pnl_ = lih_sum;
    total_pnl_ = lih_pnl_ + dh_pnl_ + la_pnl_;

    const double old = current_balance_;
    if (all_closed_have_entry_fees) {
        double cash = starting_balance_ + lih_sum;
        for (const auto& [id, p] : open_lih_positions_) {
            (void)id;
            const double fees = p.entry_fees > 0.0 ? p.entry_fees : (p.yes_cost + p.no_cost) * fee_rate_;
            cash -= p.yes_cost + p.no_cost + fees;
        }
        current_balance_ = cash;
    } else {
        spdlog::info(
            "Paper reconcile: legacy LIH snapshot (no entry_fees on all closed) — keeping loaded cash ${:.2f}",
            current_balance_);
    }

    if (reset_trading_halt && (status_ == TradingStatus::KILLED || status_ == TradingStatus::DAILY_HALT)) {
        status_ = TradingStatus::ACTIVE;
        kill_reason_ = std::nullopt;
    }
    const double equity = compute_equity_unlocked();
    peak_balance_ = std::max(peak_balance_, equity);
    daily_starting_balance_ = current_balance_;
    spdlog::info(
        "Paper balance reconciled | ${:.2f} -> ${:.2f} | lih_pnl ${:.2f} | equity ${:.2f} | status {}",
        old, current_balance_, lih_pnl_, equity, static_cast<int>(status_));
}

void RiskManager::check_risk_thresholds() {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    if (status_ == TradingStatus::KILLED) return;

    const double equity = compute_equity_unlocked();

    if (equity > peak_balance_) {
        peak_balance_ = equity;
    }

    if (peak_balance_ > 0) {
        double drawdown_pct = (peak_balance_ - equity) / peak_balance_;
        if (drawdown_pct >= total_drawdown_kill_) {
            trigger_kill_switch(
                "Total drawdown " + std::to_string(drawdown_pct * 100.0) + "% exceeded threshold. " +
                "Peak: $" + std::to_string(peak_balance_) + " -> Equity: $" + std::to_string(equity)
            );
            return;
        }
    }

    if (daily_starting_balance_ > 0) {
        double daily_loss_pct = (daily_starting_balance_ - equity) / daily_starting_balance_;
        if (daily_loss_pct >= daily_loss_limit_) {
            trigger_daily_halt(
                "Daily loss " + std::to_string(daily_loss_pct * 100.0) + "% exceeded limit. " +
                "Daily start: $" + std::to_string(daily_starting_balance_) + " -> Equity: $" + std::to_string(equity)
            );
        }
    }
}

void RiskManager::check_circuit_breaker() {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    if (!circuit_breaker_enabled_) return;
    if (status_ != TradingStatus::ACTIVE) return;

    double threshold = current_balance_ * circuit_breaker_loss_pct_;

    auto evaluate_cb = [&](const std::deque<double>& pnls, const std::string& strategy) -> std::optional<std::string> {
        if (pnls.size() < static_cast<size_t>(circuit_breaker_min_losses_)) return std::nullopt;

        std::vector<double> losses;
        for (double p : pnls) {
            if (p < 0.0) losses.push_back(p);
        }

        if (losses.size() < static_cast<size_t>(circuit_breaker_min_losses_)) return std::nullopt;

        double cumulative_loss = 0.0;
        for (double l : losses) cumulative_loss += std::abs(l);

        if (cumulative_loss >= threshold) {
            double resume_at = now() + circuit_breaker_pause_seconds_;
            circuit_breaker_resume_at_ = resume_at;
            status_ = TradingStatus::PAUSED;
            kill_reason_ = "Circuit breaker (" + strategy + "): " + std::to_string(losses.size()) + " losses in last " +
                           std::to_string(pnls.size()) + " trades, cumulative loss $" + std::to_string(cumulative_loss) +
                           " > " + std::to_string(circuit_breaker_loss_pct_ * 100.0) + "% of balance. Pausing.";
                           
            return fmt::format("CIRCUIT BREAKER triggered [{}] - trading paused for {}s. Resumes at unix {}.", strategy, circuit_breaker_pause_seconds_, resume_at);
        }
        return std::nullopt;
    };

    if (auto msg = evaluate_cb(recent_dh_pnls_, "DH")) {
        spdlog::warn("{}", *msg);
        return;
    }
}

void RiskManager::check_circuit_breaker_resume() {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    if (status_ == TradingStatus::PAUSED && circuit_breaker_resume_at_ > 0.0 && now() >= circuit_breaker_resume_at_) {
        circuit_breaker_resume_at_ = 0.0;
        status_ = TradingStatus::ACTIVE;
        kill_reason_ = std::nullopt;
        recent_la_pnls_.clear();
        recent_dh_pnls_.clear();
        spdlog::info("Circuit breaker pause expired - trading RESUMED.");
    }
}

int RiskManager::close_legacy_la_positions() {
    struct LegacyClose { std::string id; double exit_price; double proceeds; };
    std::vector<LegacyClose> to_close;
    {
        std::lock_guard<std::recursive_mutex> lock(mtx_);
        for (const auto& [id, p] : open_positions_) {
            if (p.strategy == "LA") {
                to_close.push_back({id, p.entry_price, p.cost_usdc * (1.0 + fee_rate_)});
            }
        }
    }
    int closed = 0;
    for (const auto& lc : to_close) {
        if (register_trade_close(lc.id, lc.exit_price, std::nullopt, lc.proceeds).has_value()) {
            ++closed;
            spdlog::info("Legacy LA position closed | {} | proceeds ${:.2f}", lc.id, lc.proceeds);
        }
    }
    return closed;
}

void RiskManager::trigger_kill_switch(const std::string& reason) {
    status_ = TradingStatus::KILLED;
    kill_reason_ = reason;
    spdlog::critical("KILL SWITCH TRIGGERED - ALL TRADING HALTED");
    spdlog::critical("Reason: {}", reason);
    spdlog::critical("Call reset_kill_switch(confirm=true) to resume.");
}

void RiskManager::trigger_daily_halt(const std::string& reason) {
    if (status_ != TradingStatus::DAILY_HALT) {
        status_ = TradingStatus::DAILY_HALT;
        kill_reason_ = reason;
        spdlog::warn("DAILY HALT TRIGGERED - Trading paused until midnight UTC.");
        spdlog::warn("Reason: {}", reason);
    }
}

void RiskManager::check_daily_reset() {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    if (now() >= daily_reset_time_) {
        if (status_ == TradingStatus::DAILY_HALT) {
            status_ = TradingStatus::ACTIVE;
            kill_reason_ = std::nullopt;
            spdlog::info("Daily halt reset at midnight UTC. Trading resumed. "
                         "New daily starting balance: ${:.2f}", current_balance_);
        }
        daily_starting_balance_ = current_balance_;
        daily_reset_time_ = next_midnight();
    }
}

void RiskManager::record_asset_close(const std::string& asset, double pnl, bool won) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    if (asset.empty()) return;
    asset_trades_[asset]++;
    if (won) asset_wins_[asset]++;
    asset_pnl_[asset] += pnl;
}

namespace {

boost::json::object position_to_json(const Position& p) {
    boost::json::object o;
    o["order_id"] = p.order_id;
    o["token_id"] = p.token_id;
    o["market_question"] = p.market_question;
    o["side"] = p.side;
    o["entry_price"] = p.entry_price;
    o["size_shares"] = p.size_shares;
    o["cost_usdc"] = p.cost_usdc;
    o["opened_at"] = p.opened_at;
    o["end_date_ts"] = p.end_date_ts;
    o["asset"] = p.asset;
    o["direction"] = p.direction;
    o["strategy"] = p.strategy;
    o["condition_id"] = p.condition_id;
    o["paper_mode"] = p.paper_mode;
    o["peak_price"] = p.peak_price;
    o["is_neg_risk"] = p.is_neg_risk;
    if (p.closed_at) o["closed_at"] = *p.closed_at;
    if (p.exit_price) o["exit_price"] = *p.exit_price;
    if (p.pnl_usdc) o["pnl_usdc"] = *p.pnl_usdc;
    return o;
}

bool position_from_json(const boost::json::object& o, Position& p) {
    try {
        p.order_id = std::string(o.at("order_id").as_string());
        p.token_id = std::string(o.at("token_id").as_string());
        p.market_question = o.contains("market_question") ? std::string(o.at("market_question").as_string()) : "";
        p.side = o.contains("side") ? std::string(o.at("side").as_string()) : "BUY";
        p.entry_price = o.at("entry_price").as_double();
        p.size_shares = o.at("size_shares").as_double();
        p.cost_usdc = o.at("cost_usdc").as_double();
        p.opened_at = o.at("opened_at").as_double();
        p.end_date_ts = o.contains("end_date_ts") ? o.at("end_date_ts").as_double() : 0.0;
        p.asset = o.contains("asset") ? std::string(o.at("asset").as_string()) : "";
        p.direction = o.contains("direction") ? std::string(o.at("direction").as_string()) : "";
        p.strategy = o.contains("strategy") ? std::string(o.at("strategy").as_string()) : "LA";
        p.condition_id = o.contains("condition_id") ? std::string(o.at("condition_id").as_string()) : "";
        p.paper_mode = o.contains("paper_mode") && o.at("paper_mode").as_bool();
        p.peak_price = o.contains("peak_price") ? o.at("peak_price").as_double() : 0.0;
        p.is_neg_risk = o.contains("is_neg_risk") && o.at("is_neg_risk").as_bool();
        p.closed_at = std::nullopt;
        p.exit_price = std::nullopt;
        p.pnl_usdc = std::nullopt;
        if (o.contains("closed_at")) p.closed_at = o.at("closed_at").as_double();
        if (o.contains("exit_price")) p.exit_price = o.at("exit_price").as_double();
        if (o.contains("pnl_usdc")) p.pnl_usdc = o.at("pnl_usdc").as_double();
        return true;
    } catch (...) {
        return false;
    }
}

boost::json::object dh_position_to_json(const DumpHedgePosition& p) {
    boost::json::object o;
    o["dh_id"] = p.dh_id;
    o["yes_order_id"] = p.yes_order_id;
    o["no_order_id"] = p.no_order_id;
    o["yes_token_id"] = p.yes_token_id;
    o["no_token_id"] = p.no_token_id;
    o["market_question"] = p.market_question;
    o["asset"] = p.asset;
    o["yes_entry_price"] = p.yes_entry_price;
    o["no_entry_price"] = p.no_entry_price;
    o["combined_entry_price"] = p.combined_entry_price;
    o["size_shares"] = p.size_shares;
    o["combined_cost_usdc"] = p.combined_cost_usdc;
    o["locked_profit_usdc"] = p.locked_profit_usdc;
    o["opened_at"] = p.opened_at;
    o["end_date_ts"] = p.end_date_ts;
    o["paper_mode"] = p.paper_mode;
    o["strategy"] = p.strategy;
    o["is_neg_risk"] = p.is_neg_risk;
    o["window_minutes"] = p.window_minutes;
    if (!p.condition_id.empty()) o["condition_id"] = p.condition_id;
    o["exit_reason"] = p.exit_reason;
    if (p.closed_at) o["closed_at"] = *p.closed_at;
    if (p.yes_exit_price) o["yes_exit_price"] = *p.yes_exit_price;
    if (p.no_exit_price) o["no_exit_price"] = *p.no_exit_price;
    if (p.pnl_usdc) o["pnl_usdc"] = *p.pnl_usdc;
    return o;
}

bool dh_position_from_json(const boost::json::object& o, DumpHedgePosition& p) {
    try {
        p.dh_id = std::string(o.at("dh_id").as_string());
        p.yes_order_id = o.contains("yes_order_id") ? std::string(o.at("yes_order_id").as_string()) : "";
        p.no_order_id = o.contains("no_order_id") ? std::string(o.at("no_order_id").as_string()) : "";
        p.yes_token_id = o.contains("yes_token_id") ? std::string(o.at("yes_token_id").as_string()) : "";
        p.no_token_id = o.contains("no_token_id") ? std::string(o.at("no_token_id").as_string()) : "";
        p.market_question = o.contains("market_question") ? std::string(o.at("market_question").as_string()) : "";
        p.asset = o.contains("asset") ? std::string(o.at("asset").as_string()) : "";
        p.yes_entry_price = o.at("yes_entry_price").as_double();
        p.no_entry_price = o.at("no_entry_price").as_double();
        p.combined_entry_price = o.at("combined_entry_price").as_double();
        p.size_shares = o.at("size_shares").as_double();
        p.combined_cost_usdc = o.at("combined_cost_usdc").as_double();
        p.locked_profit_usdc = o.contains("locked_profit_usdc") ? o.at("locked_profit_usdc").as_double() : 0.0;
        p.opened_at = o.at("opened_at").as_double();
        p.end_date_ts = o.contains("end_date_ts") ? o.at("end_date_ts").as_double() : 0.0;
        p.paper_mode = o.contains("paper_mode") && o.at("paper_mode").as_bool();
        p.strategy = o.contains("strategy") ? std::string(o.at("strategy").as_string()) : "DH";
        p.is_neg_risk = o.contains("is_neg_risk") && o.at("is_neg_risk").as_bool();
        p.window_minutes = o.contains("window_minutes") ? static_cast<int>(o.at("window_minutes").as_int64()) : 5;
        p.condition_id = o.contains("condition_id") ? std::string(o.at("condition_id").as_string()) : "";
        p.exit_reason = o.contains("exit_reason") ? std::string(o.at("exit_reason").as_string()) : "";
        p.closed_at = std::nullopt;
        p.yes_exit_price = std::nullopt;
        p.no_exit_price = std::nullopt;
        p.pnl_usdc = std::nullopt;
        if (o.contains("closed_at")) p.closed_at = o.at("closed_at").as_double();
        if (o.contains("yes_exit_price")) p.yes_exit_price = o.at("yes_exit_price").as_double();
        if (o.contains("no_exit_price")) p.no_exit_price = o.at("no_exit_price").as_double();
        if (o.contains("pnl_usdc")) p.pnl_usdc = o.at("pnl_usdc").as_double();
        return true;
    } catch (...) {
        return false;
    }
}

boost::json::object lih_position_to_json(const LegInHedgePosition& p) {
    boost::json::object o;
    o["lih_id"] = p.lih_id;
    o["asset"] = p.asset;
    o["market_question"] = p.market_question;
    o["yes_token_id"] = p.yes_token_id;
    o["no_token_id"] = p.no_token_id;
    if (!p.condition_id.empty()) o["condition_id"] = p.condition_id;
    o["yes_shares"] = p.yes_shares;
    o["no_shares"] = p.no_shares;
    o["yes_cost"] = p.yes_cost;
    o["no_cost"] = p.no_cost;
    if (p.yes_entry_price > 0) o["yes_entry_price"] = p.yes_entry_price;
    if (p.no_entry_price > 0) o["no_entry_price"] = p.no_entry_price;
    o["opened_at"] = p.opened_at;
    o["end_date_ts"] = p.end_date_ts;
    o["window_minutes"] = p.window_minutes;
    o["is_neg_risk"] = p.is_neg_risk;
    o["paper_mode"] = p.paper_mode;
    o["is_shadow"] = p.is_shadow;
    o["rebalance_count"] = p.rebalance_count;
    o["entry_fees"] = p.entry_fees;
    if (p.closed_at) o["closed_at"] = *p.closed_at;
    if (p.yes_exit_price) o["yes_exit_price"] = *p.yes_exit_price;
    if (p.no_exit_price) o["no_exit_price"] = *p.no_exit_price;
    if (p.pnl_usdc) o["pnl_usdc"] = *p.pnl_usdc;
    return o;
}

bool lih_position_from_json(const boost::json::object& o, LegInHedgePosition& p) {
    try {
        p.lih_id = std::string(o.at("lih_id").as_string());
        p.asset = o.contains("asset") ? std::string(o.at("asset").as_string()) : "";
        std::transform(p.asset.begin(), p.asset.end(), p.asset.begin(), ::tolower);
        p.market_question = o.contains("market_question") ? std::string(o.at("market_question").as_string()) : "";
        p.yes_token_id = o.contains("yes_token_id") ? std::string(o.at("yes_token_id").as_string()) : "";
        p.no_token_id = o.contains("no_token_id") ? std::string(o.at("no_token_id").as_string()) : "";
        p.condition_id = o.contains("condition_id") ? std::string(o.at("condition_id").as_string()) : "";
        p.yes_shares = o.at("yes_shares").as_double();
        p.no_shares = o.at("no_shares").as_double();
        p.yes_cost = o.at("yes_cost").as_double();
        p.no_cost = o.at("no_cost").as_double();
        p.yes_entry_price = o.contains("yes_entry_price") ? o.at("yes_entry_price").as_double() : 0.0;
        p.no_entry_price = o.contains("no_entry_price") ? o.at("no_entry_price").as_double() : 0.0;
        p.opened_at = o.at("opened_at").as_double();
        p.end_date_ts = o.contains("end_date_ts") ? o.at("end_date_ts").as_double() : 0.0;
        p.window_minutes = o.contains("window_minutes") ? static_cast<int>(o.at("window_minutes").as_int64()) : 5;
        p.is_neg_risk = o.contains("is_neg_risk") && o.at("is_neg_risk").as_bool();
        p.paper_mode = o.contains("paper_mode") && o.at("paper_mode").as_bool();
        p.is_shadow = o.contains("is_shadow") && o.at("is_shadow").as_bool();
        p.exit_reason = o.contains("exit_reason") ? std::string(o.at("exit_reason").as_string()) : "";
        p.rebalance_count = o.contains("rebalance_count") ? static_cast<int>(o.at("rebalance_count").as_int64()) : 0;
        p.entry_fees = o.contains("entry_fees") ? o.at("entry_fees").as_double() : 0.0;
        p.closed_at = std::nullopt;
        p.yes_exit_price = std::nullopt;
        p.no_exit_price = std::nullopt;
        p.pnl_usdc = std::nullopt;
        if (o.contains("closed_at")) p.closed_at = o.at("closed_at").as_double();
        if (o.contains("yes_exit_price")) p.yes_exit_price = o.at("yes_exit_price").as_double();
        if (o.contains("no_exit_price")) p.no_exit_price = o.at("no_exit_price").as_double();
        if (o.contains("pnl_usdc")) p.pnl_usdc = o.at("pnl_usdc").as_double();
        return true;
    } catch (...) {
        return false;
    }
}

boost::json::object string_int_map_to_json(const std::unordered_map<std::string, int>& m) {
    boost::json::object o;
    for (const auto& [k, v] : m) o[k] = v;
    return o;
}

boost::json::object string_double_map_to_json(const std::unordered_map<std::string, double>& m) {
    boost::json::object o;
    for (const auto& [k, v] : m) o[k] = v;
    return o;
}

void json_to_string_int_map(const boost::json::object& o, std::unordered_map<std::string, int>& m) {
    m.clear();
    for (const auto& kv : o) {
        if (kv.value().is_int64()) m[std::string(kv.key())] = static_cast<int>(kv.value().as_int64());
    }
}

void json_to_string_double_map(const boost::json::object& o, std::unordered_map<std::string, double>& m) {
    m.clear();
    for (const auto& kv : o) {
        if (kv.value().is_double()) m[std::string(kv.key())] = kv.value().as_double();
        else if (kv.value().is_int64()) m[std::string(kv.key())] = static_cast<double>(kv.value().as_int64());
    }
}

} // namespace

boost::json::object RiskManager::export_live_lih_state() const {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    boost::json::object root;
    root["version"] = 1;
    root["saved_at"] = now();
    root["current_balance"] = current_balance_;
    root["total_lih_trades"] = total_lih_trades_;
    root["lih_pnl"] = lih_pnl_;
    boost::json::object open_lih;
    for (const auto& [id, p] : open_lih_positions_) {
        if (p.is_shadow || p.paper_mode) continue;
        open_lih[id] = lih_position_to_json(p);
    }
    root["open_lih_positions"] = std::move(open_lih);
    boost::json::array closed_lih;
    size_t lih_start = closed_lih_positions_.size() > 200 ? closed_lih_positions_.size() - 200 : 0;
    for (size_t i = lih_start; i < closed_lih_positions_.size(); ++i) {
        if (closed_lih_positions_[i].is_shadow || closed_lih_positions_[i].paper_mode) continue;
        closed_lih.push_back(lih_position_to_json(closed_lih_positions_[i]));
    }
    root["closed_lih_positions"] = std::move(closed_lih);
    // leg1 in-flight locks are ephemeral — never persist (reload caused ghost blocks).
    root["lih_session_legs_used"] = lih_session_legs_used_;
    root["lih_session_max_legs"] = lih_session_max_legs_;
    return root;
}

bool RiskManager::import_live_lih_state(const boost::json::object& doc) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    try {
        if (!doc.contains("version") || doc.at("version").as_int64() != 1) return false;
        if (doc.contains("current_balance")) current_balance_ = doc.at("current_balance").as_double();
        if (doc.contains("total_lih_trades")) {
            total_lih_trades_ = static_cast<int>(doc.at("total_lih_trades").as_int64());
        }
        if (doc.contains("lih_pnl")) lih_pnl_ = doc.at("lih_pnl").as_double();

        open_lih_positions_.clear();
        const double now_sec = now();
        if (doc.contains("open_lih_positions") && doc.at("open_lih_positions").is_object()) {
            for (const auto& kv : doc.at("open_lih_positions").as_object()) {
                LegInHedgePosition p;
                if (lih_position_from_json(kv.value().as_object(), p)) {
                    if (p.paper_mode || p.is_shadow) continue;
                    if (p.end_date_ts > 0 && now_sec > p.end_date_ts + 5.0) {
                        spdlog::debug("import_live_lih_state: skip expired open {}", p.lih_id);
                        continue;
                    }
                    open_lih_positions_[p.lih_id] = p;
                }
            }
        }

        closed_lih_positions_.clear();
        if (doc.contains("closed_lih_positions") && doc.at("closed_lih_positions").is_array()) {
            for (const auto& v : doc.at("closed_lih_positions").as_array()) {
                LegInHedgePosition p;
                if (lih_position_from_json(v.as_object(), p)) {
                    if (p.is_shadow || p.paper_mode) continue;
                    closed_lih_positions_.push_back(p);
                }
            }
        }

        consolidate_closed_lih_positions();

        lih_leg1_inflight_.clear();
        lih_leg1_inflight_since_.clear();
        lih_rebalance_inflight_.clear();
        if (doc.contains("lih_session_legs_used")) {
            lih_session_legs_used_ = static_cast<int>(doc.at("lih_session_legs_used").as_int64());
        }
        if (doc.contains("lih_session_max_legs")) {
            lih_session_max_legs_ = static_cast<int>(doc.at("lih_session_max_legs").as_int64());
        }
        spdlog::info("Live LIH state restored | open={} closed={} session_legs={}/{}",
                     open_lih_positions_.size(), closed_lih_positions_.size(),
                     lih_session_legs_used_, lih_session_max_legs_);
        return true;
    } catch (const std::exception& e) {
        spdlog::warn("import_live_lih_state failed: {}", e.what());
        return false;
    }
}

} // namespace risk
