#pragma once

#include "EIP712Signer.h"
#include "../state/StateStore.h"
#include "../risk/RiskManager.h"
#include "../signals/LegInHedgeDetector.h"
#include "../signals/Signal.h"
#include <string>
#include <memory>
#include <mutex>
#include <optional>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <boost/json.hpp>

namespace trading {
namespace exec {

struct LegFillResult {
    bool success = false;
    double price = 0.0;
    double size_shares = 0.0;
    std::string order_id;
    /** Order submitted but fill not confirmed — do not release in-flight locks. */
    bool pending_fill = false;
};

struct BookAskInfo {
    bool ok = false;
    double best_ask = 0.0;
    double depth_shares = 0.0;
};

struct BookBidInfo {
    bool ok = false;
    double best_bid = 0.0;
};

class OrderRouter {
public:
    OrderRouter(boost::asio::io_context& ioc, 
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
                const std::string& api_key = "",
                const std::string& api_secret = "",
                const std::string& api_passphrase = "",
                const std::string& neg_risk_exchange = "",
                bool live_lih_dry_run = true,
                bool use_python_clob = false,
                const std::string& clob_bridge_host = "127.0.0.1",
                int clob_bridge_port = 8081,
                const std::string& clob_bridge_path = "/internal/clob/order");

    ~OrderRouter();

    /** Live LIH execution (book-aware). Returns false on validation failure. No-op when LIVE_LIH_DRY_RUN. */
    bool submit_lih_action(const trading::LegInAction& act, double now_sec);

    /** Re-resolve pending CLOB fills and register when confirmed. No-op in paper/shadow. */
    int poll_lih_pending_fills(double now_sec);

    bool live_lih_dry_run() const { return live_lih_dry_run_; }
    void set_live_lih_dry_run(bool v) { live_lih_dry_run_ = v; }

    bool submit_order(const std::string& token_id, 
                      double price, 
                      double size, 
                      uint8_t side,
                      bool is_neg_risk = false);

    bool check_book_depth(const std::string& token_id, double price, double size_shares);

    // Scheme A: poll CLOB REST order book into StateStore (asks for entry, bids for marks).
    void refresh_rest_book(const std::vector<std::string>& token_ids);
    void refresh_rest_book_asks(const std::vector<std::string>& token_ids);

    // Sum of ask sizes at or below price * 1.02; -1 on fetch/parse failure.
    double query_ask_depth_shares(const std::string& token_id, double price);

    void submit_close_order(const std::string& order_id, const std::string& token_id, double current_price, double size, const std::string& asset, const std::string& question, double end_date_ts, const std::string& strategy, bool is_neg_risk = false);

private:
    boost::asio::io_context& ioc_;
    boost::asio::ssl::context& ctx_;
    trading::StateStore& store_;
    risk::RiskManager& risk_manager_;
    mutable std::mutex http_mutex_;

    std::string clob_api_url_;
    std::string signer_address_;
    std::string funder_address_;
    bool paper_mode_;
    bool live_lih_dry_run_;
    std::string api_key_;
    std::string api_secret_;
    std::string api_passphrase_;
    std::string neg_risk_exchange_;
    bool use_python_clob_;
    std::string clob_bridge_host_;
    int clob_bridge_port_;
    std::string clob_bridge_path_;

    struct LihPendingFill {
        LegInAction::Kind kind = LegInAction::Kind::OpenLeg1;
        trading::MarketInfo market;
        bool buy_yes = false;
        std::string token_id;
        std::string order_id;
        std::string lih_id;
        double exec_px = 0.0;
        double shares = 0.0;
        double started_at_sec = 0.0;
        double last_poll_sec = 0.0;
    };
    std::vector<LihPendingFill> lih_pending_fills_;

    void track_lih_pending_fill(
        const trading::LegInAction& act,
        const std::string& token_id,
        const std::string& order_id,
        double exec_px,
        double shares,
        double now_sec);
    void abandon_lih_pending(const LihPendingFill& pending, const char* reason);
    bool lih_pending_position_gone(const LihPendingFill& pending) const;
    /** True if a live CLOB order is still awaiting fill confirmation (blocks retry). */
    bool lih_has_live_pending(const trading::LegInAction& act) const;
    void drop_lih_pending_for(const trading::LegInAction& act);
    void register_lih_from_pending(const LihPendingFill& pending, const LegFillResult& fill, double now_sec);
    /** Track order_id when POST returned no immediate fill — keeps inflight lock, no retry. */
    bool lih_track_or_fail_live_order(
        const trading::LegInAction& act,
        const std::string& token_id,
        LegFillResult& fill,
        double exec_px,
        double shares,
        double now_sec,
        const char* tag);
    
    std::unique_ptr<EIP712Signer> signer_;
    std::unique_ptr<EIP712Signer> signer_neg_risk_;

    Order build_order(const std::string& token_id, double price, double size_shares, uint8_t side) const;

    LegFillResult execute_via_clob_bridge(
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
        const std::string& position_id_salt
    );
    LegFillResult resolve_clob_fill(
        const std::string& token_id,
        double fallback_price,
        const std::string& order_id,
        uint8_t side = 0
    );

    // When register_position=false, sends to CLOB only (used for LIH legs / unwind).
    LegFillResult execute_rest_order(
        const Order& order,
        const Signature& sig,
        bool is_neg_risk,
        bool register_position,
        const std::string& asset = "",
        const std::string& question = "",
        double end_date_ts = 0.0,
        const std::string& strategy = "MANUAL",
        const std::string& original_order_id = ""
    );

    bool simulate_paper_order(const Order& order, const Signature& sig, const std::string& asset = "", const std::string& question = "", double end_date_ts = 0.0, const std::string& strategy = "MANUAL", const std::string& original_order_id = "", bool is_neg_risk = false, const std::string& direction = "");

    LegFillResult execute_leg_buy(const std::string& token_id, double price, double size_shares, bool is_neg_risk);
    LegFillResult execute_unwind_sell(const std::string& token_id, double price, double size_shares, bool is_neg_risk);

    std::optional<boost::json::object> fetch_book_object(const std::string& token_id);
    BookAskInfo parse_book_asks(const boost::json::object& book) const;
    std::vector<trading::StateStore::BookLevel> parse_ask_ladder(const boost::json::object& book) const;
    BookBidInfo parse_book_bids(const boost::json::object& book) const;
    BookAskInfo fetch_book_ask_info(const std::string& token_id);

    EIP712Signer& pick_signer(bool is_neg_risk) const;

    std::string generate_salt() const;
    std::string compute_hmac_signature(const std::string& timestamp, const std::string& method, const std::string& path, const std::string& body);
    std::string authenticated_http_get(const std::string& target);
    struct PolledFill {
        bool ok = false;
        std::string status;
        double size_shares = 0.0;
        double price = 0.0;
    };
    PolledFill poll_order_fill(const std::string& order_id, double fallback_price, double requested_shares);
    std::string extract_order_id(const boost::json::object& obj) const;
    double parse_matched_size(const boost::json::value& raw) const;
    std::string base64_encode(const unsigned char* input, int length);
    std::vector<unsigned char> base64_decode(const std::string& input);
    int calc_decode_length(const std::string& b64input);
};

} // namespace exec
} // namespace trading
