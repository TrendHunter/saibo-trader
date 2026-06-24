#pragma once
#include <string>

namespace trading {

struct MarketInfo {
    std::string condition_id;
    std::string question;
    std::string asset;
    std::string yes_token_id;
    std::string no_token_id;
    double strike;
    double end_date_ts; // UNIX timestamp in seconds
    std::string end_date_iso; // ISO-8601 string for display
    double volume = 0.0; // 24h volume in USD for sorting
    double yes_price = 0.5;
    double no_price = 0.5;
    double liquidity = 0.0;
    int window_minutes = 5; // 5 or 15 — Polymarket up/down series length
    // Neg-risk markets (all Polymarket 5m/15m Up-Down markets) require a
    // different EIP-712 verifying contract. Must be detected from Gamma API
    // and threaded through to the signer — wrong contract → order_version_mismatch.
    bool is_neg_risk = false;
};

} // namespace trading
