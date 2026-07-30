#include "ChainlinkFeed.h"
#include <spdlog/spdlog.h>
#include <boost/json.hpp>
#include <chrono>
#include <algorithm>
#include <cctype>

namespace trading {

namespace {

std::string lower_copy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

}  // namespace

ChainlinkFeed::ChainlinkFeed(net::io_context& ioc, ssl::context& ctx, StateStore& store)
    : resolver_(net::make_strand(ioc)),
      ioc_(ioc),
      ctx_(ctx),
      ws_(std::in_place, net::make_strand(ioc), ctx),
      timer_(net::make_strand(ioc)),
      ping_timer_(net::make_strand(ioc)),
      store_(store) {}

ChainlinkFeed::~ChainlinkFeed() = default;

void ChainlinkFeed::start() {
    running_ = true;
    resolve();
}

void ChainlinkFeed::stop() {
    running_ = false;
    connected_ = false;
    timer_.cancel();
    ping_timer_.cancel();
    if (ws_ && ws_->is_open()) {
        beast::error_code ec;
        ws_->close(websocket::close_code::normal, ec);
    }
}

void ChainlinkFeed::resolve() {
    resolver_.async_resolve(
        host_, port_,
        beast::bind_front_handler(&ChainlinkFeed::on_resolve, shared_from_this()));
}

void ChainlinkFeed::on_resolve(beast::error_code ec, tcp::resolver::results_type results) {
    if (ec) {
        spdlog::warn("ChainlinkFeed resolve failed: {}", ec.message());
        return reconnect();
    }
    beast::get_lowest_layer(*ws_).async_connect(
        results,
        beast::bind_front_handler(&ChainlinkFeed::on_connect, shared_from_this()));
}

void ChainlinkFeed::on_connect(beast::error_code ec, tcp::resolver::results_type::endpoint_type) {
    if (ec) {
        spdlog::warn("ChainlinkFeed connect failed: {}", ec.message());
        return reconnect();
    }
    SSL_set_tlsext_host_name(ws_->next_layer().native_handle(), host_.c_str());
    ws_->next_layer().async_handshake(
        ssl::stream_base::client,
        beast::bind_front_handler(&ChainlinkFeed::on_ssl_handshake, shared_from_this()));
}

void ChainlinkFeed::on_ssl_handshake(beast::error_code ec) {
    if (ec) {
        spdlog::warn("ChainlinkFeed SSL failed: {}", ec.message());
        return reconnect();
    }
    ws_->set_option(websocket::stream_base::decorator([](websocket::request_type& req) {
        req.set(http::field::user_agent, "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36");
        req.set(http::field::origin, "https://polymarket.com");
    }));
    ws_->async_handshake(
        host_, path_,
        beast::bind_front_handler(&ChainlinkFeed::on_handshake, shared_from_this()));
}

void ChainlinkFeed::on_handshake(beast::error_code ec) {
    if (ec) {
        spdlog::warn("ChainlinkFeed WS handshake failed: {}", ec.message());
        return reconnect();
    }
    connected_ = true;
    spdlog::info("ChainlinkFeed: connected (Polymarket RTDS crypto_prices_chainlink)");
    send_subscribe();
    send_ping();
    do_read();
}

void ChainlinkFeed::send_subscribe() {
    if (!connected_ || !ws_ || !ws_->is_open()) return;
    // Subscribe to full Chainlink stream; filter assets in process_message.
    boost::json::object sub;
    sub["action"] = "subscribe";
    boost::json::array subs;
    boost::json::object one;
    one["topic"] = "crypto_prices_chainlink";
    one["type"] = "*";
    subs.push_back(std::move(one));
    sub["subscriptions"] = std::move(subs);
    const std::string json = boost::json::serialize(sub);
    ws_->async_write(net::buffer(json), [](beast::error_code ec, std::size_t) {
        if (ec) spdlog::warn("ChainlinkFeed subscribe write failed: {}", ec.message());
    });
}

void ChainlinkFeed::send_ping() {
    if (!running_ || !connected_) return;
    ping_timer_.expires_after(std::chrono::seconds(10));
    ping_timer_.async_wait([self = shared_from_this()](beast::error_code ec) {
        if (ec || !self->running_ || !self->connected_ || !self->ws_ || !self->ws_->is_open()) {
            return;
        }
        // RTDS accepts empty ping frames / JSON ping; keep-alive via control ping.
        self->ws_->async_ping({}, [self](beast::error_code write_ec) {
            if (!write_ec) self->send_ping();
        });
    });
}

void ChainlinkFeed::do_read() {
    if (!running_) return;
    ws_->async_read(
        buffer_,
        beast::bind_front_handler(&ChainlinkFeed::on_read, shared_from_this()));
}

void ChainlinkFeed::on_read(beast::error_code ec, std::size_t) {
    if (ec) {
        spdlog::warn("ChainlinkFeed read failed: {}", ec.message());
        connected_ = false;
        return reconnect();
    }
    process_message(beast::buffers_to_string(buffer_.data()));
    buffer_.consume(buffer_.size());
    do_read();
}

void ChainlinkFeed::apply_price(const std::string& symbol, double price, int64_t ts_ms) {
    if (price <= 0.0) return;
    const std::string sym = lower_copy(symbol);
    PriceTick tick;
    tick.price = price;
    tick.timestamp_ms = static_cast<double>(ts_ms);
    tick.received_at = std::chrono::duration<double>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    tick.volume = 0.0;

    std::string asset;
    if (sym == "btc/usd" || sym == "btc-usd" || sym == "btcusd") {
        store_.update_btc_price(tick);
        asset = "btc";
    } else if (sym == "eth/usd" || sym == "eth-usd" || sym == "ethusd") {
        store_.update_eth_price(tick);
        asset = "eth";
    } else if (sym == "sol/usd" || sym == "sol-usd" || sym == "solusd") {
        store_.update_sol_price(tick);
        asset = "sol";
    } else {
        return;
    }
    if (tick_callback_) tick_callback_(asset, price);
}

void ChainlinkFeed::process_message(std::string_view msg) {
    if (msg.empty()) return;
    try {
        auto jv = boost::json::parse(msg);
        if (!jv.is_object()) return;
        const auto& obj = jv.as_object();
        const std::string topic = obj.contains("topic") && obj.at("topic").is_string()
            ? std::string(obj.at("topic").as_string())
            : "";
        if (topic != "crypto_prices_chainlink") return;

        if (!obj.contains("payload") || !obj.at("payload").is_object()) return;
        const auto& payload = obj.at("payload").as_object();

        // Live: { symbol, value|full_accuracy_value, timestamp }
        // Snapshot: { symbol, data: [{timestamp,value}, ...] }
        if (!payload.contains("symbol") || !payload.at("symbol").is_string()) return;
        const std::string symbol(payload.at("symbol").as_string());
        double price = 0.0;
        int64_t ts_ms = 0;

        auto read_num = [](const boost::json::value& v) -> double {
            if (v.is_double()) return v.as_double();
            if (v.is_int64()) return static_cast<double>(v.as_int64());
            if (v.is_uint64()) return static_cast<double>(v.as_uint64());
            if (v.is_string()) {
                try {
                    return std::stod(std::string(v.as_string()));
                } catch (...) {
                    return 0.0;
                }
            }
            return 0.0;
        };
        auto read_ts = [](const boost::json::value& t) -> int64_t {
            if (t.is_int64()) return t.as_int64();
            if (t.is_uint64()) return static_cast<int64_t>(t.as_uint64());
            if (t.is_double()) return static_cast<int64_t>(t.as_double());
            return 0;
        };

        if (payload.contains("full_accuracy_value")) {
            price = read_num(payload.at("full_accuracy_value"));
        }
        if (price <= 0.0 && payload.contains("value")) {
            price = read_num(payload.at("value"));
        }
        if (payload.contains("timestamp")) {
            ts_ms = read_ts(payload.at("timestamp"));
        }
        if (price <= 0.0 && payload.contains("data") && payload.at("data").is_array()) {
            const auto& arr = payload.at("data").as_array();
            if (!arr.empty() && arr.back().is_object()) {
                const auto& o = arr.back().as_object();
                if (o.contains("value")) price = read_num(o.at("value"));
                if (o.contains("timestamp")) ts_ms = read_ts(o.at("timestamp"));
            }
        }
        apply_price(symbol, price, ts_ms);
    } catch (const std::exception& e) {
        spdlog::error("ChainlinkFeed process_message error: {}", e.what());
    }
}

void ChainlinkFeed::reconnect() {
    if (!running_) return;
    connected_ = false;
    ping_timer_.cancel();
    timer_.expires_after(std::chrono::seconds(2));
    timer_.async_wait([self = shared_from_this()](beast::error_code ec) {
        if (ec || !self->running_) return;
        self->ws_.emplace(net::make_strand(self->ioc_), self->ctx_);
        self->resolve();
    });
}

}  // namespace trading
