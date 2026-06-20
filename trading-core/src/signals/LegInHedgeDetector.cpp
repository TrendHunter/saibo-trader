#include "LegInHedgeDetector.h"
#include <spdlog/spdlog.h>
#include <fmt/format.h>
#include <algorithm>
#include <cmath>

namespace trading {

namespace {
constexpr double kLegMinUsdc = 1.0;
constexpr double kFloatTol = 1e-6;
constexpr double kStatusLogIntervalSec = 30.0;
constexpr double kBalanceReserve = 0.995;
/** YES/NO within this gap = balanced; skip one-sided hedge (paired scale/dilute only). */
constexpr double kLihBalancedGapShares = 0.5;
} // namespace

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
                                       double endgame_secs,
                                       double endgame_hold_ask,
                                       double endgame_resume_hedge_ask,
                                       double endgame_soft_cap,
                                       double endgame_step_small,
                                       double endgame_step_large,
                                       double endgame_gap_large,
                                       double endgame_override_secs,
                                       double endgame_override_cooldown)
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
      allow_over_target_(allow_over_target),
      force_balance_secs_(force_balance_secs),
      max_rebalance_shares_(max_rebalance_shares),
      flex_rebalance_(flex_rebalance),
      flex_dilute_ratio_(flex_dilute_ratio),
      leg1_trend_align_(leg1_trend_align),
      trend_lookback_sec_(trend_lookback_sec),
      leg1_trend_mode_(leg1_trend_mode),
      leg1_trend_max_price_(leg1_trend_max_price),
      endgame_secs_(endgame_secs),
      endgame_hold_ask_(endgame_hold_ask),
      endgame_resume_hedge_ask_(endgame_resume_hedge_ask),
      endgame_soft_cap_(endgame_soft_cap),
      endgame_step_small_(endgame_step_small),
      endgame_step_large_(endgame_step_large),
      endgame_gap_large_(endgame_gap_large),
      endgame_override_secs_(endgame_override_secs),
      endgame_override_cooldown_(endgame_override_cooldown) {}

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

bool LegInHedgeDetector::leg1_trend_allows(const MarketInfo& market, bool pick_yes) const {
    if (!leg1_trend_align_) return true;
    return spot_trend_favors(market, pick_yes);
}

LegInHedgeDetector::Quote LegInHedgeDetector::quote_for(const MarketInfo& market) const {
    Quote q;
    auto side_ask = [&](const std::optional<StateStore::DetectionAsk>& det) -> double {
        if (!det) return 0.0;
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
        if (q.yes > kFloatTol && q.no > kFloatTol) return q;
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
    if (store_.paper_depth_sim()) {
        fill = store_.walk_ask_fill(token_id, fill).shares;
    }
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

std::optional<LegInAction> LegInHedgeDetector::evaluate(double now_ms, risk::RiskManager& rm) {
    const double now_sec = now_ms / 1000.0;
    rm.scrub_lih_inflight_locks(now_sec);
    const double max_leg_usdc = rm.get_max_leg_cost_usdc();
    const double max_matched_cap = rm.get_lih_max_matched_shares();

    for (const auto& market : markets_) {
        if (market.window_minutes == 5 && !store_.dh_enable_5m()) continue;
        if (market.window_minutes == 15 && !store_.dh_enable_15m()) continue;
        if (!store_.dh_asset_enabled(market.window_minutes, market.asset)) continue;

        const double secs_left = market.end_date_ts - now_sec;

        const std::string key = market.asset + "-" + std::to_string(market.window_minutes);

        bool blocked = false;
        for (const auto& [id, dh] : rm.get_open_dh_positions()) {
            if (dh.asset == market.asset) {
                blocked = true;
                break;
            }
        }
            if (blocked) continue;

        auto open_lih = rm.find_open_lih_for_market(market);
        if (!open_lih) {
            // Only reuse by-asset when same gamma window (avoid hedging wrong tokens).
            auto sibling = rm.find_open_lih_by_asset(market.asset, market.window_minutes);
            if (sibling && std::abs(sibling->end_date_ts - market.end_date_ts) < 2.0) {
                open_lih = sibling;
            }
        }
        Quote q = quote_for(market);
        if (q.yes <= kFloatTol || q.no <= kFloatTol) {
            if (!open_lih) log_entry_status(market, key, now_sec, q, "no quote");
            continue;
        }

        if (!open_lih) {
            const double window_total_sec = market.window_minutes * 60.0;
            const double elapsed = window_total_sec - secs_left;
            if (leg1_start_delay_sec_ > 0.0 && elapsed < leg1_start_delay_sec_) {
                log_entry_status(market, key, now_sec, q, "early window — wait volatility");
                continue;
            }
            if (secs_left < leg1_min_seconds_remaining_) {
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
            // DEBUG single-round test: re-enable session leg cap gate (LIH_SESSION_MAX_LEGS).
            // if (rm.lih_session_leg1_blocked()) {
            //     log_entry_status(market, key, now_sec, q, "session leg cap");
            //     continue;
            // }
            bool pick_yes = false;
            const char* entry_tag = "entry";

            if (leg1_trend_mode_) {
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
            double shares = leg1_shares_;
            if (max_matched_cap > kFloatTol) {
                shares = std::min(shares, max_matched_cap);
            }
            shares = cap_shares_budget(shares, max_leg_usdc, px);
            if (store_.paper_depth_sim()) {
                const auto& tok = pick_yes ? market.yes_token_id : market.no_token_id;
                shares = store_.walk_ask_fill(tok, shares).shares;
            }
            if (shares <= kFloatTol) {
                log_entry_status(market, key, now_sec, q, "depth fill 0");
                continue;
            }
            if (shares * px + kFloatTol < kLegMinUsdc) {
                log_entry_status(market, key, now_sec, q, "below min usdc");
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

            auto try_light_hedge = [&](double max_fill, bool require_full_gap,
                                       const char* mode) -> std::optional<LegInAction> {
                const double capped_gap = std::min({gap, max_fill, remaining_matched});
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
                const bool on_trend = spot_trend_favors(market, heavy_yes);
                // Hold only when clearly winning (≥ hold ask) and on-trend; < resume ask always hedge.
                const bool hold_win = heavy_ask >= endgame_hold_ask_ - kFloatTol
                    && heavy_ask > endgame_resume_hedge_ask_ + kFloatTol
                    && on_trend;
                if (hold_win) {
                    std::string msg = fmt::format(
                        "[LIH DEBUG] endgame-hold | {} {}m | heavy {:.4f} on-trend | {:.0f}s left — skip hedge",
                        market.asset, market.window_minutes, heavy_ask, secs_left);
                    spdlog::info(msg);
                    store_.push_telemetry(msg);
                    continue;
                }

                const double step = gap >= endgame_gap_large_ - kFloatTol
                    ? endgame_step_large_ : endgame_step_small_;
                const char* mode = endgame_override ? "endgame-override" : "endgame";

                if (at_target) {
                    if (auto act = try_light_hedge(step, false, mode)) return act;
                    continue;
                }

                const bool within_soft_cap = marginal <= endgame_soft_cap_ + kFloatTol;
                if (within_soft_cap || endgame_override) {
                    if (auto act = try_light_hedge(step, false, mode)) return act;
                }
                continue;
            }

            const double budget_step = cap_shares_budget(leg1_shares_, max_leg_usdc, light_ask);

            if (at_target || force) {
                const char* mode = at_target ? "hedge" : "force";
                if (auto act = try_light_hedge(gap, force, mode)) return act;
                if (!force && budget_step > kFloatTol) {
                    if (auto act = try_light_hedge(budget_step, false, mode)) return act;
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
