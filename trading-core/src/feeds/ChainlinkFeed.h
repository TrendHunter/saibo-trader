#pragma once
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <string>
#include <memory>
#include <optional>
#include <deque>
#include <functional>
#include <unordered_set>
#include "../state/StateStore.h"

namespace trading {

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = boost::asio::ip::tcp;

/**
 * Polymarket RTDS Chainlink oracle prices (settlement source for BTC/ETH/SOL up-down).
 * Host: wss://ws-live-data.polymarket.com  topic: crypto_prices_chainlink
 */
class ChainlinkFeed : public std::enable_shared_from_this<ChainlinkFeed> {
public:
    explicit ChainlinkFeed(net::io_context& ioc, ssl::context& ctx, StateStore& store);
    ~ChainlinkFeed();

    void start();
    void stop();

    using TickCallback = std::function<void(const std::string& /*asset*/, double /*price*/)>;
    void set_tick_callback(TickCallback cb) { tick_callback_ = std::move(cb); }

private:
    void resolve();
    void on_resolve(beast::error_code ec, tcp::resolver::results_type results);
    void on_connect(beast::error_code ec, tcp::resolver::results_type::endpoint_type ep);
    void on_ssl_handshake(beast::error_code ec);
    void on_handshake(beast::error_code ec);
    void send_subscribe();
    void send_ping();
    void do_read();
    void on_read(beast::error_code ec, std::size_t bytes_transferred);
    void process_message(std::string_view msg);
    void apply_price(const std::string& symbol, double price, int64_t ts_ms);
    void reconnect();

    tcp::resolver resolver_;
    net::io_context& ioc_;
    ssl::context& ctx_;
    std::optional<websocket::stream<beast::ssl_stream<beast::tcp_stream>>> ws_;
    net::steady_timer timer_;
    net::steady_timer ping_timer_;
    beast::flat_buffer buffer_;

    StateStore& store_;
    std::string host_ = "ws-live-data.polymarket.com";
    std::string port_ = "443";
    std::string path_ = "/";

    TickCallback tick_callback_;
    bool running_ = false;
    bool connected_ = false;
};

}  // namespace trading
