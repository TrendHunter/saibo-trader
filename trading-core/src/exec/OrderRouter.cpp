#include "OrderRouter.h"
#include "../signals/LegInHedgeDetector.h"
#include <boost/json.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/error.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/version.hpp>
#include <spdlog/spdlog.h>
#include <fmt/core.h>
#include <iostream>
#include <chrono>
#include <ctime>
#include <random>
#include <cmath>
#include <algorithm>
#include <thread>

namespace trading {
namespace exec {

namespace {
constexpr double kMinLegUsdc = 1.0;
constexpr double kFloatTol = 1e-6;
constexpr double kMinFillShares = 0.01;
constexpr double kDepthFillRatio = 0.90;
constexpr double kMaxAskPriceSlack = 1.05;

/** Beijing hour of market window start (UTC+8, no DST) — matches detector. */
int window_start_bj_hour(const MarketInfo& market) {
    const double window_total_sec = std::max(60.0, market.window_minutes * 60.0);
    const time_t t = static_cast<time_t>(market.end_date_ts - window_total_sec);
    tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &t);
#else
    gmtime_r(&t, &utc);
#endif
    return (utc.tm_hour + 8) % 24;
}

/** Reject LEG1 when fresh exec ask is below offhours floor (detector may have used stale REST). */
bool offhours_exec_ask_blocked(const StateStore& store, const MarketInfo& market, double exec_px) {
    const double min_ask = store.mm2_offhours_min_ask();
    const int bj_start = store.mm2_main_bj_start();
    const int bj_end = store.mm2_main_bj_end();
    if (min_ask <= kFloatTol || bj_end <= bj_start) return false;
    const int bj_h = window_start_bj_hour(market);
    const bool in_main = bj_h >= bj_start && bj_h < bj_end;
    return !in_main && exec_px + kFloatTol < min_ask;
}

bool leg_meets_minimum(double price, double size_shares) {
    return price > 0.0 && size_shares * price >= kMinLegUsdc - kFloatTol;
}

bool bridge_fill_is_terminal_dead(const std::string& error_msg, const std::string& status) {
    if (error_msg.find("0 fill") != std::string::npos) return true;
    return status == "unmatched" || status == "cancelled" || status == "expired";
}

bool lih_action_is_force(const trading::LegInAction& act) {
    return act.note.find("force") != std::string::npos
        || act.note.find("endgame") != std::string::npos;
}

void parse_json_exec_meta(const boost::json::object& j, LegFillResult& r) {
    if (j.contains("post_order_type") && j.at("post_order_type").is_string()) {
        r.post_order_type = std::string(j.at("post_order_type").as_string());
    }
    if (j.contains("trader_side") && j.at("trader_side").is_string()) {
        r.trader_side = std::string(j.at("trader_side").as_string());
    }
    if (j.contains("exec_class") && j.at("exec_class").is_string()) {
        r.exec_class = std::string(j.at("exec_class").as_string());
    }
    if (j.contains("intent") && j.at("intent").is_string()) {
        r.intent = std::string(j.at("intent").as_string());
    }
    if (j.contains("side_ask_at_fill")) {
        const auto& v = j.at("side_ask_at_fill");
        if (v.is_double()) {
            r.side_ask_at_fill = v.as_double();
            r.has_side_ask = true;
        } else if (v.is_int64()) {
            r.side_ask_at_fill = static_cast<double>(v.as_int64());
            r.has_side_ask = true;
        }
    }
    if (j.contains("side_bid_at_fill")) {
        const auto& v = j.at("side_bid_at_fill");
        if (v.is_double()) {
            r.side_bid_at_fill = v.as_double();
            r.has_side_bid = true;
        } else if (v.is_int64()) {
            r.side_bid_at_fill = static_cast<double>(v.as_int64());
            r.has_side_bid = true;
        }
    }
    if (j.contains("price_vs_ask_cents")) {
        const auto& v = j.at("price_vs_ask_cents");
        if (v.is_double()) r.price_vs_ask_cents = v.as_double();
        else if (v.is_int64()) r.price_vs_ask_cents = static_cast<double>(v.as_int64());
    }
}

risk::LihExecMeta exec_meta_from_fill(const LegFillResult& fill) {
    risk::LihExecMeta m;
    m.post_order_type = fill.post_order_type.empty() ? "FAK" : fill.post_order_type;
    m.trader_side = fill.trader_side;
    m.exec_class = fill.exec_class;
    m.intent = fill.intent;
    m.order_id = fill.order_id;
    m.side_ask_at_fill = fill.side_ask_at_fill;
    m.side_bid_at_fill = fill.side_bid_at_fill;
    m.price_vs_ask_cents = fill.price_vs_ask_cents;
    m.has_side_ask = fill.has_side_ask;
    m.has_side_bid = fill.has_side_bid;
    return m;
}

void infer_shadow_exec_meta(
    risk::LihExecMeta& m,
    double fill_px,
    double side_ask,
    double side_bid,
    const std::string& post_type,
    const std::string& intent,
    const std::string& order_id = "") {
    m.post_order_type = post_type;
    m.intent = intent;
    m.order_id = order_id;
    if (side_ask > kFloatTol) {
        m.side_ask_at_fill = side_ask;
        m.price_vs_ask_cents = (fill_px - side_ask) * 100.0;
        m.has_side_ask = true;
    }
    if (side_bid > kFloatTol) {
        m.side_bid_at_fill = side_bid;
        m.has_side_bid = true;
    }
    constexpr double tick = 0.01;
    if (m.has_side_ask) {
        if (fill_px < side_ask - tick) m.exec_class = "limit_below_ask";
        else if (m.has_side_bid && fill_px <= side_bid + tick) m.exec_class = "limit_at_bid";
        else if (fill_px <= side_ask + tick) m.exec_class = "taker_at_ask";
        else m.exec_class = "taker_above_ask";
    } else {
        m.exec_class = "no_book";
    }
    if (post_type == "FAK") m.trader_side = "TAKER";
    else if (m.exec_class == "limit_below_ask" || m.exec_class == "limit_at_bid") m.trader_side = "MAKER";
    else if (m.exec_class == "taker_at_ask" || m.exec_class == "taker_above_ask") m.trader_side = "TAKER";
}

std::string lih_intent_for(const trading::LegInAction& act) {
    switch (act.kind) {
    case trading::LegInAction::Kind::OpenLeg1:
    case trading::LegInAction::Kind::AddLeg1:
        return "leg1_clip";
    case trading::LegInAction::Kind::CompleteHedge:
        return "hedge";
    case trading::LegInAction::Kind::UnwindLeg1:
        return "unwind";
    }
    return "";
}
} // namespace

OrderRouter::OrderRouter(boost::asio::io_context& ioc, 
                        boost::asio::ssl::context& ctx,
                        trading::StateStore& store,
                        risk::RiskManager& risk_manager,
                        const std::string& clob_api_url, 
                        const std::string& chain_id_str,
                        const std::string& verifying_contract,
                        const std::string& private_key_hex,
                        const std::string& signer_address,
                        const std::string& funder_address,
                        bool paper_mode,
                        const std::string& api_key,
                        const std::string& api_secret,
                        const std::string& api_passphrase,
                        const std::string& neg_risk_exchange,
                        bool live_lih_dry_run,
                        bool use_python_clob,
                        const std::string& clob_bridge_host,
                        int clob_bridge_port,
                        const std::string& clob_bridge_path)
    : ioc_(ioc), ctx_(ctx), store_(store), risk_manager_(risk_manager),
      clob_api_url_(clob_api_url), signer_address_(signer_address), funder_address_(funder_address), 
      paper_mode_(paper_mode),
      live_lih_dry_run_(live_lih_dry_run && !paper_mode),
      api_key_(api_key), api_secret_(api_secret), api_passphrase_(api_passphrase),
      neg_risk_exchange_(neg_risk_exchange),
      use_python_clob_(use_python_clob && !paper_mode),
      clob_bridge_host_(clob_bridge_host),
      clob_bridge_port_(clob_bridge_port),
      clob_bridge_path_(clob_bridge_path) {
    
    if (!paper_mode_ && api_key_.empty()) {
        spdlog::critical("[FATAL] Live trading enabled but POLY_API_KEY is missing! Run derive_and_update_keys.py first.");
        throw std::runtime_error("Missing API credentials for live trading");
    }
    if (live_lih_dry_run_) {
        spdlog::info("[LIVE LIH] Dry-run ON — REST book validation only, no CLOB orders will be sent");
    }
    if (use_python_clob_) {
        spdlog::info("[LIVE EXEC] Python CLOB bridge ON — orders via {}:{}{}",
                     clob_bridge_host_, clob_bridge_port_, clob_bridge_path_);
    }
    
    signer_ = std::make_unique<EIP712Signer>(std::stoull(chain_id_str), verifying_contract, private_key_hex);
    if (!neg_risk_exchange_.empty()) {
        signer_neg_risk_ = std::make_unique<EIP712Signer>(std::stoull(chain_id_str), neg_risk_exchange_, private_key_hex);
    }
}

OrderRouter::~OrderRouter() {}

Order OrderRouter::build_order(const std::string& token_id, double price, double size_shares, uint8_t side) const {
    Order order;
    order.salt = generate_salt();
    order.maker = funder_address_;
    order.signer = signer_address_;
    order.taker = "0x0000000000000000000000000000000000000000";
    order.tokenId = token_id;

    uint64_t scale = 1000000;
    if (side == 0) {
        order.makerAmount = std::to_string(static_cast<uint64_t>(size_shares * price * scale));
        order.takerAmount = std::to_string(static_cast<uint64_t>(size_shares * scale));
    } else {
        order.makerAmount = std::to_string(static_cast<uint64_t>(size_shares * scale));
        order.takerAmount = std::to_string(static_cast<uint64_t>(size_shares * price * scale));
    }

    auto now = std::chrono::system_clock::now();
    order.expiration = "0";
    order.side = side;
    order.signatureType = (funder_address_ == signer_address_ ? 0 : 1);
    order.timestamp = std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
    order.metadata = "0x0000000000000000000000000000000000000000000000000000000000000000";
    order.builder = "0x0000000000000000000000000000000000000000000000000000000000000000";
    return order;
}

bool OrderRouter::submit_order(const std::string& token_id, double price, double size, uint8_t side, bool is_neg_risk) {
    try {
        Order order = build_order(token_id, price, size, side);
        Signature sig = pick_signer(is_neg_risk).sign_order(order);
        if (paper_mode_) {
            return simulate_paper_order(order, sig, "", "", 0.0, "MANUAL", "", is_neg_risk);
        }
        LegFillResult fill = execute_rest_order(order, sig, is_neg_risk, true, "", "", 0.0, "MANUAL", "");
        return fill.success;
    } catch (const std::exception& e) {
        spdlog::error("Order signature failed: {}", e.what());
        return false;
    }
}

double OrderRouter::query_ask_depth_shares(const std::string& token_id, double price) {
    if (price <= 0.0) return -1.0;
    auto book = fetch_book_object(token_id);
    if (!book) return -1.0;
    if (!book->contains("asks") || !book->at("asks").is_array()) return -1.0;

    const double max_price = price * kMaxAskPriceSlack;
    double shares_available = 0.0;
    for (const auto& level_v : book->at("asks").as_array()) {
        if (!level_v.is_object()) continue;
        const auto& level = level_v.as_object();
        if (!level.contains("price") || !level.contains("size")) continue;
        double p = std::stod(std::string(level.at("price").as_string()));
        double s = std::stod(std::string(level.at("size").as_string()));
        if (p <= max_price) {
            shares_available += s;
        }
    }
    return shares_available;
}

std::optional<boost::json::object> OrderRouter::fetch_book_object(const std::string& token_id) {
    namespace beast = boost::beast;
    namespace http = beast::http;

    std::string host = "clob.polymarket.com";
    std::string target = "/book?token_id=" + token_id;

    try {
        std::lock_guard<std::mutex> lock(http_mutex_);

        boost::asio::ip::tcp::resolver resolver(ioc_);
        beast::ssl_stream<beast::tcp_stream> stream(ioc_, ctx_);
        if (!SSL_set_tlsext_host_name(stream.native_handle(), host.c_str())) return std::nullopt;
        auto const results = resolver.resolve(host, "443");
        beast::get_lowest_layer(stream).connect(results);
        stream.handshake(boost::asio::ssl::stream_base::client);

        http::request<http::string_body> req{http::verb::get, target, 11};
        req.set(http::field::host, host);
        req.set(http::field::user_agent, "PolymarketBot/1.0");

        http::write(stream, req);
        beast::flat_buffer buffer;
        http::response<http::string_body> res;
        http::read(stream, buffer, res);
        beast::error_code ec;
        stream.shutdown(ec);

        if (res.result() != http::status::ok) return std::nullopt;

        auto jv = boost::json::parse(res.body());
        if (!jv.is_object()) return std::nullopt;
        return jv.as_object();
    } catch (const std::exception& e) {
        spdlog::warn("fetch_book_object failed for {}: {}", token_id.substr(0, 12), e.what());
        return std::nullopt;
    }
}

BookAskInfo OrderRouter::parse_book_asks(const boost::json::object& book) const {
    BookAskInfo info;
    if (!book.contains("asks") || !book.at("asks").is_array()) return info;
    const auto& asks = book.at("asks").as_array();
    if (asks.empty()) return info;

    double best = 1.0;
    for (const auto& level_v : asks) {
        if (!level_v.is_object()) continue;
        const auto& level = level_v.as_object();
        if (!level.contains("price")) continue;
        double p = std::stod(std::string(level.at("price").as_string()));
        if (p > 0.0 && p < best) best = p;
    }
    if (best >= 1.0) return info;

    info.best_ask = best;
    const double max_price = best * kMaxAskPriceSlack;
    for (const auto& level_v : asks) {
        if (!level_v.is_object()) continue;
        const auto& level = level_v.as_object();
        if (!level.contains("price") || !level.contains("size")) continue;
        double p = std::stod(std::string(level.at("price").as_string()));
        double s = std::stod(std::string(level.at("size").as_string()));
        if (p <= max_price) {
            info.depth_shares += s;
        }
    }
    if (book.contains("min_order_size")) {
        try {
            const auto& mv = book.at("min_order_size");
            if (mv.is_string()) {
                info.min_order_size = std::stod(std::string(mv.as_string()));
            } else if (mv.is_double()) {
                info.min_order_size = mv.as_double();
            }
        } catch (...) {}
    }
    if (book.contains("tick_size")) {
        try {
            const auto& tv = book.at("tick_size");
            if (tv.is_string()) {
                info.tick_size = std::stod(std::string(tv.as_string()));
            } else if (tv.is_double()) {
                info.tick_size = tv.as_double();
            }
        } catch (...) {}
    }
    if (info.min_order_size <= kFloatTol) info.min_order_size = 5.0;
    if (info.tick_size <= kFloatTol) info.tick_size = 0.01;
    info.ok = true;
    return info;
}

std::vector<trading::StateStore::BookLevel> OrderRouter::parse_ask_ladder(const boost::json::object& book) const {
    std::vector<trading::StateStore::BookLevel> ladder;
    if (!book.contains("asks") || !book.at("asks").is_array()) return ladder;
    for (const auto& level_v : book.at("asks").as_array()) {
        if (!level_v.is_object()) continue;
        const auto& level = level_v.as_object();
        if (!level.contains("price") || !level.contains("size")) continue;
        double p = std::stod(std::string(level.at("price").as_string()));
        double s = std::stod(std::string(level.at("size").as_string()));
        if (p > 0.0 && s > 0.0) {
            ladder.push_back({p, s});
        }
    }
    std::sort(ladder.begin(), ladder.end(),
              [](const trading::StateStore::BookLevel& a, const trading::StateStore::BookLevel& b) {
                  return a.price < b.price;
              });
    return ladder;
}

BookBidInfo OrderRouter::parse_book_bids(const boost::json::object& book) const {
    BookBidInfo info;
    if (!book.contains("bids") || !book.at("bids").is_array()) return info;
    const auto& bids = book.at("bids").as_array();
    if (bids.empty()) return info;

    double best = 0.0;
    for (const auto& level_v : bids) {
        if (!level_v.is_object()) continue;
        const auto& level = level_v.as_object();
        if (!level.contains("price")) continue;
        double p = std::stod(std::string(level.at("price").as_string()));
        if (p > best) best = p;
    }
    if (best <= 0.0) return info;
    info.best_bid = best;
    info.ok = true;
    return info;
}

BookAskInfo OrderRouter::fetch_book_ask_info(const std::string& token_id) {
    auto book = fetch_book_object(token_id);
    if (!book) return {};
    return parse_book_asks(*book);
}

BookBidInfo OrderRouter::fetch_book_bid_info(const std::string& token_id) {
    auto book = fetch_book_object(token_id);
    if (!book) return {};
    return parse_book_bids(*book);
}

void OrderRouter::refresh_rest_book(const std::vector<std::string>& token_ids) {
    for (const auto& token_id : token_ids) {
        auto book = fetch_book_object(token_id);
        if (!book) continue;
        auto ask = parse_book_asks(*book);
        if (ask.ok) {
            store_.update_rest_book_ask(token_id, ask.best_ask, ask.depth_shares);
            store_.update_rest_ask_ladder(token_id, parse_ask_ladder(*book));
        }
        auto bid = parse_book_bids(*book);
        if (bid.ok) {
            store_.update_rest_book_bid(token_id, bid.best_bid);
        }
    }
}

void OrderRouter::refresh_rest_book_asks(const std::vector<std::string>& token_ids) {
    refresh_rest_book(token_ids);
}

bool OrderRouter::check_book_depth(const std::string& token_id, double price, double size_shares) {
    if (size_shares <= 0.0) return false;
    double available = query_ask_depth_shares(token_id, price);
    if (available < 0.0) return false;
    return available >= size_shares * kDepthFillRatio;
}

LegFillResult OrderRouter::execute_leg_buy(
    const std::string& token_id, double price, double size_shares, bool is_neg_risk,
    const std::string& order_type,
    const std::string& intent,
    double side_ask, double side_bid,
    bool has_side_ask, bool has_side_bid) {
    try {
        if (!paper_mode_ && use_python_clob_) {
            return execute_via_clob_bridge(
                token_id, price, size_shares, 0, is_neg_risk, false,
                "", "", 0.0, "MANUAL", "", "",
                order_type, intent, side_ask, side_bid, has_side_ask, has_side_bid);
        }
        Order order = build_order(token_id, price, size_shares, 0);
        Signature sig = pick_signer(is_neg_risk).sign_order(order);
        if (paper_mode_) {
            LegFillResult r;
            r.success = simulate_paper_order(order, sig, "", "", 0.0, "MANUAL", "", is_neg_risk);
            if (r.success) {
                r.price = price;
                r.size_shares = size_shares;
                r.post_order_type = order_type;
                r.intent = intent;
                risk::LihExecMeta tmp;
                infer_shadow_exec_meta(tmp, price, side_ask, side_bid, order_type, intent);
                r.trader_side = tmp.trader_side;
                r.exec_class = tmp.exec_class;
                r.side_ask_at_fill = tmp.side_ask_at_fill;
                r.side_bid_at_fill = tmp.side_bid_at_fill;
                r.price_vs_ask_cents = tmp.price_vs_ask_cents;
                r.has_side_ask = tmp.has_side_ask;
                r.has_side_bid = tmp.has_side_bid;
            }
            return r;
        }
        return execute_rest_order(order, sig, is_neg_risk, false);
    } catch (const std::exception& e) {
        spdlog::error("Leg buy failed: {}", e.what());
        return {};
    }
}

LegFillResult OrderRouter::execute_unwind_sell(const std::string& token_id, double price, double size_shares, bool is_neg_risk) {
    try {
        Order order = build_order(token_id, price, size_shares, 1);
        Signature sig = pick_signer(is_neg_risk).sign_order(order);
        if (paper_mode_) {
            LegFillResult r;
            r.success = true;
            r.price = price;
            r.size_shares = size_shares;
            return r;
        }
        return execute_rest_order(order, sig, is_neg_risk, false);
    } catch (const std::exception& e) {
        spdlog::error("Unwind sell failed: {}", e.what());
        return {};
    }
}

void OrderRouter::submit_close_order(const std::string& order_id, const std::string& token_id, double current_price, double size, const std::string& asset, const std::string& question, double end_date_ts, const std::string& strategy, bool is_neg_risk) {
    try {
        Order order = build_order(token_id, current_price, size, 1);
        Signature sig = pick_signer(is_neg_risk).sign_order(order);
        if (paper_mode_) {
            simulate_paper_order(order, sig, asset, question, end_date_ts, strategy, order_id, is_neg_risk);
        } else {
            execute_rest_order(order, sig, is_neg_risk, true, asset, question, end_date_ts, strategy, order_id);
        }
    } catch (const std::exception& e) {
        spdlog::error("Close order signature failed: {}", e.what());
    }
}

bool OrderRouter::simulate_paper_order(const Order& order, const Signature& sig, const std::string& asset, const std::string& question, double end_date_ts, const std::string& strategy, const std::string& original_order_id, bool is_neg_risk, const std::string& direction) {
    (void)sig;
    (void)is_neg_risk;
    if (order.side == 0) {
        double price = std::stod(order.makerAmount) / std::stod(order.takerAmount);
        double size_shares = std::stod(order.takerAmount) / 1000000.0;
        double cost = price * size_shares;

        risk::Position pos;
        pos.order_id = "paper_" + order.salt;
        pos.token_id = order.tokenId;
        pos.market_question = question;
        pos.side = "BUY";
        pos.entry_price = price;
        pos.size_shares = size_shares;
        pos.cost_usdc = cost;
        pos.opened_at = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
        pos.end_date_ts = end_date_ts;
        pos.asset = asset;
        pos.strategy = strategy;
        pos.paper_mode = true;
        pos.is_neg_risk = is_neg_risk;
        pos.direction = direction;

        risk_manager_.register_trade_open(pos);
        spdlog::info("[PAPER TRADE] FILLED | {} | {} | Strategy: {} | Price: {:.4f} | Size: {:.2f} | Cost: ${:.2f}",
                     asset, question, strategy, price, size_shares, cost);
    } else {
        double price = std::stod(order.takerAmount) / std::stod(order.makerAmount);
        risk_manager_.register_trade_close(original_order_id, price);
        spdlog::info("[PAPER TRADE] CLOSED | {} | {} | Strategy: {} | Exit Price: {:.4f}",
                     asset, question, strategy, price);
    }
    return true;
}

LegFillResult OrderRouter::execute_via_clob_bridge(
    const std::string& token_id,
    double price,
    double size_shares,
    uint8_t side,
    bool is_neg_risk,
    bool register_position,
    const std::string& asset,
    const std::string& question,
    double end_date_ts,
    const std::string& strategy,
    const std::string& original_order_id,
    const std::string& position_id_salt,
    const std::string& order_type,
    const std::string& intent,
    double side_ask,
    double side_bid,
    bool has_side_ask,
    bool has_side_bid
) {
    namespace beast = boost::beast;
    namespace http = beast::http;

    LegFillResult result;
    boost::json::object body;
    body["token_id"] = token_id;
    body["price"] = price;
    body["size_shares"] = size_shares;
    body["side"] = side == 0 ? "BUY" : "SELL";
    body["neg_risk"] = is_neg_risk;
    body["order_type"] = order_type.empty() ? "FAK" : order_type;
    if (!intent.empty()) body["intent"] = intent;
    if (has_side_ask) body["side_ask"] = side_ask;
    if (has_side_bid) body["side_bid"] = side_bid;
    const std::string payload = boost::json::serialize(body);

    try {
        std::lock_guard<std::mutex> lock(http_mutex_);

        boost::asio::ip::tcp::resolver resolver(ioc_);
        beast::tcp_stream stream(ioc_);
        auto const results = resolver.resolve(clob_bridge_host_, std::to_string(clob_bridge_port_));
        beast::get_lowest_layer(stream).connect(results);

        http::request<http::string_body> req{http::verb::post, clob_bridge_path_, 11};
        req.set(http::field::host, clob_bridge_host_);
        req.set(http::field::user_agent, "PolymarketBot/1.0");
        req.set(http::field::content_type, "application/json");
        req.body() = payload;
        req.prepare_payload();

        http::write(stream, req);

        beast::flat_buffer buffer;
        http::response<http::string_body> res;
        http::read(stream, buffer, res);

        beast::error_code ec;
        stream.socket().shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);

        if (res.result() != http::status::ok) {
            spdlog::error("[LIVE EXEC] Bridge REJECTED: {} | Body: {}", res.result_int(), res.body());
            // Still try to parse body for order_id / fill hints.
            try {
                auto response_json = boost::json::parse(res.body()).as_object();
                if (response_json.contains("order_id") && response_json.at("order_id").is_string()) {
                    result.order_id = std::string(response_json.at("order_id").as_string());
                }
                if (response_json.contains("size_shares")) {
                    const auto& sv = response_json.at("size_shares");
                    if (sv.is_double()) result.size_shares = sv.as_double();
                    else if (sv.is_int64()) result.size_shares = static_cast<double>(sv.as_int64());
                }
                if (response_json.contains("price")) {
                    const auto& pv = response_json.at("price");
                    if (pv.is_double()) result.price = pv.as_double();
                    else if (pv.is_int64()) result.price = static_cast<double>(pv.as_int64());
                }
                if (response_json.contains("success") && response_json.at("success").as_bool()
                    && result.size_shares > 0.0) {
                    result.success = true;
                    result.price = result.price > 0.0 ? result.price : price;
                    parse_json_exec_meta(response_json, result);
                    if (!register_position) {
                        spdlog::info("[LIVE EXEC] Bridge fill | {} | {:.4f} x {:.4f}",
                                     asset, result.price, result.size_shares);
                    }
                    return result;
                }
                if (!result.order_id.empty()) {
                    result.pending_fill = true;
                }
            } catch (...) {}
            return result;
        }

        auto response_json = boost::json::parse(res.body()).as_object();
        std::string order_id;
        if (response_json.contains("order_id") && response_json.at("order_id").is_string()) {
            order_id = std::string(response_json.at("order_id").as_string());
        }
        const bool success = response_json.contains("success") && response_json.at("success").as_bool();
        std::string error_msg;
        if (response_json.contains("error") && response_json.at("error").is_string()) {
            error_msg = std::string(response_json.at("error").as_string());
        }

        double actual_price = price;
        double filled_size = 0.0;
        if (response_json.contains("price")) {
            const auto& pv = response_json.at("price");
            if (pv.is_double()) actual_price = pv.as_double();
            else if (pv.is_int64()) actual_price = static_cast<double>(pv.as_int64());
        }
        if (response_json.contains("size_shares")) {
            const auto& sv = response_json.at("size_shares");
            if (sv.is_double()) filled_size = sv.as_double();
            else if (sv.is_int64()) filled_size = static_cast<double>(sv.as_int64());
        }
        result.order_id = order_id;

        if (!success) {
            std::string status;
            if (response_json.contains("status") && response_json.at("status").is_string()) {
                status = std::string(response_json.at("status").as_string());
            }
            if (!order_id.empty()) {
                if (bridge_fill_is_terminal_dead(error_msg, status)) {
                    spdlog::warn("[LIVE EXEC] Bridge dead order {} | order_id={} | err={} status={}",
                                 asset, order_id,
                                 error_msg.empty() ? "success=false" : error_msg, status);
                } else {
                    result.pending_fill = true;
                    spdlog::warn("[LIVE EXEC] Bridge uncertain fill {} | order_id={} | err={}",
                                 asset, order_id, error_msg.empty() ? "success=false" : error_msg);
                }
            } else {
                spdlog::error("[LIVE EXEC] Bridge order failed: {}",
                              error_msg.empty() ? "success=false" : error_msg);
            }
            return result;
        }

        if (filled_size <= 0.0) {
            if (!order_id.empty()) {
                result.pending_fill = true;
                spdlog::warn("[LIVE EXEC] Bridge 0 fill but order_id={} for {} — pending",
                             order_id, asset);
            } else {
                spdlog::warn("[LIVE EXEC] Bridge returned 0 fill for {}", asset);
            }
            return result;
        }

        result.success = true;
        result.price = actual_price;
        result.size_shares = filled_size;
        result.pending_fill = false;
        parse_json_exec_meta(response_json, result);

        if (!register_position) {
            spdlog::info("[LIVE EXEC] Bridge fill | {} | {:.4f} x {:.4f}", asset, actual_price, filled_size);
            return result;
        }

        if (side == 0) {
            risk::Position pos;
            pos.order_id = "live_" + position_id_salt;
            pos.token_id = token_id;
            pos.market_question = question;
            pos.side = "BUY";
            pos.entry_price = actual_price;
            pos.size_shares = filled_size;
            pos.cost_usdc = actual_price * filled_size;
            pos.opened_at = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
            pos.end_date_ts = end_date_ts;
            pos.asset = asset;
            pos.strategy = strategy;
            pos.paper_mode = false;
            pos.is_neg_risk = is_neg_risk;

            risk_manager_.register_trade_open(pos);
            spdlog::info("[LIVE EXEC] BUY FILLED | {} | {:.4f} x {:.2f}", asset, actual_price, filled_size);
        } else {
            risk_manager_.register_trade_close(original_order_id, actual_price);
            spdlog::info("[LIVE EXEC] SELL FILLED | {} | {:.4f}", asset, actual_price);
        }
        return result;
    } catch (const std::exception& e) {
        spdlog::error("[LIVE EXEC] Bridge network error: {}", e.what());
        return result;
    }
}

LegFillResult OrderRouter::resolve_clob_fill(
    const std::string& token_id,
    double fallback_price,
    const std::string& order_id,
    uint8_t side) {
    namespace beast = boost::beast;
    namespace http = beast::http;

    LegFillResult result;
    boost::json::object body;
    body["token_id"] = token_id;
    body["price"] = fallback_price;
    body["side"] = side == 0 ? "BUY" : "SELL";
    if (!order_id.empty()) body["order_id"] = order_id;
    const std::string payload = boost::json::serialize(body);

    try {
        std::lock_guard<std::mutex> lock(http_mutex_);
        boost::asio::ip::tcp::resolver resolver(ioc_);
        beast::tcp_stream stream(ioc_);
        auto const results = resolver.resolve(clob_bridge_host_, std::to_string(clob_bridge_port_));
        beast::get_lowest_layer(stream).connect(results);

        http::request<http::string_body> req{http::verb::post, "/internal/clob/resolve", 11};
        req.set(http::field::host, clob_bridge_host_);
        req.set(http::field::user_agent, "PolymarketBot/1.0");
        req.set(http::field::content_type, "application/json");
        req.body() = payload;
        req.prepare_payload();

        http::write(stream, req);
        beast::flat_buffer buffer;
        http::response<http::string_body> res;
        http::read(stream, buffer, res);
        beast::error_code ec;
        stream.socket().shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);

        if (res.result() != http::status::ok) return result;
        auto response_json = boost::json::parse(res.body()).as_object();
        result.success = response_json.contains("success") && response_json.at("success").as_bool();
        if (response_json.contains("order_id") && response_json.at("order_id").is_string()) {
            result.order_id = std::string(response_json.at("order_id").as_string());
        }
        if (response_json.contains("price")) {
            const auto& pv = response_json.at("price");
            if (pv.is_double()) result.price = pv.as_double();
        }
        if (response_json.contains("size_shares")) {
            const auto& sv = response_json.at("size_shares");
            if (sv.is_double()) result.size_shares = sv.as_double();
            else if (sv.is_int64()) result.size_shares = static_cast<double>(sv.as_int64());
        }
        if (result.success && result.size_shares > 0.0) {
            result.pending_fill = false;
            spdlog::info("[LIVE EXEC] Resolve fill | {:.4f} x {:.4f} order_id={}",
                         result.price, result.size_shares, result.order_id);
        }
    } catch (const std::exception& e) {
        spdlog::warn("[LIVE EXEC] Resolve bridge error: {}", e.what());
    }
    return result;
}

LegFillResult OrderRouter::execute_rest_order(
    const Order& order,
    const Signature& sig,
    bool is_neg_risk,
    bool register_position,
    const std::string& asset,
    const std::string& question,
    double end_date_ts,
    const std::string& strategy,
    const std::string& original_order_id
) {
    namespace beast = boost::beast;
    namespace http = beast::http;

    LegFillResult result;

    double target_price = 0.0;
    double size_shares = 0.0;
    if (order.side == 0) {
        target_price = std::stod(order.makerAmount) / std::stod(order.takerAmount);
        size_shares = std::stod(order.takerAmount) / 1000000.0;
    } else {
        target_price = std::stod(order.takerAmount) / std::stod(order.makerAmount);
        size_shares = std::stod(order.makerAmount) / 1000000.0;
    }

    if (use_python_clob_ && !paper_mode_) {
        return execute_via_clob_bridge(
            order.tokenId, target_price, size_shares, order.side, is_neg_risk,
            register_position, asset, question, end_date_ts, strategy,
            original_order_id, order.salt);
    }

    boost::json::object root;
    boost::json::object ord;
    ord["salt"] = std::stoull(order.salt);
    ord["maker"] = order.maker;
    ord["signer"] = order.signer;
    ord["tokenId"] = order.tokenId;
    ord["makerAmount"] = order.makerAmount;
    ord["takerAmount"] = order.takerAmount;
    ord["expiration"] = order.expiration;
    ord["side"] = order.side == 0 ? "BUY" : "SELL";
    ord["timestamp"] = order.timestamp;
    ord["metadata"] = "";
    ord["builder"] = order.builder;
    ord["signatureType"] = static_cast<std::int64_t>(order.signatureType);
    ord["signature"] = sig.rsv_hex;
    root["order"] = std::move(ord);
    root["owner"] = api_key_;
    root["orderType"] = "FAK";
    root["postOnly"] = false;

    std::string payload = boost::json::serialize(root);

    try {
        std::lock_guard<std::mutex> lock(http_mutex_);

        std::string host = "clob.polymarket.com";
        std::string target = "/order";

        boost::asio::ip::tcp::resolver resolver(ioc_);
        beast::ssl_stream<beast::tcp_stream> stream(ioc_, ctx_);

        if (!SSL_set_tlsext_host_name(stream.native_handle(), host.c_str())) {
            throw std::runtime_error("Failed to set SNI hostname");
        }

        auto const results = resolver.resolve(host, "443");
        beast::get_lowest_layer(stream).connect(results);
        stream.handshake(boost::asio::ssl::stream_base::client);

        http::request<http::string_body> req{http::verb::post, target, 11};
        req.set(http::field::host, host);
        req.set(http::field::user_agent, "PolymarketBot/1.0");
        req.set(http::field::content_type, "application/json");
        if (!api_key_.empty()) {
            std::string timestamp = std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
            std::string signature = compute_hmac_signature(timestamp, "POST", target, payload);

            req.set("POLY_API_KEY", api_key_);
            req.set("POLY_PASSPHRASE", api_passphrase_);
            req.set("POLY_TIMESTAMP", timestamp);
            req.set("POLY_SIGNATURE", signature);
            req.set("POLY_ADDRESS", signer_address_);
        }
        req.body() = payload;
        req.prepare_payload();

        http::write(stream, req);

        beast::flat_buffer buffer;
        http::response<http::string_body> res;
        http::read(stream, buffer, res);

        beast::error_code ec;
        stream.shutdown(ec);

        if (res.result() != http::status::ok && res.result() != http::status::created) {
            spdlog::error("[LIVE EXEC] Order REJECTED: {} | Body: {}", res.result_int(), res.body());
            return result;
        }

        auto response_json = boost::json::parse(res.body()).as_object();
        spdlog::info("[LIVE EXEC] Response: {}", res.body());

        const bool success = !response_json.contains("success") || response_json.at("success").as_bool();
        std::string error_msg;
        if (response_json.contains("errorMsg") && response_json.at("errorMsg").is_string()) {
            error_msg = std::string(response_json.at("errorMsg").as_string());
        }
        if (!success || !error_msg.empty()) {
            spdlog::error("[LIVE EXEC] Order rejected by CLOB: {}", error_msg.empty() ? "success=false" : error_msg);
            return result;
        }

        std::string status;
        if (response_json.contains("status") && response_json.at("status").is_string()) {
            status = std::string(response_json.at("status").as_string());
        }
        if (status == "unmatched") {
            spdlog::warn("[LIVE EXEC] FAK unmatched — no liquidity");
            return result;
        }

        double actual_price = target_price;
        double filled_size = 0.0;

        if (response_json.contains("price")) {
            actual_price = std::stod(std::string(response_json["price"].as_string()));
        }
        if (response_json.contains("size_matched")) {
            filled_size = parse_matched_size(response_json.at("size_matched"));
        } else if (response_json.contains("sizeMatched")) {
            filled_size = parse_matched_size(response_json.at("sizeMatched"));
        } else if (status == "matched" || status == "filled") {
            filled_size = size_shares;
        }

        const std::string order_id = extract_order_id(response_json);
        if (!order_id.empty()) {
            auto polled = poll_order_fill(order_id, actual_price, size_shares);
            if (polled.ok) {
                if (polled.price > 0.0) actual_price = polled.price;
                if (polled.size_shares > 0.0) filled_size = polled.size_shares;
                if (polled.status == "unmatched" || polled.status == "cancelled") {
                    spdlog::warn("[LIVE EXEC] Order {} terminal status={} after poll", order_id.substr(0, 16), polled.status);
                    return result;
                }
            }
        }

        if (filled_size <= 0) {
            spdlog::warn("[LIVE EXEC] 0 size matched after poll");
            return result;
        }

        result.success = true;
        result.price = actual_price;
        result.size_shares = filled_size;

        if (!register_position) {
            return result;
        }

        if (order.side == 0) {
            risk::Position pos;
            pos.order_id = "live_" + order.salt;
            pos.token_id = order.tokenId;
            pos.market_question = question;
            pos.side = "BUY";
            pos.entry_price = actual_price;
            pos.size_shares = filled_size;
            pos.cost_usdc = actual_price * filled_size;
            pos.opened_at = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
            pos.end_date_ts = end_date_ts;
            pos.asset = asset;
            pos.strategy = strategy;
            pos.paper_mode = false;
            pos.is_neg_risk = is_neg_risk;

            risk_manager_.register_trade_open(pos);
            spdlog::info("[LIVE EXEC] BUY FILLED | {} | {:.4f} x {:.2f}", asset, actual_price, filled_size);
        } else {
            risk_manager_.register_trade_close(original_order_id, actual_price);
            spdlog::info("[LIVE EXEC] SELL FILLED | {} | {:.4f}", asset, actual_price);
        }
        return result;
    } catch (const std::exception& e) {
        spdlog::error("[LIVE EXEC] Network error: {}", e.what());
        return result;
    }
}

double OrderRouter::parse_matched_size(const boost::json::value& raw) const {
    double sm = 0.0;
    if (raw.is_string()) sm = std::stod(std::string(raw.as_string()));
    else if (raw.is_double()) sm = raw.as_double();
    else if (raw.is_int64()) sm = static_cast<double>(raw.as_int64());
    else if (raw.is_uint64()) sm = static_cast<double>(raw.as_uint64());
    if (sm > 1000.0) sm /= 1000000.0;
    return sm;
}

std::string OrderRouter::extract_order_id(const boost::json::object& obj) const {
    auto pick = [&](const char* key) -> std::string {
        if (!obj.contains(key)) return "";
        const auto& v = obj.at(key);
        if (v.is_string()) return std::string(v.as_string());
        return "";
    };
    std::string id = pick("orderID");
    if (id.empty()) id = pick("orderId");
    if (id.empty()) id = pick("id");
    return id;
}

std::string OrderRouter::authenticated_http_get(const std::string& target) {
    namespace beast = boost::beast;
    namespace http = beast::http;

    std::lock_guard<std::mutex> lock(http_mutex_);

    const std::string host = "clob.polymarket.com";
    boost::asio::ip::tcp::resolver resolver(ioc_);
    beast::ssl_stream<beast::tcp_stream> stream(ioc_, ctx_);

    if (!SSL_set_tlsext_host_name(stream.native_handle(), host.c_str())) {
        throw std::runtime_error("Failed to set SNI hostname");
    }

    auto const results = resolver.resolve(host, "443");
    beast::get_lowest_layer(stream).connect(results);
    stream.handshake(boost::asio::ssl::stream_base::client);

    http::request<http::string_body> req{http::verb::get, target, 11};
    req.set(http::field::host, host);
    req.set(http::field::user_agent, "PolymarketBot/1.0");
    if (!api_key_.empty()) {
        const std::string timestamp = std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        const std::string signature = compute_hmac_signature(timestamp, "GET", target, "");
        req.set("POLY_API_KEY", api_key_);
        req.set("POLY_PASSPHRASE", api_passphrase_);
        req.set("POLY_TIMESTAMP", timestamp);
        req.set("POLY_SIGNATURE", signature);
        req.set("POLY_ADDRESS", signer_address_);
    }

    http::write(stream, req);
    beast::flat_buffer buffer;
    http::response<http::string_body> res;
    http::read(stream, buffer, res);
    beast::error_code ec;
    stream.shutdown(ec);

    if (res.result() != http::status::ok) {
        throw std::runtime_error("GET " + target + " HTTP " + std::to_string(res.result_int()));
    }
    return res.body();
}

OrderRouter::PolledFill OrderRouter::poll_order_fill(
    const std::string& order_id, double fallback_price, double requested_shares)
{
    PolledFill out;
    out.price = fallback_price;

    constexpr int kMaxAttempts = 5;
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        if (attempt > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
        try {
            const std::string body = authenticated_http_get("/order/" + order_id);
            const auto obj = boost::json::parse(body).as_object();

            if (obj.contains("status") && obj.at("status").is_string()) {
                out.status = std::string(obj.at("status").as_string());
            }
            if (obj.contains("price")) {
                const auto& pv = obj.at("price");
                if (pv.is_string()) out.price = std::stod(std::string(pv.as_string()));
                else if (pv.is_double()) out.price = pv.as_double();
            }
            if (obj.contains("size_matched")) {
                out.size_shares = parse_matched_size(obj.at("size_matched"));
            } else if (obj.contains("sizeMatched")) {
                out.size_shares = parse_matched_size(obj.at("sizeMatched"));
            } else if (out.status == "matched" || out.status == "filled") {
                out.size_shares = requested_shares;
            }

            out.ok = true;
            if (out.size_shares > 0.0) {
                spdlog::debug("[LIVE EXEC] poll {} attempt {} | status={} size={:.4f}",
                              order_id.substr(0, 16), attempt + 1, out.status, out.size_shares);
                return out;
            }
            if (out.status == "unmatched" || out.status == "cancelled" || out.status == "expired") {
                return out;
            }
        } catch (const std::exception& e) {
            spdlog::debug("[LIVE EXEC] poll {} attempt {} failed: {}", order_id.substr(0, 16), attempt + 1, e.what());
        }
    }
    return out;
}

EIP712Signer& OrderRouter::pick_signer(bool is_neg_risk) const {
    if (is_neg_risk && signer_neg_risk_) {
        return *signer_neg_risk_;
    }
    return *signer_;
}

std::string OrderRouter::generate_salt() const {
    static std::mutex salt_mutex;
    static std::mt19937 gen(std::random_device{}());
    static std::uniform_int_distribution<uint32_t> dis;
    std::lock_guard<std::mutex> lock(salt_mutex);
    return std::to_string(dis(gen));
}

std::string OrderRouter::compute_hmac_signature(const std::string& timestamp, const std::string& method, const std::string& path, const std::string& body) {
    std::string message = timestamp + method + path + body;
    auto decoded_secret = base64_decode(api_secret_);
    
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len;
    
    HMAC(EVP_sha256(), decoded_secret.data(), decoded_secret.size(),
         reinterpret_cast<const unsigned char*>(message.c_str()), message.length(),
         hash, &hash_len);
    
    return base64_encode(hash, hash_len);
}

std::string OrderRouter::base64_encode(const unsigned char* input, int length) {
    BIO *bio, *b64;
    BUF_MEM *bufferPtr;

    b64 = BIO_new(BIO_f_base64());
    bio = BIO_new(BIO_s_mem());
    bio = BIO_push(b64, bio);

    BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);
    BIO_write(bio, input, length);
    BIO_flush(bio);
    BIO_get_mem_ptr(bio, &bufferPtr);
    BIO_set_close(bio, BIO_NOCLOSE);

    std::string result(bufferPtr->data, bufferPtr->length);
    BIO_free_all(bio);

    return result;
}

std::vector<unsigned char> OrderRouter::base64_decode(const std::string& input) {
    BIO *bio, *b64;
    int decodeLen = calc_decode_length(input);
    std::vector<unsigned char> buffer(decodeLen);

    bio = BIO_new_mem_buf(input.c_str(), -1);
    b64 = BIO_new(BIO_f_base64());
    bio = BIO_push(b64, bio);

    BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);
    int actualLen = BIO_read(bio, buffer.data(), static_cast<int>(input.length()));
    buffer.resize(actualLen);
    BIO_free_all(bio);

    return buffer;
}

int OrderRouter::calc_decode_length(const std::string& b64input) {
    int len = static_cast<int>(b64input.size());
    int padding = 0;

    if (len > 0 && b64input[len-1] == '=' && len > 1 && b64input[len-2] == '=')
        padding = 2;
    else if (len > 0 && b64input[len-1] == '=')
        padding = 1;

    return (len * 3) / 4 - padding;
}

namespace {

double resize_for_ask_book(const BookAskInfo& book, double requested_shares) {
    if (!book.ok) return 0.0;
    const double min_sh = book.min_order_size > kFloatTol ? book.min_order_size : 5.0;
    double try_shares = std::min(requested_shares, book.depth_shares / kDepthFillRatio);
    while (try_shares + kFloatTol >= min_sh) {
        if (book.depth_shares + kFloatTol >= try_shares * kDepthFillRatio) {
            return try_shares;
        }
        try_shares = std::floor(try_shares * 0.5 * 100.0 + 1e-9) / 100.0;
    }
    return 0.0;
}

bool live_fill_usable(double price, double fill_shares, double min_order_size) {
    if (fill_shares + kFloatTol < kMinFillShares) return false;
    if (!leg_meets_minimum(price, fill_shares)) return false;
    (void)min_order_size;
    return true;
}

} // namespace

void OrderRouter::abandon_lih_pending(const LihPendingFill& pending, const char* reason) {
    switch (pending.kind) {
    case LegInAction::Kind::OpenLeg1:
        risk_manager_.end_lih_leg1_inflight(pending.market.asset, pending.market.window_minutes);
        break;
    case LegInAction::Kind::CompleteHedge:
        if (!pending.lih_id.empty()) {
            risk_manager_.end_lih_rebalance_inflight(pending.lih_id);
        }
        break;
    case LegInAction::Kind::AddLeg1:
        if (!pending.lih_id.empty()) {
            risk_manager_.end_lih_rebalance_inflight(pending.lih_id);
        }
        break;
    case LegInAction::Kind::UnwindLeg1:
        if (!pending.lih_id.empty()) {
            risk_manager_.end_lih_rebalance_inflight(pending.lih_id);
        }
        break;
    default:
        break;
    }
    spdlog::warn("[LIVE LIH] pending abandoned {} order_id={} — {}",
                 pending.market.asset, pending.order_id, reason);
    store_.push_telemetry(fmt::format(
        "[LIH LIVE] pending abandoned {} | order_id={} | {}",
        pending.market.asset, pending.order_id, reason));
}

bool OrderRouter::lih_pending_position_gone(const LihPendingFill& pending) const {
    const auto open = risk_manager_.get_open_lih_positions();
    if (pending.kind == LegInAction::Kind::OpenLeg1) {
        for (const auto& [id, pos] : open) {
            if (pos.asset == pending.market.asset &&
                pos.window_minutes == pending.market.window_minutes) {
                return true;
            }
            (void)id;
        }
        return false;
    }
    if (pending.lih_id.empty()) return false;
    return open.find(pending.lih_id) == open.end();
}

bool OrderRouter::lih_has_live_pending(const LegInAction& act) const {
    for (const auto& pending : lih_pending_fills_) {
        if (pending.order_id.empty()) continue;
        if (act.kind == LegInAction::Kind::OpenLeg1) {
            if (pending.kind == LegInAction::Kind::OpenLeg1 &&
                pending.market.asset == act.market.asset &&
                pending.market.window_minutes == act.market.window_minutes) {
                return true;
            }
            continue;
        }
        if (!act.lih_id.empty() && pending.lih_id == act.lih_id &&
            (pending.kind == LegInAction::Kind::CompleteHedge ||
             pending.kind == LegInAction::Kind::AddLeg1 ||
             pending.kind == LegInAction::Kind::UnwindLeg1)) {
            return true;
        }
    }
    return false;
}

void OrderRouter::drop_lih_pending_for(const LegInAction& act) {
    lih_pending_fills_.erase(
        std::remove_if(
            lih_pending_fills_.begin(), lih_pending_fills_.end(),
            [&](const LihPendingFill& pending) {
                if (act.kind == LegInAction::Kind::OpenLeg1) {
                    return pending.kind == LegInAction::Kind::OpenLeg1 &&
                           pending.market.asset == act.market.asset &&
                           pending.market.window_minutes == act.market.window_minutes;
                }
                return !act.lih_id.empty() && pending.lih_id == act.lih_id;
            }),
        lih_pending_fills_.end());
}

void OrderRouter::register_lih_from_pending(
    const LihPendingFill& pending, const LegFillResult& fill, double now_sec) {
    const char* side_label = pending.buy_yes ? "YES" : "NO";
    switch (pending.kind) {
    case LegInAction::Kind::OpenLeg1:
        risk_manager_.register_lih_open_leg1(
            pending.market, pending.buy_yes, fill.price, fill.size_shares, now_sec, false);
        store_.push_signal(fmt::format(
            "LIH LIVE LEG1 {} {} {:.2f}sh @ {:.4f} (pending resolved)",
            pending.market.asset, side_label, fill.size_shares, fill.price));
        spdlog::info("[LIVE LIH] LEG1 pending resolved {} {:.2f}sh order_id={}",
                     pending.market.asset, fill.size_shares, pending.order_id);
        break;
    case LegInAction::Kind::CompleteHedge: {
        const char* tag = "HEDGE";
        if (pending.lih_id.empty()) {
            spdlog::error("[LIVE LIH] pending {} missing lih_id order_id={}", tag, pending.order_id);
            return;
        }
        double reg_sh = fill.size_shares;
        const auto open = risk_manager_.get_open_lih_positions();
        const auto pit = open.find(pending.lih_id);
        if (pit != open.end()) {
            const double gap = std::abs(pit->second.yes_shares - pit->second.no_shares);
            if (gap <= kFloatTol) {
                spdlog::warn("[LIVE LIH] {} pending skip {} order_id={} — gap already 0",
                             tag, pending.market.asset, pending.order_id);
                return;
            }
            reg_sh = std::min(reg_sh, gap);
        }
        if (reg_sh <= kFloatTol) return;
        risk_manager_.register_lih_add_leg(
            pending.lih_id, pending.buy_yes, fill.price, reg_sh, false);
        store_.push_signal(fmt::format(
            "LIH LIVE {} {} {} {:.2f}sh @ {:.4f} (pending resolved)",
            tag, pending.market.asset, side_label, reg_sh, fill.price));
        spdlog::info("[LIVE LIH] {} pending resolved {} {:.2f}sh order_id={}",
                     tag, pending.market.asset, reg_sh, pending.order_id);
        break;
    }
    case LegInAction::Kind::AddLeg1: {
        const char* tag = "CLIP";
        if (pending.lih_id.empty()) {
            spdlog::error("[LIVE LIH] pending {} missing lih_id order_id={}", tag, pending.order_id);
            return;
        }
        risk_manager_.register_lih_add_leg(
            pending.lih_id, pending.buy_yes, fill.price, fill.size_shares, false, true);
        store_.push_signal(fmt::format(
            "LIH LIVE {} {} {:.2f}sh @ {:.4f} (pending resolved)",
            tag, side_label, fill.size_shares, fill.price));
        spdlog::info("[LIVE LIH] {} pending resolved {} {:.2f}sh order_id={}",
                     tag, pending.market.asset, fill.size_shares, pending.order_id);
        break;
    }
    case LegInAction::Kind::UnwindLeg1: {
        const char* tag = "UNWIND";
        if (pending.lih_id.empty()) {
            spdlog::error("[LIVE LIH] pending {} missing lih_id order_id={}", tag, pending.order_id);
            return;
        }
        double reg_sh = fill.size_shares;
        const auto open = risk_manager_.get_open_lih_positions();
        const auto pit = open.find(pending.lih_id);
        if (pit != open.end()) {
            const double gap = std::abs(pit->second.yes_shares - pit->second.no_shares);
            if (gap <= kFloatTol) {
                spdlog::warn("[LIVE LIH] {} pending skip {} order_id={} — gap already 0",
                             tag, pending.market.asset, pending.order_id);
                return;
            }
            reg_sh = std::min(reg_sh, gap);
        }
        if (reg_sh <= kFloatTol) return;
        risk_manager_.register_lih_unwind(
            pending.lih_id, pending.buy_yes, fill.price, reg_sh, false, true);
        store_.push_signal(fmt::format(
            "LIH LIVE {} sell {} {:.2f}sh @ {:.4f} (pending resolved)",
            tag, side_label, reg_sh, fill.price));
        spdlog::info("[LIVE LIH] {} pending resolved {} {:.2f}sh order_id={}",
                     tag, pending.market.asset, reg_sh, pending.order_id);
        break;
    }
    default:
        break;
    }
}

bool OrderRouter::lih_track_or_fail_live_order(
    const LegInAction& act,
    const std::string& token_id,
    LegFillResult& fill,
    double exec_px,
    double shares,
    double now_sec,
    const char* tag) {
    if (fill.success && fill.size_shares >= kMinFillShares) return false;
    if (fill.order_id.empty()) return false;
    track_lih_pending_fill(act, token_id, fill.order_id, exec_px, shares, now_sec);
    spdlog::warn("[LIVE LIH] {} awaiting fill {} order_id={} — no retry until resolved",
                 tag, act.market.asset, fill.order_id);
    store_.push_telemetry(fmt::format(
        "[LIH LIVE] {} pending {} | order_id={} | awaiting fill (no retry)",
        tag, act.market.asset, fill.order_id));
    fill.pending_fill = true;
    return true;
}

void OrderRouter::track_lih_pending_fill(
    const trading::LegInAction& act,
    const std::string& token_id,
    const std::string& order_id,
    double exec_px,
    double shares,
    double now_sec) {
    if (order_id.empty()) return;
    if (lih_has_live_pending(act)) {
        spdlog::warn("[LIVE LIH] pending already active {} {} — ignore order_id={}",
                     act.market.asset, act.lih_id, order_id);
        return;
    }
    for (auto& pending : lih_pending_fills_) {
        if (pending.order_id == order_id) {
            pending.last_poll_sec = 0.0;
            return;
        }
    }
    LihPendingFill pending;
    pending.kind = act.kind;
    pending.market = act.market;
    pending.buy_yes = act.buy_yes;
    pending.token_id = token_id;
    pending.order_id = order_id;
    pending.lih_id = act.lih_id;
    pending.exec_px = exec_px;
    pending.shares = shares;
    pending.started_at_sec = now_sec;
    pending.last_poll_sec = 0.0;
    lih_pending_fills_.push_back(std::move(pending));
    spdlog::info("[LIVE LIH] tracking pending {} order_id={}", act.market.asset, order_id);
}

int OrderRouter::poll_lih_pending_fills(double now_sec) {
    if (paper_mode_ || live_lih_dry_run_ || lih_pending_fills_.empty()) return 0;

    constexpr double kPollIntervalSec = 1.0;
    constexpr double kDeadAbandonSec = 45.0;
    constexpr double kStaleAbandonSec = 120.0;
    int resolved = 0;

    for (auto it = lih_pending_fills_.begin(); it != lih_pending_fills_.end(); ) {
        LihPendingFill& pending = *it;
        const double age = now_sec - pending.started_at_sec;

        if (lih_pending_position_gone(pending)) {
            abandon_lih_pending(pending, "position closed or leg1 already open");
            it = lih_pending_fills_.erase(it);
            continue;
        }

        if (now_sec - pending.last_poll_sec < kPollIntervalSec) {
            ++it;
            continue;
        }
        pending.last_poll_sec = now_sec;

        LegFillResult fill = resolve_clob_fill(
            pending.token_id, pending.exec_px, pending.order_id,
            pending.kind == LegInAction::Kind::UnwindLeg1 ? 1 : 0);
        if (fill.success && fill.size_shares >= kMinFillShares) {
            const auto open = risk_manager_.get_open_lih_positions();
            const auto pit = open.find(pending.lih_id);
            if (pit != open.end()
                && (pending.kind == LegInAction::Kind::CompleteHedge
                    || pending.kind == LegInAction::Kind::UnwindLeg1)) {
                const double gap = std::abs(pit->second.yes_shares - pit->second.no_shares);
                if (gap <= kFloatTol) {
                    abandon_lih_pending(pending, "gap already closed");
                    it = lih_pending_fills_.erase(it);
                    continue;
                }
            }
            register_lih_from_pending(pending, fill, now_sec);
            it = lih_pending_fills_.erase(it);
            ++resolved;
            continue;
        }

        bool abandon = false;
        const char* abandon_reason = nullptr;
        if (age >= kStaleAbandonSec) {
            abandon = true;
            abandon_reason = "timeout";
        } else if (age >= kDeadAbandonSec) {
            const auto polled = poll_order_fill(pending.order_id, pending.exec_px, pending.shares);
            if (polled.ok && polled.size_shares <= kFloatTol &&
                (polled.status == "unmatched" || polled.status == "cancelled" ||
                 polled.status == "expired" || polled.status.empty())) {
                abandon = true;
                abandon_reason = polled.status.empty() ? "0 fill confirmed" : polled.status.c_str();
            }
        }

        if (abandon) {
            abandon_lih_pending(pending, abandon_reason ? abandon_reason : "stale");
            it = lih_pending_fills_.erase(it);
            continue;
        }

        ++it;
    }
    return resolved;
}

bool OrderRouter::submit_lih_action(const trading::LegInAction& act, double now_sec) {
    if (paper_mode_) return false;

    const bool is_neg_risk = act.market.is_neg_risk;
    const double target = store_.lih_target_combined();
    const double leg1_trigger_min = store_.lih_leg1_trigger_min();
    const double leg1_trigger_max = store_.lih_leg1_trigger_max();
    const double leg1_max = store_.lih_leg1_trigger_mode()
        ? (leg1_trigger_max > 1e-6 ? leg1_trigger_max : 0.99)
        : (store_.lih_leg1_trend_mode()
            ? store_.lih_leg1_trend_max_price()
            : store_.lih_leg1_max_price());
    const char* side_label = act.buy_yes ? "YES" : "NO";

    auto shadow = [&](const char* tag, const std::string& detail) {
        const std::string line = fmt::format(
            "[LIVE LIH SHADOW] {} {} {}m | {} | dry_run — no order sent",
            tag, act.market.asset, act.market.window_minutes, detail);
        spdlog::info("{}", line);
        store_.push_telemetry(line);
        store_.push_signal(line);
        // Plain stdout → bridge captures as [CORE INFO] in logs/bridge.log (prelive scan).
        std::cout << line << std::endl;
    };

    switch (act.kind) {
    case LegInAction::Kind::OpenLeg1: {
        const std::string& tok = act.buy_yes ? act.market.yes_token_id : act.market.no_token_id;
        BookAskInfo book = fetch_book_ask_info(tok);
        if (!book.ok) {
            spdlog::warn("[LIVE LIH] LEG1 {} — empty ask book", act.market.asset);
            return false;
        }
        const double exec_px = book.best_ask;
        if (store_.lih_leg1_trigger_mode()) {
            if (exec_px + kFloatTol < leg1_trigger_min) {
                spdlog::info("[LIVE LIH] LEG1 skip {} | ask {:.4f} < trigger {:.4f}",
                             act.market.asset, exec_px, leg1_trigger_min);
                return false;
            }
            if (leg1_trigger_max > 1e-6 && exec_px > leg1_trigger_max + kFloatTol) {
                spdlog::info("[LIVE LIH] LEG1 skip {} | ask {:.4f} > trigger_max {:.4f}",
                             act.market.asset, exec_px, leg1_trigger_max);
                return false;
            }
        } else if (exec_px > leg1_max + kFloatTol) {
            spdlog::info("[LIVE LIH] LEG1 skip {} | ask {:.4f} > max {:.4f}",
                         act.market.asset, exec_px, leg1_max);
            return false;
        }
        // Detector gates on cached REST; fill uses a fresh book fetch. Re-check the
        // offhours floor on exec_px so stale cache cannot open lottery asks.
        if (offhours_exec_ask_blocked(store_, act.market, exec_px)) {
            spdlog::info(
                "[LIVE LIH] LEG1 skip {} | offhours exec ask {:.4f} < min {:.4f} "
                "(decision {:.4f})",
                act.market.asset, exec_px, store_.mm2_offhours_min_ask(), act.price);
            return false;
        }
        double shares = resize_for_ask_book(book, act.shares);
        const double min_sh = book.min_order_size > kFloatTol ? book.min_order_size : 5.0;
        if (shares + kFloatTol < act.shares) {
            spdlog::info("[LIVE LIH] LEG1 resize {} {:.2f} -> {:.2f} sh (book depth)",
                         act.market.asset, act.shares, shares);
        }
        if (shares + kFloatTol < min_sh) {
            spdlog::info("[LIVE LIH] LEG1 skip {} — book depth {:.2f} < min {:.2f} sh",
                         act.market.asset, shares, min_sh);
            return false;
        }
        if (!leg_meets_minimum(exec_px, shares)) {
            spdlog::warn("[LIVE LIH] LEG1 {} — depth/min not met for {:.2f} sh @ {:.4f}",
                         act.market.asset, shares, exec_px);
            return false;
        }
        const double cost = shares * exec_px;
        if (!risk_manager_.can_open_lih_leg(
                cost, false, nullptr, 0.0, &act.market.asset, act.market.window_minutes).first) {
            return false;
        }
        if (lih_has_live_pending(act)) {
            spdlog::debug("[LIVE LIH] LEG1 skip {} — pending order in flight", act.market.asset);
            return false;
        }

        if (!risk_manager_.try_begin_lih_leg1(act.market.asset, act.market.window_minutes)) {
            spdlog::warn("[LIVE LIH] LEG1 blocked — in-flight or open {} {}m",
                         act.market.asset, act.market.window_minutes);
            return false;
        }

        const std::string detail = fmt::format("{} {:.2f}sh @ {:.4f} ({})", side_label, shares, exec_px, act.note);
        const std::string intent = lih_intent_for(act);
        if (live_lih_dry_run_) {
            std::string post_type = "FAK";
            double shadow_px = exec_px;
            const std::string order_mode = store_.lih_leg1_order_mode();
            if (order_mode == "gtc") {
                const double tick = book.tick_size > kFloatTol ? book.tick_size : 0.01;
                shadow_px = std::max(tick, book.best_ask - tick);
                post_type = "GTC";
            }
            risk::LihExecMeta exec;
            infer_shadow_exec_meta(exec, shadow_px, book.best_ask, 0.0, post_type, intent);
            risk_manager_.register_lih_open_leg1(
                act.market, act.buy_yes, shadow_px, shares, now_sec, true, false, true, &exec);
            shadow("LEG1", detail);
            return true;
        }

        LegFillResult fill = execute_leg_buy(
            tok, exec_px, shares, is_neg_risk, "FAK", intent, book.best_ask, 0.0, true, false);
        if ((!fill.success || fill.size_shares < kMinFillShares) && use_python_clob_) {
            LegFillResult resolved = resolve_clob_fill(tok, exec_px, fill.order_id, 0);
            if (resolved.success && resolved.size_shares >= kMinFillShares) {
                fill = resolved;
            } else if (!fill.order_id.empty() && resolved.order_id.empty()) {
                resolved.order_id = fill.order_id;
            }
            if (!fill.success && !resolved.order_id.empty()) {
                fill.order_id = resolved.order_id;
                fill.pending_fill = true;
            }
        }
        if (fill.pending_fill || lih_track_or_fail_live_order(
                act, tok, fill, exec_px, shares, now_sec, "LEG1")) {
            return false;
        }
        if (!fill.success || !live_fill_usable(fill.price > 0 ? fill.price : exec_px, fill.size_shares, min_sh)) {
            risk_manager_.end_lih_leg1_inflight(act.market.asset, act.market.window_minutes);
            spdlog::error("[LIVE LIH] LEG1 buy failed {} (filled {:.4f})",
                          act.market.asset, fill.size_shares);
            return false;
        }
        if (fill.size_shares + kFloatTol < shares) {
            spdlog::info("[LIVE LIH] LEG1 partial {:.2f}/{:.2f} {} — accepting fill",
                         fill.size_shares, shares, act.market.asset);
        }
        drop_lih_pending_for(act);
        risk_manager_.register_lih_open_leg1(
            act.market, act.buy_yes, fill.price, fill.size_shares, now_sec, false);
        store_.push_signal(fmt::format("LIH LIVE LEG1 {} {} {:.2f}sh @ {:.4f} ({})",
            act.market.asset, side_label, fill.size_shares, fill.price, act.note));
        return true;
    }

    case LegInAction::Kind::AddLeg1: {
        const std::string& tok = act.buy_yes ? act.market.yes_token_id : act.market.no_token_id;
        BookAskInfo book = fetch_book_ask_info(tok);
        if (!book.ok) {
            spdlog::warn("[LIVE LIH] CLIP {} — empty ask book", act.market.asset);
            return false;
        }
        const double exec_px = book.best_ask;
        if (!store_.lih_leg1_trigger_mode() && !store_.lih_leg1_trend_mode()) {
            const double leg1_max = store_.lih_leg1_max_price();
            if (exec_px > leg1_max + kFloatTol) {
                spdlog::info("[LIVE LIH] CLIP skip {} | ask {:.4f} > max {:.4f}",
                             act.market.asset, exec_px, leg1_max);
                return false;
            }
        }
        double shares = resize_for_ask_book(book, act.shares);
        const double min_sh = book.min_order_size > kFloatTol ? book.min_order_size : 5.0;
        if (shares + kFloatTol < min_sh || !leg_meets_minimum(exec_px, shares)) return false;
        const double cost = shares * exec_px;
        if (!risk_manager_.can_open_lih_leg(cost, true, &act.lih_id, shares).first) return false;
        if (act.lih_id.empty() || !risk_manager_.try_begin_lih_rebalance(act.lih_id)) {
            spdlog::warn("[LIVE LIH] CLIP blocked — in-flight or missing lih_id {}", act.lih_id);
            return false;
        }
        const char* tag = "CLIP";
        const std::string detail = fmt::format(
            "{} {:.2f}sh @ {:.4f} ({})", side_label, shares, exec_px, act.note);
        const std::string intent = lih_intent_for(act);
        if (live_lih_dry_run_) {
            risk::LihExecMeta exec;
            infer_shadow_exec_meta(exec, exec_px, book.best_ask, 0.0, "FAK", intent);
            risk_manager_.register_lih_add_leg(act.lih_id, act.buy_yes, exec_px, shares, true, false, &exec);
            shadow(tag, detail);
            return true;
        }
        if (lih_has_live_pending(act)) return false;
        LegFillResult fill = execute_leg_buy(
            tok, exec_px, shares, is_neg_risk, "FAK", intent, book.best_ask, 0.0, true, false);
        if ((!fill.success || fill.size_shares < kMinFillShares) && use_python_clob_) {
            LegFillResult resolved = resolve_clob_fill(tok, exec_px, fill.order_id, 0);
            if (resolved.success && resolved.size_shares >= kMinFillShares) fill = resolved;
        }
        if (fill.pending_fill || lih_track_or_fail_live_order(
                act, tok, fill, exec_px, shares, now_sec, tag)) {
            return false;
        }
        if (!fill.success || !live_fill_usable(fill.price > 0 ? fill.price : exec_px, fill.size_shares, min_sh)) {
            risk_manager_.end_lih_rebalance_inflight(act.lih_id);
            return false;
        }
        if (fill.size_shares + kFloatTol < shares) {
            spdlog::info("[LIVE LIH] CLIP partial {:.2f}/{:.2f} {} — accepting fill",
                         fill.size_shares, shares, act.market.asset);
        }
        drop_lih_pending_for(act);
        risk_manager_.register_lih_add_leg(act.lih_id, act.buy_yes, fill.price, fill.size_shares, false);
        store_.push_signal(fmt::format("LIH LIVE {} {} {} {:.2f}sh @ {:.4f} ({})",
            tag, act.market.asset, side_label, fill.size_shares, fill.price, act.note));
        return true;
    }

    case LegInAction::Kind::CompleteHedge: {
        const std::string& tok = act.buy_yes ? act.market.yes_token_id : act.market.no_token_id;
        BookAskInfo book = fetch_book_ask_info(tok);
        if (!book.ok) {
            spdlog::warn("[LIVE LIH] HEDGE {} — empty ask book", act.market.asset);
            return false;
        }
        const double exec_px = book.best_ask;

        if (!act.lih_id.empty()) {
            auto open = risk_manager_.get_open_lih_positions();
            auto it = open.find(act.lih_id);
            if (it != open.end()) {
                const auto& pos = it->second;
                const double yes_avg = pos.yes_shares > kFloatTol ? pos.yes_cost / pos.yes_shares : 0.0;
                const double no_avg = pos.no_shares > kFloatTol ? pos.no_cost / pos.no_shares : 0.0;
                const double heavy_avg = act.buy_yes ? no_avg : yes_avg;
                if (heavy_avg > kFloatTol && !lih_action_is_force(act)) {
                    const double marginal = heavy_avg + exec_px;
                    double cap = target;
                    if (act.note.find("mid-soft") != std::string::npos) {
                        const double mid = store_.lih_mid_soft_cap();
                        if (mid > target + kFloatTol) cap = mid;
                    }
                    if (marginal > cap + kFloatTol) {
                        spdlog::info("[LIVE LIH] hedge skip {} | marginal {:.4f} > cap {:.4f}",
                                     act.market.asset, marginal, cap);
                        return false;
                    }
                }
            }
        }

        double shares = resize_for_ask_book(book, act.shares);
        const double min_sh = book.min_order_size > kFloatTol ? book.min_order_size : 5.0;
        if (shares + kFloatTol < act.shares) {
            spdlog::info("[LIVE LIH] HEDGE {} — book resize {:.2f} -> {:.2f} sh",
                         act.market.asset, act.shares, shares);
        }
        if (shares + kFloatTol < min_sh) {
            spdlog::warn("[LIVE LIH] HEDGE {} — depth not met for {:.2f} sh (min {:.2f})",
                         act.market.asset, shares, min_sh);
            return false;
        }
        if (!leg_meets_minimum(exec_px, shares)) return false;

        const double cost = shares * exec_px;
        if (!risk_manager_.can_open_lih_leg(cost, true, &act.lih_id, shares).first) return false;

        const char* tag = "HEDGE";
        const std::string detail = fmt::format("{} {:.2f}sh @ {:.4f} ({})", side_label, shares, exec_px, act.note);
        const std::string intent = lih_intent_for(act);
        if (live_lih_dry_run_) {
            if (act.lih_id.empty() || !risk_manager_.try_begin_lih_rebalance(act.lih_id)) {
                spdlog::warn("[LIVE LIH] {} shadow blocked — rebalance in-flight or missing lih_id {}",
                             tag, act.lih_id);
                return false;
            }
            risk::LihExecMeta exec;
            infer_shadow_exec_meta(exec, exec_px, book.best_ask, 0.0, "FAK", intent);
            risk_manager_.register_lih_add_leg(act.lih_id, act.buy_yes, exec_px, shares, true, false, &exec);
            shadow(tag, detail);
            return true;
        }

        if (lih_has_live_pending(act)) {
            spdlog::debug("[LIVE LIH] {} skip {} — pending hedge order in flight", tag, act.market.asset);
            return false;
        }
        if (act.lih_id.empty() || !risk_manager_.try_begin_lih_rebalance(act.lih_id)) {
            spdlog::warn("[LIVE LIH] {} blocked — rebalance in-flight or missing lih_id {}",
                         tag, act.lih_id);
            return false;
        }

        LegFillResult fill = execute_leg_buy(
            tok, exec_px, shares, is_neg_risk, "FAK", intent, book.best_ask, 0.0, true, false);
        if ((!fill.success || fill.size_shares < kMinFillShares) && use_python_clob_) {
            LegFillResult resolved = resolve_clob_fill(tok, exec_px, fill.order_id, 0);
            if (resolved.success && resolved.size_shares >= kMinFillShares) {
                fill = resolved;
            } else if (!fill.order_id.empty() && resolved.order_id.empty()) {
                resolved.order_id = fill.order_id;
            }
            if (!fill.success && !resolved.order_id.empty()) {
                fill.order_id = resolved.order_id;
                fill.pending_fill = true;
            }
        }
        if (fill.pending_fill || lih_track_or_fail_live_order(
                act, tok, fill, exec_px, shares, now_sec, tag)) {
            return false;
        }
        if (!fill.success || !live_fill_usable(fill.price > 0 ? fill.price : exec_px, fill.size_shares, min_sh)) {
            risk_manager_.end_lih_rebalance_inflight(act.lih_id);
            spdlog::error("[LIVE LIH] {} failed {} (filled {:.4f}/{:.4f})",
                          tag, act.market.asset, fill.size_shares, shares);
            return false;
        }
        if (fill.size_shares + kFloatTol < shares) {
            spdlog::info("[LIVE LIH] {} partial {:.2f}/{:.2f} {} — accepting fill",
                         tag, fill.size_shares, shares, act.market.asset);
        }
        drop_lih_pending_for(act);
        risk_manager_.register_lih_add_leg(act.lih_id, act.buy_yes, fill.price, fill.size_shares, false);
        store_.push_signal(fmt::format("LIH LIVE {} {} {} {:.2f}sh @ {:.4f} ({})",
            tag, act.market.asset, side_label, fill.size_shares, fill.price, act.note));
        return true;
    }

    case LegInAction::Kind::UnwindLeg1: {
        const std::string& tok = act.buy_yes ? act.market.yes_token_id : act.market.no_token_id;
        BookBidInfo book = fetch_book_bid_info(tok);
        if (!book.ok) {
            spdlog::warn("[LIVE LIH] UNWIND {} — empty bid book", act.market.asset);
            return false;
        }
        const double exec_px = book.best_bid;
        double shares = act.shares;
        if (shares < kMinFillShares) {
            spdlog::warn("[LIVE LIH] UNWIND {} — size {:.2f} below minimum", act.market.asset, shares);
            return false;
        }
        if (!leg_meets_minimum(exec_px, shares)) return false;

        const char* tag = "UNWIND";
        const std::string detail = fmt::format(
            "sell {} {:.2f}sh @ {:.4f} ({})",
            side_label, shares, exec_px, act.note);
        if (live_lih_dry_run_) {
            if (act.lih_id.empty() || !risk_manager_.try_begin_lih_rebalance(act.lih_id)) {
                spdlog::warn("[LIVE LIH] {} shadow blocked — in-flight or missing lih_id {}",
                             tag, act.lih_id);
                return false;
            }
            if (!risk_manager_.register_lih_unwind(
                    act.lih_id, act.buy_yes, exec_px, shares, true, false)) {
                risk_manager_.end_lih_rebalance_inflight(act.lih_id);
                return false;
            }
            shadow(tag, detail);
            return true;
        }

        if (lih_has_live_pending(act)) {
            spdlog::debug("[LIVE LIH] {} skip {} — pending order in flight", tag, act.market.asset);
            return false;
        }
        if (act.lih_id.empty() || !risk_manager_.try_begin_lih_rebalance(act.lih_id)) {
            spdlog::warn("[LIVE LIH] {} blocked — in-flight or missing lih_id {}",
                         tag, act.lih_id);
            return false;
        }

        LegFillResult fill = execute_unwind_sell(tok, exec_px, shares, is_neg_risk);
        if ((!fill.success || fill.size_shares < kMinFillShares) && use_python_clob_) {
            LegFillResult resolved = resolve_clob_fill(tok, exec_px, fill.order_id, 1);
            if (resolved.success && resolved.size_shares >= kMinFillShares) {
                fill = resolved;
            } else if (!fill.order_id.empty() && resolved.order_id.empty()) {
                resolved.order_id = fill.order_id;
            }
            if (!fill.success && !resolved.order_id.empty()) {
                fill.order_id = resolved.order_id;
                fill.pending_fill = true;
            }
        }
        if (fill.pending_fill || lih_track_or_fail_live_order(
                act, tok, fill, exec_px, shares, now_sec, tag)) {
            return false;
        }
        if (!fill.success || fill.size_shares < kMinFillShares) {
            risk_manager_.end_lih_rebalance_inflight(act.lih_id);
            spdlog::error("[LIVE LIH] {} failed {} (filled {:.4f}/{:.4f})",
                          tag, act.market.asset, fill.size_shares, shares);
            return false;
        }
        drop_lih_pending_for(act);
        risk_manager_.register_lih_unwind(
            act.lih_id, act.buy_yes, fill.price, fill.size_shares, false, true);
        store_.push_signal(fmt::format("LIH LIVE {} {} {} {:.2f}sh @ {:.4f} ({})",
            tag, act.market.asset, side_label, fill.size_shares, fill.price, act.note));
        return true;
    }
    }
    return false;
}

} // namespace exec
} // namespace trading
