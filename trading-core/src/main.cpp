#include <iostream>
#include <fstream>
#include <filesystem>
#include <string>
#include <unordered_set>
#include <spdlog/spdlog.h>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <thread>
#include <chrono>
#include <mutex>
#include <algorithm>
#include "state/StateStore.h"
#include "feeds/BinanceFeed.h"
#include "feeds/PolymarketFeed.h"
#include "feeds/GammaClient.h"
#include "risk/RiskManager.h"
#include "signals/DumpHedgeDetector.h"
#include "signals/LegInHedgeDetector.h"
#include "exec/OrderRouter.h"
#include "state/PaperStateStore.h"
#include <spdlog/sinks/basic_file_sink.h>
#include <fmt/core.h>
#include <atomic>
#include <cstdlib>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/json.hpp>

using namespace trading;
namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

// =============================================================================
// trading-core 主程序 (main.cpp)
// -----------------------------------------------------------------------------
// 职责：读取 .env → 初始化风控/行情/策略/下单 → 主循环输出 JSON 给 dashboard_bridge
// 模块概览：
//   1. 链上余额查询        fetch_usdc_balance*
//   2. 配置加载            load_env / env_flag_
//   3. 实盘余额同步        sync_live_balance
//   4. 到期自动赎回        attempt_onchain_redeem_async
//   5. Legacy sim helpers (unused in live-only build)        apply_paper_slippage / paper_hedge_liquidity_miss
//   6. 市场结算定价        try_binary_settlement_prices / official_settlement_prices
//   7. LIH/DH 到期平仓     check_and_close_lih/dh_positions
//   8. Web 热更新配置      apply_runtime_config
//   9. main() 启动与主循环  见下方分段注释
// =============================================================================

// --- 1. 链上余额：通过 Polygon RPC 读取 ERC20 余额（pUSD / USDC.e / USDC）---
// Query USDC balance from Polygon RPC (on-chain)
double fetch_usdc_balance_for_contract(const std::string& funder_address, const std::string& usdc_contract, const std::string& label) {
    try {
        std::string addr = funder_address;
        if (addr.substr(0, 2) == "0x") addr = addr.substr(2);
        // Lowercase the address for consistency
        std::transform(addr.begin(), addr.end(), addr.begin(), ::tolower);
        std::string padded_addr = std::string(64 - addr.size(), '0') + addr;
        std::string call_data = "0x70a08231" + padded_addr;

        boost::json::object rpc_req;
        rpc_req["jsonrpc"] = "2.0";
        rpc_req["method"] = "eth_call";
        rpc_req["id"] = 1;
        boost::json::object call_obj;
        call_obj["to"] = usdc_contract;
        call_obj["data"] = call_data;
        rpc_req["params"] = boost::json::array{call_obj, "latest"};
        std::string body = boost::json::serialize(rpc_req);

        net::io_context ioc;
        ssl::context ctx{ssl::context::sslv23_client};
        ctx.set_default_verify_paths();
        tcp::resolver resolver(ioc);
        beast::ssl_stream<beast::tcp_stream> stream(ioc, ctx);
        
        if (!SSL_set_tlsext_host_name(stream.native_handle(), "polygon-rpc.com")) {
            return -1;
        }
        auto const results = resolver.resolve("polygon-rpc.com", "443");
        beast::get_lowest_layer(stream).connect(results);
        stream.handshake(ssl::stream_base::client);

        http::request<http::string_body> req{http::verb::post, "/", 11};
        req.set(http::field::host, "polygon-rpc.com");
        req.set(http::field::content_type, "application/json");
        req.body() = body;
        req.prepare_payload();
        http::write(stream, req);

        beast::flat_buffer buffer;
        http::response<http::string_body> res;
        http::read(stream, buffer, res);

        auto jv = boost::json::parse(res.body());
        auto& obj = jv.as_object();
        
        // Check for RPC errors
        if (obj.contains("error")) {
            spdlog::warn("RPC error for {}: {}", label, res.body());
            beast::error_code ec;
            stream.shutdown(ec);
            return -1;
        }
        
        if (!obj.contains("result")) {
            spdlog::warn("RPC response missing 'result' for {}: {}", label, res.body());
            beast::error_code ec;
            stream.shutdown(ec);
            return -1;
        }
        
        std::string hex_result = std::string(obj.at("result").as_string());
        
        // Handle "0x" or empty results
        if (hex_result.empty() || hex_result == "0x" || hex_result == "0x0") {
            beast::error_code ec;
            stream.shutdown(ec);
            return 0.0;
        }
        
        unsigned long long raw_balance = std::stoull(hex_result, nullptr, 16);
        double balance = static_cast<double>(raw_balance) / 1000000.0;
        
        beast::error_code ec;
        stream.shutdown(ec);
        return balance;
    } catch (const std::exception& e) {
        spdlog::error("Failed to fetch {} balance: {}", label, e.what());
        return -1;
    }
}

double fetch_usdc_balance(const std::string& funder_address) {
    // pUSD (V2 collateral), USDC.e (legacy), native USDC
    double bal_pusd = fetch_usdc_balance_for_contract(funder_address, "0xc011a7e12a19f7b1f670d46f03b03f3342e82dfb", "pUSD");
    if (bal_pusd >= 0) {
        spdlog::info("pUSD balance: ${:.2f}", bal_pusd);
    }

    double bal = fetch_usdc_balance_for_contract(funder_address, "0x2791Bca1f2de4661ED88A30C99A7a9449Aa84174", "USDC.e");
    if (bal >= 0) {
        spdlog::info("USDC.e balance: ${:.2f}", bal);
    }

    double bal2 = fetch_usdc_balance_for_contract(funder_address, "0x3c499c542cEF5E3811e1192ce70d8cC03d5c3359", "USDC");
    if (bal2 >= 0) {
        spdlog::info("USDC (native) balance: ${:.2f}", bal2);
    }

    double total = 0;
    if (bal_pusd >= 0) total += bal_pusd;
    if (bal >= 0) total += bal;
    if (bal2 >= 0) total += bal2;

    if (bal_pusd < 0 && bal < 0 && bal2 < 0) return -1;
    return total;
}

// --- 2. 配置加载：解析项目根目录 .env（忽略 # 注释行）---
std::unordered_map<std::string, std::string> load_env(const std::string& filepath) {
    std::unordered_map<std::string, std::string> env;
    std::ifstream file(filepath);
    if (!file.is_open()) {
        spdlog::warn("Could not open {} - using defaults", filepath);
        return env;
    }
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto pos = line.find('=');
        if (pos != std::string::npos) {
            std::string key = line.substr(0, pos);
            std::string val = line.substr(pos + 1);
            key.erase(0, key.find_first_not_of(" \t\r\n"));
            key.erase(key.find_last_not_of(" \t\r\n") + 1);
            val.erase(0, val.find_first_not_of(" \t\r\n"));
            val.erase(val.find_last_not_of(" \t\r\n") + 1);
            if (val.size() >= 2 && ((val.front() == '"' && val.back() == '"') || (val.front() == '\'' && val.back() == '\''))) {
                val = val.substr(1, val.size() - 2);
            }
            env[key] = val;
        }
    }
    return env;
}

// --- 3. 自动赎回去重：同一 condition_id 只触发一次链上 redeem ---
static std::unordered_set<std::string> g_redeem_triggered;
static std::mutex g_redeem_mutex;

// 解析 .env 布尔值（true/false/1/0/yes/no/on/off）
static bool env_flag_true(const std::unordered_map<std::string, std::string>& env, const std::string& key, bool default_val) {
    auto it = env.find(key);
    if (it == env.end()) return default_val;
    std::string v = it->second;
    std::transform(v.begin(), v.end(), v.begin(), ::tolower);
    if (v == "false" || v == "0" || v == "no" || v == "off") return false;
    if (v == "true" || v == "1" || v == "yes" || v == "on") return true;
    return default_val;
}

static int env_int(const std::unordered_map<std::string, std::string>& env, const std::string& key,
                   int default_val, int min_v, int max_v) {
    auto it = env.find(key);
    if (it == env.end()) return default_val;
    try {
        int v = std::stoi(it->second);
        return std::max(min_v, std::min(max_v, v));
    } catch (...) {
        return default_val;
    }
}

// All popen("python …") helpers must use project .venv — system python3 lacks web3/dotenv/clob.
static std::string g_python_bin;

static void init_python_bin(const std::unordered_map<std::string, std::string>& env) {
    if (env.count("VENV_PYTHON") && !env.at("VENV_PYTHON").empty()) {
        g_python_bin = env.at("VENV_PYTHON");
    } else {
#ifdef _WIN32
        g_python_bin = ".venv\\Scripts\\python.exe";
#else
        g_python_bin = ".venv/bin/python3";
#endif
    }
    if (!std::filesystem::exists(g_python_bin)) {
        spdlog::warn("Python bin not found at {} — falling back to PATH python", g_python_bin);
#ifdef _WIN32
        g_python_bin = "python";
#else
        g_python_bin = "python3";
#endif
    } else {
        spdlog::info("Python helper bin: {}", g_python_bin);
    }
}

static std::string python_script_cmd(const std::string& script, const std::string& script_args = "",
                                     bool merge_stderr = true) {
    std::string cmd = g_python_bin + " " + script;
    if (!script_args.empty()) cmd += " " + script_args;
#ifndef _WIN32
    cmd += merge_stderr ? " 2>&1" : " 2>/dev/null";
#endif
    return cmd;
}

static std::string popen_read_first_line(const std::string& cmd) {
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    char buf[512];
    std::string out;
    if (fgets(buf, sizeof(buf), pipe)) out = buf;
    pclose(pipe);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

static std::string popen_read_all(const std::string& cmd) {
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    std::string output;
    char buf[512];
    while (fgets(buf, sizeof(buf), pipe)) output += buf;
    pclose(pipe);
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) output.pop_back();
    return output;
}

static bool verify_venv_web3() {
    const std::string out = popen_read_first_line(python_script_cmd(
        "-c",
        "\"import web3; "
        "from web3.middleware import ExtraDataToPOAMiddleware; "
        "print('ok')\""));
    if (out == "ok") {
        spdlog::info("venv web3 OK ({})", g_python_bin);
        return true;
    }
    spdlog::critical(
        "venv web3 MISSING ({}) — output: {} | fix: .venv/bin/pip install 'web3>=6,<8'",
        g_python_bin, out.empty() ? "(empty)" : out);
    return false;
}

// --- 4. 实盘余额同步：调用 fetch_balance.py 刷新 RiskManager 当前余额 ---
static void sync_live_balance(risk::RiskManager& risk_manager) {
    const std::string out = popen_read_first_line(python_script_cmd("fetch_balance.py", "", false));
    if (out.empty()) return;
    try {
        double new_bal = std::stod(out);
        if (new_bal > 0) risk_manager.update_balance(new_bal);
    } catch (...) {}
}

// --- 5. 到期自动赎回：后台线程调用 redeem_positions.py 把已结算仓位换回 USDC ---
static void attempt_onchain_redeem_async(
    const std::string& condition_id,
    const std::string& dh_id,
    bool neg_risk,
    StateStore& store,
    risk::RiskManager& risk_manager
) {
    if (condition_id.empty() || condition_id.size() < 10) {
        spdlog::warn("Redeem skipped for {} — missing condition_id", dh_id);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_redeem_mutex);
        if (g_redeem_triggered.count(condition_id)) return;
        g_redeem_triggered.insert(condition_id);
    }

    std::thread([condition_id, dh_id, neg_risk, &store, &risk_manager]() {
        spdlog::info("AUTO-REDEEM starting | {} | condition {}", dh_id, condition_id.substr(0, 18));
        store.push_telemetry(fmt::format("REDEEM START {} | {}", dh_id, condition_id.substr(0, 18)));

        const std::string neg_flag = neg_risk ? "true" : "false";
        const std::string cmd = python_script_cmd(
            "redeem_positions.py",
            "\"" + condition_id + "\" " + neg_flag);
        const std::string output = popen_read_all(cmd);
        if (output.empty()) {
            spdlog::error("AUTO-REDEEM popen failed for {} | cmd={}", dh_id, cmd);
            {
                std::lock_guard<std::mutex> lock(g_redeem_mutex);
                g_redeem_triggered.erase(condition_id);
            }
            return;
        }

        try {
            auto jv = boost::json::parse(output);
            auto obj = jv.as_object();
            bool ok = obj.contains("success") && obj.at("success").as_bool();
            std::string msg = obj.contains("message") ? std::string(obj.at("message").as_string()) : "";
            std::string tx = obj.contains("tx_hash") && obj.at("tx_hash").is_string()
                ? std::string(obj.at("tx_hash").as_string()) : "";

            if (ok) {
                spdlog::info("AUTO-REDEEM OK | {} | tx {}", dh_id, tx.empty() ? "n/a" : tx.substr(0, 20));
                store.push_telemetry(fmt::format("REDEEM OK {} | tx {}", dh_id, tx.empty() ? "n/a" : tx.substr(0, 18)));
                sync_live_balance(risk_manager);
            } else {
                spdlog::warn("AUTO-REDEEM skipped/failed | {} | {}", dh_id, msg);
                store.push_telemetry(fmt::format("REDEEM FAIL {} | {}", dh_id, msg));
                {
                    std::lock_guard<std::mutex> lock(g_redeem_mutex);
                    g_redeem_triggered.erase(condition_id);
                }
            }
        } catch (const std::exception& e) {
            spdlog::error("AUTO-REDEEM parse error | {} | raw: {}", dh_id, output.substr(0, 200));
            {
                std::lock_guard<std::mutex> lock(g_redeem_mutex);
                g_redeem_triggered.erase(condition_id);
            }
        }
    }).detach();
}

// --- 6. Legacy sim helpers (slippage / miss probability) ---
static double apply_paper_slippage(double price, bool is_buy, double slip_pct) {
    if (slip_pct <= 0.0 || price <= 0.0) return price;
    return is_buy ? price * (1.0 + slip_pct) : price * (1.0 - slip_pct);
}

static bool paper_hedge_liquidity_miss(const std::string& token_id, double now_sec, double rate) {
    if (rate <= 0.0) return false;
    const auto bucket = static_cast<int64_t>(now_sec);
    const std::string seed = token_id + "|" + std::to_string(bucket);
    const size_t h = std::hash<std::string>{}(seed);
    return (h % 10000) < static_cast<size_t>(rate * 10000.0 + 0.5);
}

static double paper_action_extra_slip(const StateStore& store, const LegInAction& act) {
    if (!store.paper_realism_enabled()) return 0.0;
    if (act.kind == LegInAction::Kind::OpenLeg1) {
        return store.paper_leg1_extra_slip_pct();
    }
    if (act.kind == LegInAction::Kind::CompleteHedge) {
        double extra = store.paper_hedge_extra_slip_pct();
        if (act.note.find("force") != std::string::npos) {
            extra += store.paper_force_extra_slip_pct();
        }
        return extra;
    }
    return 0.0;
}

// 从 .env 读取 double，解析失败则返回 fallback
static double env_double_or(const std::unordered_map<std::string, std::string>& env,
                            const char* key, double fallback) {
    auto it = env.find(key);
    if (it == env.end() || it->second.empty()) return fallback;
    try {
        return std::stod(it->second);
    } catch (...) {
        return fallback;
    }
}

// --- 7. 市场结算：窗口到期后确定 YES/NO 兑付价（官方结算 > 盘口 bid > 0.5/0.5 兜底）---
static std::optional<std::pair<double, double>> try_binary_settlement_prices(
    GammaClient& gamma,
    StateStore& store,
    const std::string& condition_id,
    const std::string& yes_token_id,
    const std::string& no_token_id,
    const std::string& asset_label) {
    if (!condition_id.empty()) {
        if (auto out = gamma.fetch_settlement_outcomes(condition_id)) {
            if (out->resolved && out->yes_payout + out->no_payout == 1.0) {
                spdlog::info("{} settle {} | official YES={:.0f} NO={:.0f}",
                             asset_label, condition_id.substr(0, 12),
                             out->yes_payout, out->no_payout);
                return {{out->yes_payout, out->no_payout}};
            }
        }
    }

    auto mark = [&](const std::string& tid) -> std::optional<double> {
        if (auto b = store.get_official_mark_bid(tid); b && *b > 0.0) return b;
        if (auto b = store.get_token_bid(tid); b && b->price > 0.0) return b->price;
        return gamma.fetch_token_price(tid, "SELL");
    };

    auto yp = mark(yes_token_id);
    auto np = mark(no_token_id);
    if (yp) {
        if (*yp >= 0.85) return {{1.0, 0.0}};
        if (*yp <= 0.15) return {{0.0, 1.0}};
    }
    if (np) {
        if (*np >= 0.85) return {{0.0, 1.0}};
        if (*np <= 0.15) return {{1.0, 0.0}};
    }
    if (yp && np) {
        if (*yp >= 0.65 && *np <= 0.35) return {{1.0, 0.0}};
        if (*np >= 0.65 && *yp <= 0.35) return {{0.0, 1.0}};
    }
    return std::nullopt;
}

static std::pair<double, double> official_settlement_prices(
    GammaClient& gamma,
    StateStore& store,
    const std::string& condition_id,
    const std::string& yes_token_id,
    const std::string& no_token_id,
    const std::string& asset_label) {
    if (auto p = try_binary_settlement_prices(
            gamma, store, condition_id, yes_token_id, no_token_id, asset_label)) {
        return *p;
    }
    spdlog::warn("{} | resolution unknown — marking 0.5/0.5 (hedged fallback)", asset_label);
    return {0.5, 0.5};
}

// --- 8. LIH 到期平仓：已对冲按 1:1 结算；未对冲需等 0/1 官方结果 ---
void check_and_close_lih_positions(
    risk::RiskManager& risk_manager,
    StateStore& store,
    GammaClient& gamma,
    bool auto_redeem_enabled,
    const std::string* live_state_path = nullptr) {
    const double now = std::chrono::duration<double>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    auto open = risk_manager.get_open_lih_positions();
    for (const auto& [id, p] : open) {
        if (now < p.end_date_ts) continue;

        const double matched = std::min(p.yes_shares, p.no_shares);
        const double gap = std::abs(p.yes_shares - p.no_shares);
        // Hedge leg may differ slightly in size (e.g. 10.15 vs 10.00); still treat as hedged.
        const bool fully_hedged = matched >= 1.0 && gap <= 0.5;

        auto prices = try_binary_settlement_prices(
            gamma, store, p.condition_id, p.yes_token_id, p.no_token_id, "LIH " + p.asset);
        if (!prices) {
            if (!fully_hedged) {
                const double overdue = now - p.end_date_ts;
                spdlog::warn("[LIH] {} unhedged settlement deferred ({:.0f}s past expiry) — need 0/1 resolution",
                             p.asset, overdue);
                store.push_telemetry(fmt::format(
                    "[LIH] SETTLE {} deferred | unhedged yes={:.2f} no={:.2f} | awaiting winner",
                    p.asset, p.yes_shares, p.no_shares));
                continue;
            }
            spdlog::warn("[LIH] {} hedged settlement unknown — 0.5/0.5 fallback", p.asset);
            prices = {{0.5, 0.5}};
        }

        const auto [ey, en] = *prices;
        const bool is_live = !p.paper_mode;
        const std::string condition_id = p.condition_id;
        if (fully_hedged) {
            const double proceeds = matched * 1.0;
            const double cost = p.yes_cost + p.no_cost;
            if (risk_manager.register_lih_close(id, ey, en, "Market resolved (hedged)", now)) {
                store.push_telemetry(fmt::format(
                    "[LIH LIVE] CLOSED {} | {} hedged {:.2f}sh | PnL ~${:+.2f} | YES={:.0f} NO={:.0f}",
                    id, p.asset, matched, proceeds - cost, ey, en));
                if (live_state_path && !live_state_path->empty()) {
                    persistence::save_live_lih_state(risk_manager, *live_state_path, false);
                }
            }
        } else {
            risk_manager.register_lih_close(id, ey, en, "Market resolved (unhedged)", now);
            store.push_telemetry(fmt::format(
                "[LIH LIVE] CLOSED {} | {} UNHEDGED | yes={:.2f} no={:.2f} | YES={:.0f} NO={:.0f}",
                id, p.asset, p.yes_shares, p.no_shares, ey, en));
            if (live_state_path && !live_state_path->empty()) {
                persistence::save_live_lih_state(risk_manager, *live_state_path, false);
            }
        }
        if (is_live && auto_redeem_enabled && !condition_id.empty()) {
            attempt_onchain_redeem_async(condition_id, id, p.is_neg_risk, store, risk_manager);
        }
    }
}

// --- 9. DH 到期平仓：双边持仓按结算价 register_dh_close，实盘可选 auto-redeem ---
void check_and_close_dh_positions(
    risk::RiskManager& risk_manager,
    StateStore& store,
    GammaClient& gamma,
    bool auto_redeem_enabled
) {
    auto now = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();

    auto open_dh = risk_manager.get_open_dh_positions();
    for (const auto& [id, p] : open_dh) {
        if (now < p.end_date_ts) continue;

        const bool is_live = !p.paper_mode;
        auto [ey, en] = official_settlement_prices(
            gamma, store, p.condition_id, p.yes_token_id, p.no_token_id, "DH " + p.asset);

        risk_manager.register_dh_close(id, ey, en, "EXPIRED");
        store.push_telemetry(fmt::format(
            "SETTLED {} DH @ YES={:.0f} NO={:.0f} | {}",
            p.asset, ey, en, p.market_question));

        if (is_live && auto_redeem_enabled && !p.condition_id.empty()) {
            attempt_onchain_redeem_async(p.condition_id, id, p.is_neg_risk, store, risk_manager);
        } else if (is_live && p.condition_id.empty()) {
            spdlog::warn("Live DH {} closed without condition_id — cannot auto-redeem", id);
        }
    }
}

// 解析 Web/bridge 写入的布尔配置字符串
static bool parse_config_bool(const std::string& v) {
    std::string lower = v;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    return !(lower == "false" || lower == "0" || lower == "no" || lower == "off");
}

// 按资产/窗口粒度开关 DH 市场（DH_ENABLE_5M_BTC 等）
static bool apply_dh_asset_config(StateStore& store, const std::string& k, const std::string& v) {
    const bool enabled = parse_config_bool(v);
    if (k == "DH_ENABLE_5M_BTC") {
        store.set_dh_asset_enabled(5, "btc", enabled);
        store.push_telemetry(fmt::format("CONFIG DH_ENABLE_5M_BTC={}", enabled ? "true" : "false"));
        return true;
    }
    if (k == "DH_ENABLE_5M_ETH") {
        store.set_dh_asset_enabled(5, "eth", enabled);
        store.push_telemetry(fmt::format("CONFIG DH_ENABLE_5M_ETH={}", enabled ? "true" : "false"));
        return true;
    }
    if (k == "DH_ENABLE_5M_SOL") {
        store.set_dh_asset_enabled(5, "sol", enabled);
        store.push_telemetry(fmt::format("CONFIG DH_ENABLE_5M_SOL={}", enabled ? "true" : "false"));
        return true;
    }
    if (k == "DH_ENABLE_15M_BTC") {
        store.set_dh_asset_enabled(15, "btc", enabled);
        store.push_telemetry(fmt::format("CONFIG DH_ENABLE_15M_BTC={}", enabled ? "true" : "false"));
        return true;
    }
    if (k == "DH_ENABLE_15M_ETH") {
        store.set_dh_asset_enabled(15, "eth", enabled);
        store.push_telemetry(fmt::format("CONFIG DH_ENABLE_15M_ETH={}", enabled ? "true" : "false"));
        return true;
    }
    return false;
}

// 实盘 LIH 快照路径（供 reload_lih_state 控制指令使用）
static std::string g_live_state_reload_path;

// 链上持仓对齐：后台跑 live_lih_reconcile.py 并 reload 快照
// fast_positions_only=true → --positions-only（仅 Data API 持仓，~1s，成交后触发）
// fast_positions_only=false → --merge（成交历史 + 持仓，兜底）
static void try_live_chain_reconcile_async(
    risk::RiskManager& risk_manager,
    const std::string& live_path,
    bool fast_positions_only = false) {
    static std::atomic<bool> running{false};
    static std::atomic<int64_t> last_reconcile_ms{0};
    constexpr int64_t kMinGapMs = 2500;
    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (now_ms - last_reconcile_ms.load() < kMinGapMs) return;
    if (running.exchange(true)) return;
    last_reconcile_ms.store(now_ms);
    const std::string script_args = fast_positions_only ? "--positions-only" : "--merge";
    std::thread([&risk_manager, live_path, script_args]() {
        const std::string cmd = python_script_cmd("scripts/live_lih_reconcile.py", script_args);
        const int rc = std::system(cmd.c_str());
        if (rc == 0) {
            if (persistence::load_live_lih_state(risk_manager, live_path, false)) {
                spdlog::info("Chain reconcile ({}) reloaded {}", script_args, live_path);
            }
        } else {
            spdlog::warn("Chain reconcile {} failed (exit {})", script_args, rc);
        }
        running.store(false);
    }).detach();
}

// --- 10. Web 热更新：读取 logs/runtime_config.json，应用 pause/resume/参数 patch 后删除 ---
static void apply_runtime_config(
    const std::string& path,
    risk::RiskManager& risk_manager,
    StateStore& store,
    std::mutex& detector_mutex,
    std::unique_ptr<DumpHedgeDetector>& dh_detector,
    std::unique_ptr<LegInHedgeDetector>& lih_detector
) {
    std::ifstream file(path);
    if (!file.is_open()) return;

    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();

    boost::json::value jv;
    try {
        jv = boost::json::parse(content);
    } catch (const std::exception& e) {
        spdlog::warn("Runtime config parse error: {}", e.what());
        std::remove(path.c_str());
        return;
    }

    auto& obj = jv.as_object();

    // control：pause / resume / reset_kill / reload_lih_state / reset_lih_session
    if (obj.contains("control") && obj.at("control").is_string()) {
        std::string action = std::string(obj.at("control").as_string());
        std::string reason = obj.contains("reason") ? std::string(obj.at("reason").as_string()) : "Web control";
        if (action == "pause") {
            risk_manager.pause(reason);
            store.push_telemetry(fmt::format("CONFIG PAUSE | {}", reason));
        } else if (action == "resume") {
            if (std::filesystem::exists("logs/STOP_TRADING")) {
                std::error_code ec;
                std::filesystem::remove("logs/STOP_TRADING", ec);
                if (ec) {
                    store.push_telemetry("CONFIG RESUME blocked | cannot clear STOP_TRADING");
                    spdlog::warn("Resume blocked: failed to clear logs/STOP_TRADING ({})", ec.message());
                } else {
                    spdlog::info("Resume: cleared logs/STOP_TRADING (explicit Web resume)");
                }
            }
            if (std::filesystem::exists("logs/STOP_TRADING")) {
                // Still present after remove attempt — do not resume.
            } else if (risk_manager.resume()) {
                risk_manager.reset_lih_session();
                if (risk_manager.get_max_concurrent_positions() <= 0) {
                    const auto env_now = load_env(".env");
                    if (env_now.count("RISK_MAX_CONCURRENT_POSITIONS")) {
                        const int env_max = std::stoi(env_now.at("RISK_MAX_CONCURRENT_POSITIONS"));
                        if (env_max > 0) {
                            risk_manager.set_max_concurrent_positions(env_max);
                            store.push_telemetry(
                                fmt::format("CONFIG RESUME | restored RISK_MAX_CONCURRENT_POSITIONS={}", env_max));
                        }
                    }
                }
                const std::string msg = risk_manager.get_lih_pause_after_round()
                    ? "CONFIG RESUME | trading enabled, LIH session reset (debug pause mode)"
                    : "CONFIG RESUME | trading enabled, LIH session reset";
                store.push_telemetry(msg);
            }
        } else if (action == "reset_kill") {
            if (risk_manager.reset_kill_switch(true)) {
                store.push_telemetry("CONFIG RESET_KILL | kill switch cleared");
            }
        } else if (action == "reload_lih_state") {
            if (store.live_lih_dry_run()) {
                store.push_telemetry("CONFIG reload_lih_state skipped | shadow mode");
                spdlog::info("reload_lih_state skipped in shadow mode");
            } else if (!g_live_state_reload_path.empty() &&
                persistence::load_live_lih_state(risk_manager, g_live_state_reload_path, false)) {
                store.push_telemetry("CONFIG reload_lih_state | live LIH snapshot reloaded");
            }
        } else if (action == "reset_lih_session") {
            risk_manager.reset_lih_session();
            store.push_telemetry("CONFIG reset_lih_session | leg counter cleared");
        }
    }

    // patch：Web 策略页保存的 .env 热更新项（风控 / DH / LIH / 资产开关）
    if (obj.contains("patch") && obj.at("patch").is_object()) {
        const auto& patch = obj.at("patch").as_object();
        double sum_target = store.get_dh_sum_target();
        double min_discount = store.get_dh_min_discount();
        double cooldown = store.get_dh_cooldown_seconds();
        double min_secs = store.get_dh_min_seconds_remaining();
        bool dh_changed = false;

        for (const auto& [key, val] : patch) {
            if (!val.is_string()) continue;
            std::string k = std::string(key);
            std::string v = std::string(val.as_string());
            try {
                if (k == "RISK_MAX_POSITION_FRACTION") {
                    risk_manager.set_max_position_fraction(std::stod(v));
                    store.push_telemetry(fmt::format("CONFIG RISK_MAX_POSITION_FRACTION={}", v));
                } else if (k == "RISK_DAILY_LOSS_LIMIT") {
                    risk_manager.set_daily_loss_limit(std::stod(v));
                    store.push_telemetry(fmt::format("CONFIG RISK_DAILY_LOSS_LIMIT={}", v));
                } else if (k == "RISK_TOTAL_DRAWDOWN_KILL") {
                    risk_manager.set_total_drawdown_kill(std::stod(v));
                    store.push_telemetry(fmt::format("CONFIG RISK_TOTAL_DRAWDOWN_KILL={}", v));
                } else if (k == "RISK_MAX_CONCURRENT_POSITIONS") {
                    risk_manager.set_max_concurrent_positions(std::stoi(v));
                    store.push_telemetry(fmt::format("CONFIG RISK_MAX_CONCURRENT_POSITIONS={}", v));
                } else if (k == "FEE_RATE") {
                    double fr = std::stod(v);
                    risk_manager.set_fee_rate(fr);
                    store.set_fee_rate(fr);
                    store.push_telemetry(fmt::format("CONFIG FEE_RATE={}", v));
                } else if (k == "DH_SUM_TARGET") {
                    sum_target = std::stod(v);
                    dh_changed = true;
                } else if (k == "DH_MIN_DISCOUNT") {
                    min_discount = std::stod(v);
                    dh_changed = true;
                } else if (k == "DH_COOLDOWN_SECONDS") {
                    cooldown = std::stod(v);
                    dh_changed = true;
                } else if (k == "DH_MIN_SECONDS_REMAINING") {
                    min_secs = std::stod(v);
                    dh_changed = true;
                } else if (k == "BINANCE_FEED_ENABLED") {
                    bool enabled = parse_config_bool(v);
                    store.set_binance_feed_enabled(enabled);
                    store.push_telemetry(fmt::format("CONFIG BINANCE_FEED_ENABLED={}", enabled ? "true" : "false"));
                } else if (k == "DH_ENABLE_5M") {
                    bool enabled = parse_config_bool(v);
                    store.set_dh_window_enabled(enabled, store.dh_enable_15m());
                    store.push_telemetry(fmt::format("CONFIG DH_ENABLE_5M={}", enabled ? "true" : "false"));
                } else if (k == "DH_ENABLE_15M") {
                    bool enabled = parse_config_bool(v);
                    store.set_dh_window_enabled(store.dh_enable_5m(), enabled);
                    store.push_telemetry(fmt::format("CONFIG DH_ENABLE_15M={}", enabled ? "true" : "false"));
                } else if (k == "LIH_MAX_MATCHED_SHARES") {
                    risk_manager.set_lih_max_matched_shares(std::stod(v));
                    store.push_telemetry(fmt::format("CONFIG LIH_MAX_MATCHED_SHARES={}", v));
                } else if (k == "LIH_MAX_USDC_PER_SLOT") {
                    risk_manager.set_lih_max_usdc_per_slot(std::stod(v));
                    store.push_telemetry(fmt::format("CONFIG LIH_MAX_USDC_PER_SLOT={}", v));
                } else if (k == "LIH_MIN_BALANCE_USDC") {
                    risk_manager.set_lih_min_balance_usdc(std::stod(v));
                    store.push_telemetry(fmt::format("CONFIG LIH_MIN_BALANCE_USDC={}", v));
                } else if (k == "LIH_PAUSE_AFTER_ROUND") {
                    const bool enabled = parse_config_bool(v);
                    risk_manager.set_lih_pause_after_round(enabled);
                    store.push_telemetry(fmt::format("CONFIG LIH_PAUSE_AFTER_ROUND={}", enabled ? "true" : "false"));
                } else if (k == "LIH_LEG1_MAX_PRICE") {
                    const double x = std::stod(v);
                    store.set_lih_config(x, store.lih_target_combined(), store.lih_use_mirror());
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_leg1_max_price(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_LEG1_MAX_PRICE={}", v));
                } else if (k == "LIH_TARGET_COMBINED") {
                    const double x = std::stod(v);
                    store.set_lih_config(store.lih_leg1_max_price(), x, store.lih_use_mirror());
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_target_combined(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_TARGET_COMBINED={}", v));
                } else if (k == "LIH_USE_MIRROR") {
                    const bool enabled = parse_config_bool(v);
                    store.set_lih_config(store.lih_leg1_max_price(), store.lih_target_combined(), enabled);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_use_mirror_prices(enabled);
                    store.push_telemetry(fmt::format("CONFIG LIH_USE_MIRROR={}", enabled ? "true" : "false"));
                } else if (k == "LIH_COOLDOWN_SECONDS") {
                    const double x = std::stod(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_leg1_cooldown_seconds(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_COOLDOWN_SECONDS={} (leg1 alias)", v));
                } else if (k == "LIH_LEG1_COOLDOWN_SECONDS") {
                    const double x = std::stod(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_leg1_cooldown_seconds(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_LEG1_COOLDOWN_SECONDS={}", v));
                } else if (k == "LIH_REBALANCE_COOLDOWN_SECONDS") {
                    const double x = std::stod(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_rebalance_cooldown_seconds(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_REBALANCE_COOLDOWN_SECONDS={}", v));
                } else if (k == "LIH_MIN_SECONDS_REMAINING") {
                    const double x = std::stod(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_min_seconds_remaining(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_MIN_SECONDS_REMAINING={}", v));
                } else if (k == "LIH_LEG1_MIN_SECONDS_REMAINING") {
                    const double x = std::stod(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_leg1_min_seconds_remaining(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_LEG1_MIN_SECONDS_REMAINING={}", v));
                } else if (k == "LIH_LEG1_START_DELAY_SEC") {
                    const double x = std::stod(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_leg1_start_delay_sec(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_LEG1_START_DELAY_SEC={}", v));
                } else if (k == "LIH_LEG1_SHARES") {
                    const double x = std::stod(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_leg1_shares(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_LEG1_SHARES={}", v));
                } else if (k == "LIH_ALLOW_OVER_TARGET") {
                    const bool enabled = parse_config_bool(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_allow_over_target(enabled);
                    store.push_telemetry(fmt::format("CONFIG LIH_ALLOW_OVER_TARGET={}", enabled ? "true" : "false"));
                } else if (k == "LIH_FORCE_BALANCE_SECS") {
                    const double x = std::stod(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_force_balance_secs(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_FORCE_BALANCE_SECS={}", v));
                } else if (k == "LIH_LEG1_TREND_ALIGN") {
                    const bool enabled = parse_config_bool(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_leg1_trend_align(enabled);
                    store.push_telemetry(fmt::format("CONFIG LIH_LEG1_TREND_ALIGN={}", enabled ? "true" : "false"));
                } else if (k == "LIH_LEG1_MODE") {
                    std::string mode = v;
                    std::transform(mode.begin(), mode.end(), mode.begin(), ::tolower);
                    const bool trend = (mode == "trend" || mode == "expensive");
                    store.set_lih_leg1_mode(trend ? "trend" : "cheap");
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_leg1_trend_mode(trend);
                    store.push_telemetry(fmt::format("CONFIG LIH_LEG1_MODE={}", trend ? "trend" : "cheap"));
                } else if (k == "LIH_LEG1_TREND_MAX_PRICE") {
                    const double x = std::stod(v);
                    store.set_lih_leg1_trend_max_price(x);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_leg1_trend_max_price(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_LEG1_TREND_MAX_PRICE={}", v));
                } else if (k == "LIH_TREND_LOOKBACK_SEC") {
                    const double x = std::stod(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_trend_lookback_sec(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_TREND_LOOKBACK_SEC={}", v));
                } else if (k == "LIH_ENDGAME_SECS") {
                    const double x = std::stod(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_endgame_secs(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_ENDGAME_SECS={}", v));
                } else if (k == "LIH_ENDGAME_HOLD_ASK") {
                    const double x = std::stod(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_endgame_hold_ask(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_ENDGAME_HOLD_ASK={}", v));
                } else if (k == "LIH_ENDGAME_RESUME_HEDGE_ASK") {
                    const double x = std::stod(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_endgame_resume_hedge_ask(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_ENDGAME_RESUME_HEDGE_ASK={}", v));
                } else if (k == "LIH_ENDGAME_SOFT_CAP") {
                    const double x = std::stod(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_endgame_soft_cap(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_ENDGAME_SOFT_CAP={}", v));
                } else if (k == "LIH_ENDGAME_OVERRIDE_SECS") {
                    const double x = std::stod(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_endgame_override_secs(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_ENDGAME_OVERRIDE_SECS={}", v));
                } else if (k == "LIH_MAX_REBALANCE_SHARES") {
                    const double x = std::stod(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_max_rebalance_shares(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_MAX_REBALANCE_SHARES={}", v));
                } else if (k == "LIH_FLEX_DILUTE_RATIO") {
                    const double x = std::stod(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_flex_dilute_ratio(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_FLEX_DILUTE_RATIO={}", v));
                } else if (k == "LIH_REBALANCE_MODE") {
                    std::string mode = v;
                    std::transform(mode.begin(), mode.end(), mode.begin(), ::tolower);
                    const bool flex = (mode == "flex" || mode == "b");
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_flex_rebalance(flex);
                    store.push_telemetry(fmt::format("CONFIG LIH_REBALANCE_MODE={}", flex ? "flex" : "simple"));
                } else if (apply_dh_asset_config(store, k, v)) {
                }
            } catch (const std::exception& e) {
                spdlog::warn("Failed to apply config {}={}: {}", k, v, e.what());
            }
        }

        if (dh_changed) {
            store.set_dh_config(sum_target, min_discount);
            store.set_dh_timing(cooldown, min_secs);
            std::lock_guard<std::mutex> lock(detector_mutex);
            if (dh_detector) {
                dh_detector->set_sum_target(sum_target);
                dh_detector->set_min_discount(min_discount);
                dh_detector->set_cooldown_seconds(cooldown);
                dh_detector->set_min_seconds_remaining(min_secs);
            }
            store.push_telemetry(fmt::format(
                "CONFIG DH | sum<={:.3f} disc>={:.3f} cd={:.0f}s min_rem={:.0f}s",
                sum_target, min_discount, cooldown, min_secs));
        }
    }

    std::remove(path.c_str());
}

// =============================================================================
// main — 程序入口：初始化 → 启动 Feed 线程 → 250ms 主循环 → stdout JSON 遥测
// =============================================================================
int main() {
    try {
        // --- A. 日志：写入 bot.log ---
        auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>("bot.log", true);
        auto logger = std::make_shared<spdlog::logger>("bot", file_sink);
        spdlog::set_default_logger(logger);
        spdlog::set_level(spdlog::level::debug);
        spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");

        // --- B. 读取 .env：实盘模式、Polymarket 钱包、起始余额 ---
        auto env = load_env(".env");
        init_python_bin(env);
        bool paper_mode = false;
        if (env.count("PAPER_MODE")) {
            std::string pm = env["PAPER_MODE"];
            std::transform(pm.begin(), pm.end(), pm.begin(), ::tolower);
            if (pm == "true" || pm == "1") {
                spdlog::warn("Legacy PAPER_MODE is ignored — live-only build");
            }
        }
        
        std::string polymarket_host = env.count("POLYMARKET_HOST") ? env["POLYMARKET_HOST"] : "https://clob.polymarket.com";
        std::string polymarket_chain_id = env.count("POLYMARKET_CHAIN_ID") ? env["POLYMARKET_CHAIN_ID"] : "137";
        std::string polymarket_signer = env.count("POLYMARKET_SIGNER") ? env["POLYMARKET_SIGNER"] : "";
        std::string polymarket_funder = env.count("POLYMARKET_FUNDER") ? env["POLYMARKET_FUNDER"] : "";
        
        // signer/funder 互为默认；代理钱包模式下两者通常不同，勿留空 SIGNER
        if (polymarket_signer.empty() && !polymarket_funder.empty()) polymarket_signer = polymarket_funder;
        if (polymarket_funder.empty() && !polymarket_signer.empty()) polymarket_funder = polymarket_signer;

        double starting_balance = 0.0;
        if (!verify_venv_web3()) {
            spdlog::critical("[FATAL] Live mode requires web3 in project .venv (see log above)");
            return 1;
        }
        spdlog::info("Fetching Polymarket balance via SDK...");
        const std::string bal_out = popen_read_first_line(
            python_script_cmd("fetch_balance.py", "", false));
        if (!bal_out.empty()) {
            try {
                starting_balance = std::stod(bal_out);
                spdlog::info("Detected Polymarket balance: ${:.2f}", starting_balance);
            } catch (...) {
                spdlog::warn("Could not parse balance output: {}", bal_out);
            }
        }
        if (starting_balance <= 0) {
            spdlog::info("SDK returned $0.00, falling back to on-chain RPC for funder address...");
            starting_balance = fetch_usdc_balance(polymarket_funder);
        }
        if (starting_balance <= 0) {
            spdlog::warn("Polymarket balance is $0.00. Deposit pUSD/USDC to the proxy wallet to trade.");
        }

        std::string polymarket_pk = env.count("POLYMARKET_PRIVATE_KEY") ? env["POLYMARKET_PRIVATE_KEY"] : "";
        if (polymarket_pk.empty() ||
            polymarket_pk == "0x0000000000000000000000000000000000000000000000000000000000000001" ||
            polymarket_pk == "0xYourWalletPrivateKey") {
            spdlog::critical("[FATAL] Live mode requires a valid POLYMARKET_PRIVATE_KEY in .env");
            return 1;
        }
        if (polymarket_funder.empty() || polymarket_funder == "0xYourPolygonWalletAddress") {
            spdlog::critical("[FATAL] Live mode requires POLYMARKET_FUNDER in .env");
            return 1;
        }
        // --- C. EIP-712 签名合约：标准 V2 与 NegRisk（5m/15m Up-Down 用后者）---
        // V2 Exchange addresses (April 2026 migration)
        const std::string V2_EXCHANGE = "0xE111180000d2663C0091e4f400237545B87B996B";
        const std::string V2_NEG_RISK = "0xe2222d279d744050d28e00520010520000310F59";
        
        std::string verifying_contract = V2_EXCHANGE;

        // --- D. 实盘开关：auto-redeem、DH/LIH dry-run、Python CLOB bridge 路径 ---
        bool auto_redeem = !paper_mode && env_flag_true(env, "AUTO_REDEEM", true);
        bool live_dh_dry_run = !paper_mode && env_flag_true(env, "LIVE_DH_DRY_RUN", false);
        bool live_lih_dry_run = !paper_mode && env_flag_true(env, "LIVE_LIH_DRY_RUN", true);
        bool use_python_clob = !paper_mode && env_flag_true(env, "USE_PYTHON_CLOB", true);
        std::string clob_bridge_host = env.count("CLOB_BRIDGE_HOST") ? env["CLOB_BRIDGE_HOST"] : "127.0.0.1";
        int clob_bridge_port = env.count("CLOB_BRIDGE_PORT") ? std::stoi(env["CLOB_BRIDGE_PORT"]) : 8081;
        std::string clob_bridge_path = env.count("CLOB_BRIDGE_PATH") ? env["CLOB_BRIDGE_PATH"] : "/internal/clob/order";
        const int wallet_sync_interval_sec = env_int(env, "WALLET_SYNC_INTERVAL_SEC", 2, 1, 120);
        const int lih_chain_reconcile_sec = env_int(env, "LIH_CHAIN_RECONCILE_SEC", 10, 5, 600);
        const int gamma_market_refresh_sec = env_int(env, "GAMMA_MARKET_REFRESH_SEC", 5, 3, 120);

        spdlog::info("Starting Core v3.0 (LIH) | Mode: {} | Bal: ${:.2f} | Auto-redeem: {} | DH dry-run: {} | LIH dry-run: {} | wallet_sync={}s | gamma_refresh={}s",
                     live_lih_dry_run ? "SHADOW" : "LIVE", starting_balance,
                     auto_redeem ? "on" : "off",
                     live_dh_dry_run ? "on" : "off",
                     live_lih_dry_run ? "on" : "off",
                     wallet_sync_interval_sec, gamma_market_refresh_sec);

        // --- E. 网络 IO 上下文：Feed 线程（Binance/Polymarket WS）与 Gamma REST ---
        boost::asio::io_context feed_ioc;
        boost::asio::ssl::context feed_ctx{boost::asio::ssl::context::sslv23_client};
        feed_ctx.set_default_verify_paths();

        boost::asio::io_context gamma_ioc;
        boost::asio::ssl::context gamma_ctx{boost::asio::ssl::context::sslv23_client};
        gamma_ctx.set_default_verify_paths();

        // --- F. 风控参数（RiskManager）与 DH 结构对冲阈值 ---
        double max_pos = env.count("RISK_MAX_POSITION_FRACTION") ? std::stod(env["RISK_MAX_POSITION_FRACTION"]) : 0.08;
        double daily_loss = env.count("RISK_DAILY_LOSS_LIMIT") ? std::stod(env["RISK_DAILY_LOSS_LIMIT"]) : 0.20;
        double drawdown = env.count("RISK_TOTAL_DRAWDOWN_KILL") ? std::stod(env["RISK_TOTAL_DRAWDOWN_KILL"]) : 0.40;
        int max_concurrent = env.count("RISK_MAX_CONCURRENT_POSITIONS") ? std::stoi(env["RISK_MAX_CONCURRENT_POSITIONS"]) : 3;
        double min_order = env.count("MIN_ORDER_SIZE") ? std::stod(env["MIN_ORDER_SIZE"]) : 5.0;
        double fee_rate = env.count("FEE_RATE") ? std::stod(env["FEE_RATE"]) : 0.018;
        double dh_sum_target = env.count("DH_SUM_TARGET") ? std::stod(env["DH_SUM_TARGET"]) : 0.95;
        double dh_min_discount = env.count("DH_MIN_DISCOUNT") ? std::stod(env["DH_MIN_DISCOUNT"]) : 0.03;
        double dh_cooldown = env.count("DH_COOLDOWN_SECONDS") ? std::stod(env["DH_COOLDOWN_SECONDS"]) : 30.0;
        double dh_min_secs = env.count("DH_MIN_SECONDS_REMAINING") ? std::stod(env["DH_MIN_SECONDS_REMAINING"]) : 60.0;

        // --- G. LIH 分腿对冲参数（leg1 入场 / rebalance / force 配平）---
        bool lih_enabled = env_flag_true(env, "LIH_ENABLED", true);
        if (!paper_mode && lih_enabled && !live_lih_dry_run) {
            spdlog::warn("[LIVE LIH] LIVE_LIH_DRY_RUN=false — real CLOB orders WILL be sent");
        }
        bool lih_use_mirror = env_flag_true(env, "LIH_USE_MIRROR", true);
        double lih_leg1_max = env.count("LIH_LEG1_MAX_PRICE") ? std::stod(env["LIH_LEG1_MAX_PRICE"]) : 0.45;
        double lih_target_combined = env.count("LIH_TARGET_COMBINED") ? std::stod(env["LIH_TARGET_COMBINED"]) : 0.94;
        double lih_min_secs = env.count("LIH_MIN_SECONDS_REMAINING") ? std::stod(env["LIH_MIN_SECONDS_REMAINING"]) : 15.0;
        double lih_leg1_min_secs = env.count("LIH_LEG1_MIN_SECONDS_REMAINING")
            ? std::stod(env["LIH_LEG1_MIN_SECONDS_REMAINING"]) : 30.0;
        double lih_leg1_start_delay = env_double_or(env, "LIH_LEG1_START_DELAY_SEC", 5.0);
        double lih_leg1_cooldown = 20.0;
        double lih_rebalance_cooldown = 5.0;
        if (env.count("LIH_LEG1_COOLDOWN_SECONDS")) {
            lih_leg1_cooldown = std::stod(env["LIH_LEG1_COOLDOWN_SECONDS"]);
        } else if (env.count("LIH_COOLDOWN_SECONDS")) {
            lih_leg1_cooldown = std::stod(env["LIH_COOLDOWN_SECONDS"]);
        }
        if (env.count("LIH_REBALANCE_COOLDOWN_SECONDS")) {
            lih_rebalance_cooldown = std::stod(env["LIH_REBALANCE_COOLDOWN_SECONDS"]);
        }
        double lih_leg1_shares = env.count("LIH_LEG1_SHARES") ? std::stod(env["LIH_LEG1_SHARES"]) : 10.0;
        bool lih_allow_over_target = env_flag_true(env, "LIH_ALLOW_OVER_TARGET", true);
        double lih_force_balance_secs = env.count("LIH_FORCE_BALANCE_SECS")
            ? std::stod(env["LIH_FORCE_BALANCE_SECS"]) : 60.0;
        double lih_max_rebalance_shares = env.count("LIH_MAX_REBALANCE_SHARES")
            ? std::stod(env["LIH_MAX_REBALANCE_SHARES"]) : 0.0;
        double lih_max_matched_shares = env.count("LIH_MAX_MATCHED_SHARES")
            ? std::stod(env["LIH_MAX_MATCHED_SHARES"]) : 50.0;
        double lih_max_usdc_per_slot = env.count("LIH_MAX_USDC_PER_SLOT")
            ? std::stod(env["LIH_MAX_USDC_PER_SLOT"]) : 0.0;
        bool lih_one_slot_global = env_flag_true(env, "LIH_ONE_SLOT_GLOBAL", max_concurrent <= 1);
        int lih_session_max_legs = env.count("LIH_SESSION_MAX_LEGS")
            ? std::stoi(env["LIH_SESSION_MAX_LEGS"]) : 2;
        // Default false: continuous live trading. Set LIH_PAUSE_AFTER_ROUND=true for debug rounds.
        bool lih_pause_after_round = env_flag_true(env, "LIH_PAUSE_AFTER_ROUND", false);
        double lih_min_balance_usdc = env.count("LIH_MIN_BALANCE_USDC")
            ? std::stod(env["LIH_MIN_BALANCE_USDC"]) : 10.0;
        std::string lih_rebalance_mode = env.count("LIH_REBALANCE_MODE") ? env["LIH_REBALANCE_MODE"] : "flex";
        std::transform(lih_rebalance_mode.begin(), lih_rebalance_mode.end(), lih_rebalance_mode.begin(), ::tolower);
        bool lih_flex_rebalance = (lih_rebalance_mode == "flex" || lih_rebalance_mode == "b");
        double lih_flex_dilute_ratio = env.count("LIH_FLEX_DILUTE_RATIO")
            ? std::stod(env["LIH_FLEX_DILUTE_RATIO"]) : 0.95;
        bool lih_leg1_trend_align = env_flag_true(env, "LIH_LEG1_TREND_ALIGN", false);
        double lih_trend_lookback_sec = env.count("LIH_TREND_LOOKBACK_SEC")
            ? std::stod(env["LIH_TREND_LOOKBACK_SEC"]) : 60.0;
        std::string lih_leg1_mode = env.count("LIH_LEG1_MODE") ? env["LIH_LEG1_MODE"] : "cheap";
        std::transform(lih_leg1_mode.begin(), lih_leg1_mode.end(), lih_leg1_mode.begin(), ::tolower);
        const bool lih_leg1_trend_mode = (lih_leg1_mode == "trend" || lih_leg1_mode == "expensive");
        double lih_leg1_trend_max = env_double_or(env, "LIH_LEG1_TREND_MAX_PRICE", 0.65);
        double lih_endgame_secs = env.count("LIH_ENDGAME_SECS")
            ? std::stod(env["LIH_ENDGAME_SECS"]) : 100.0;
        double lih_endgame_hold_ask = env_double_or(env, "LIH_ENDGAME_HOLD_ASK", 0.90);
        double lih_endgame_resume_hedge_ask = env_double_or(env, "LIH_ENDGAME_RESUME_HEDGE_ASK", 0.89);
        double lih_endgame_soft_cap = env_double_or(env, "LIH_ENDGAME_SOFT_CAP", 1.15);
        double lih_endgame_step_small = env_double_or(env, "LIH_ENDGAME_STEP_SHARES_SMALL", 5.0);
        double lih_endgame_step_large = env_double_or(env, "LIH_ENDGAME_STEP_SHARES_LARGE", 10.0);
        double lih_endgame_gap_large = env_double_or(env, "LIH_ENDGAME_GAP_LARGE", 10.0);
        double lih_endgame_override_secs = env_double_or(env, "LIH_ENDGAME_OVERRIDE_SECS", 50.0);
        double lih_endgame_override_cooldown = env_double_or(env, "LIH_ENDGAME_OVERRIDE_COOLDOWN", 2.0);
        std::string mirror_path = env.count("LIVE_MIRROR_PATH") ? env["LIVE_MIRROR_PATH"] : "logs/live_mirror.json";

        const std::string strategy = lih_enabled ? "leg_in" : "dump_hedge";

        // --- H. Market feeds & optional depth/slippage sim (legacy) ---
        bool binance_feed_enabled = true;
        if (env.count("BINANCE_FEED_ENABLED")) {
            std::string bf = env["BINANCE_FEED_ENABLED"];
            std::transform(bf.begin(), bf.end(), bf.begin(), ::tolower);
            binance_feed_enabled = !(bf == "false" || bf == "0" || bf == "no" || bf == "off");
        }
        bool book_aware_detect = env_flag_true(env, "DH_BOOK_AWARE_DETECT", true);
        bool paper_official_book = paper_mode && env_flag_true(env, "PAPER_OFFICIAL_BOOK", true);
        double paper_slippage_pct = 0.0;
        if (paper_mode && env.count("PAPER_SLIPPAGE_PCT")) {
            paper_slippage_pct = std::stod(env["PAPER_SLIPPAGE_PCT"]);
        }
        bool paper_depth_sim = paper_mode && env_flag_true(env, "PAPER_DEPTH_SIM", true);
        bool paper_realism = paper_mode && env_flag_true(env, "PAPER_REALISM_ENABLED", false);
        const double paper_liq_take = env_double_or(env, "PAPER_LIQUIDITY_TAKE_RATIO", 0.35);
        const double paper_min_fill = env_double_or(env, "PAPER_MIN_FILL_RATIO", 0.55);
        const double paper_book_age = env_double_or(env, "PAPER_BOOK_MAX_AGE_SECS", 10.0);
        const double paper_hedge_fail = env_double_or(env, "PAPER_HEDGE_FAIL_RATE", 0.12);
        const double paper_leg1_extra_slip = env_double_or(env, "PAPER_LEG1_EXTRA_SLIP_PCT", 0.008);
        const double paper_hedge_extra_slip = env_double_or(env, "PAPER_HEDGE_EXTRA_SLIP_PCT", 0.012);
        const double paper_force_extra_slip = env_double_or(env, "PAPER_FORCE_EXTRA_SLIP_PCT", 0.03);

        spdlog::info("Strategy: {} | LIH: {} | max_pos={:.0f}% | Binance chart: {} | Book-aware: {}",
                     strategy,
                     lih_enabled ? "on" : "off",
                     max_pos * 100.0,
                     binance_feed_enabled ? "on" : "off",
                     book_aware_detect ? "on" : "off");
        if (paper_mode) {
            spdlog::info("Paper pricing | official CLOB book: {} | slippage: {:.2f}% | depth sim: {}",
                         paper_official_book ? "on" : "off", paper_slippage_pct * 100.0,
                         paper_depth_sim ? "on" : "off");
            if (paper_realism) {
                spdlog::info(
                    "Paper realism | liq_take={:.0f}% min_fill={:.0f}% book_age={:.0f}s "
                    "hedge_miss={:.0f}% leg1+{:.2f}% hedge+{:.2f}% force+{:.2f}%",
                    paper_liq_take * 100.0, paper_min_fill * 100.0, paper_book_age,
                    paper_hedge_fail * 100.0, paper_leg1_extra_slip * 100.0,
                    paper_hedge_extra_slip * 100.0, paper_force_extra_slip * 100.0);
            }
        }
        if (lih_enabled) {
            const std::string max_rebal_str = lih_max_rebalance_shares > 0.0
                ? fmt::format("{:.0f}", lih_max_rebalance_shares)
                : "unlimited";
            const std::string max_matched_str = lih_max_matched_shares > 0.0
                ? fmt::format("{:.0f}", lih_max_matched_shares)
                : "unlimited";
            const std::string slot_cap_str = lih_max_usdc_per_slot > 0.0
                ? fmt::format("${:.2f}", lih_max_usdc_per_slot)
                : "balance×pos_frac";
            spdlog::info(
                "LIH config | leg1_mode={} leg1<={:.2f} trend_max<={:.2f} target<={:.2f} entry={:.1f} "
                "leg1_delay={:.0f}s mode={} dilute={:.2f} "
                "leg1_min={:.0f}s hedge_min={:.0f}s force={:.0f}s trend_align={} lookback={:.0f}s "
                "endgame={:.0f}s hold>={:.2f} soft_cap={:.2f} step={:.0f}/{:.0f} override={:.0f}s "
                "leg1_cd={} rebal_cd={} max_rebal_sh={} max_matched_sh={} slot_cap={} "
                "pause_after_round={} session_legs={}",
                lih_leg1_trend_mode ? "trend" : "cheap",
                lih_leg1_max, lih_leg1_trend_max, lih_target_combined, lih_leg1_shares, lih_leg1_start_delay,
                lih_flex_rebalance ? "flex" : "standard",
                lih_flex_dilute_ratio,
                lih_leg1_min_secs, lih_min_secs,
                lih_force_balance_secs,
                lih_leg1_trend_align ? "on" : "off", lih_trend_lookback_sec,
                lih_endgame_secs, lih_endgame_hold_ask, lih_endgame_soft_cap,
                lih_endgame_step_small, lih_endgame_step_large, lih_endgame_override_secs,
                lih_leg1_cooldown <= 0.0 ? "off" : fmt::format("{:.0f}s", lih_leg1_cooldown),
                lih_rebalance_cooldown <= 0.0 ? "off" : fmt::format("{:.0f}s", lih_rebalance_cooldown),
                max_rebal_str, max_matched_str, slot_cap_str,
                lih_pause_after_round ? "yes" : "no", lih_session_max_legs);
        } else {
            spdlog::info("DH config | sum<={:.2f} disc>={:.2f}", dh_sum_target, dh_min_discount);
        }

        // --- I. CLOB API 凭据（实盘必填，由 derive_and_update_keys.py 生成）---
        std::string poly_api_key = env.count("POLY_API_KEY") ? env["POLY_API_KEY"] : "";
        std::string poly_api_secret = env.count("POLY_API_SECRET") ? env["POLY_API_SECRET"] : "";
        std::string poly_api_passphrase = env.count("POLY_PASSPHRASE") ? env["POLY_PASSPHRASE"] : "";
        std::string neg_risk_exchange = V2_NEG_RISK;

        if (!paper_mode && poly_api_key.empty()) {
            spdlog::critical("[FATAL] Live trading enabled but POLY_API_KEY is missing!");
            spdlog::critical("Please run 'python derive_and_update_keys.py' first to generate API credentials.");
            return 1;
        }

        // --- J. 核心状态：StateStore（遥测/行情缓存）+ RiskManager（仓位/风控）---
        StateStore store;
        store.set_paper_mode(paper_mode);
        if (!paper_mode) {
            store.push_telemetry(fmt::format("💰 BALANCE SYNCED | ${:.2f}", starting_balance));
        }
        risk::RiskManager risk_manager(starting_balance, max_pos, daily_loss, drawdown, max_concurrent, true, 3, 5, 0.02, 300.0, min_order);
        risk_manager.set_fee_rate(fee_rate);
        risk_manager.set_lih_max_matched_shares(lih_max_matched_shares);
        risk_manager.set_lih_max_usdc_per_slot(lih_max_usdc_per_slot);
        risk_manager.set_lih_one_slot_global(lih_one_slot_global);
        risk_manager.set_lih_session_max_legs(lih_session_max_legs);
        risk_manager.set_lih_pause_after_round(lih_pause_after_round);
        risk_manager.set_lih_min_balance_usdc(lih_min_balance_usdc);
        if (!paper_mode && lih_enabled && lih_min_balance_usdc > 0.0 &&
            starting_balance + 1e-6 < lih_min_balance_usdc) {
            spdlog::warn("[LIH] Wallet ${:.2f} below LIH_MIN_BALANCE_USDC=${:.2f} — new leg1 blocked until topped up",
                         starting_balance, lih_min_balance_usdc);
        }

        // --- K. 状态持久化：实盘 live_state.json ---
        std::string live_state_path = env.count("LIVE_STATE_PATH") ? env["LIVE_STATE_PATH"] : "logs/live_state.json";
        g_live_state_reload_path = live_state_path;
        bool live_state_persist = env_flag_true(env, "LIVE_STATE_PERSIST", true);

        if (lih_enabled && live_state_persist) {
            if (persistence::load_live_lih_state(risk_manager, live_state_path, live_lih_dry_run)) {
                if (!live_lih_dry_run) {
                    spdlog::info("Live LIH state loaded from {}", live_state_path);
                }
            } else {
                spdlog::info("Live LIH state: fresh session (no snapshot at {})", live_state_path);
            }
            risk_manager.purge_paper_positions();
        }
        int legacy_la = risk_manager.close_legacy_la_positions();
        if (legacy_la > 0) {
            spdlog::warn("Closed {} legacy LA open position(s) — LA strategy removed", legacy_la);
            store.push_telemetry(fmt::format("LEGACY LA CLOSED | {} position(s)", legacy_la));
        }

        // --- L. 启动保护：任何进程重启默认 PAUSED，仅 Web 手动 resume 可开交易 ---
        {
            std::filesystem::create_directories("logs");
            {
                std::ofstream stop_flag("logs/STOP_TRADING", std::ios::out | std::ios::trunc);
                if (stop_flag) stop_flag << "1\n";
            }
            constexpr const char* kStartupPauseReason =
                "startup — manual Web resume required";
            risk_manager.pause(kStartupPauseReason);
            store.push_telemetry("STARTUP PAUSED | manual Web resume required");
            spdlog::warn("Startup: trading forced PAUSED (restart never auto-trades)");
        }

        // --- M. Push risk/strategy params into StateStore for telemetry ---
        store.set_risk_manager(&risk_manager);
        store.set_fee_rate(fee_rate);
        store.set_strategy(strategy);
        store.set_dh_config(dh_sum_target, dh_min_discount);
        store.set_dh_timing(dh_cooldown, dh_min_secs);
        store.set_dh_window_enabled(
            env_flag_true(env, "DH_ENABLE_5M", true),
            env_flag_true(env, "DH_ENABLE_15M", true));
        store.set_dh_asset_enabled(5, "btc", env_flag_true(env, "DH_ENABLE_5M_BTC", true));
        store.set_dh_asset_enabled(5, "eth", env_flag_true(env, "DH_ENABLE_5M_ETH", true));
        store.set_dh_asset_enabled(5, "sol", env_flag_true(env, "DH_ENABLE_5M_SOL", true));
        store.set_dh_asset_enabled(15, "btc", env_flag_true(env, "DH_ENABLE_15M_BTC", true));
        store.set_dh_asset_enabled(15, "eth", env_flag_true(env, "DH_ENABLE_15M_ETH", true));
        store.set_binance_feed_enabled(binance_feed_enabled);
        store.set_book_aware_detect(book_aware_detect);
        store.set_paper_official_book(paper_official_book);
        store.set_paper_depth_sim(paper_depth_sim);
        store.set_paper_slippage_pct(paper_slippage_pct);
        store.set_paper_realism_enabled(paper_realism);
        store.set_paper_liquidity_take_ratio(paper_liq_take);
        store.set_paper_min_fill_ratio(paper_min_fill);
        store.set_paper_book_max_age_secs(paper_book_age);
        store.set_paper_hedge_fail_rate(paper_hedge_fail);
        store.set_paper_leg1_extra_slip_pct(paper_leg1_extra_slip);
        store.set_paper_hedge_extra_slip_pct(paper_hedge_extra_slip);
        store.set_paper_force_extra_slip_pct(paper_force_extra_slip);
        store.set_lih_enabled(lih_enabled);
        store.set_lih_disable_dh(lih_enabled);
        store.set_lih_config(lih_leg1_max, lih_target_combined, lih_use_mirror);
        store.set_lih_leg1_mode(lih_leg1_trend_mode ? "trend" : "cheap");
        store.set_lih_leg1_trend_max_price(lih_leg1_trend_max);
        store.set_live_lih_dry_run(live_lih_dry_run);
        store.set_mirror_path(mirror_path);
        if (env.count("LIVE_TRADES_BASELINE_TS")) {
            try {
                const double baseline = std::stod(env["LIVE_TRADES_BASELINE_TS"]);
                if (baseline > 0) store.set_trades_baseline_ts(baseline);
            } catch (...) {}
        }

        // --- N. OrderRouter: live CLOB (NegRisk dual signer) ---
        exec::OrderRouter router(feed_ioc, feed_ctx, store, risk_manager, polymarket_host, polymarket_chain_id, verifying_contract, polymarket_pk, polymarket_signer, polymarket_funder, paper_mode, poly_api_key, poly_api_secret, poly_api_passphrase, neg_risk_exchange, live_dh_dry_run, live_lih_dry_run, use_python_clob, clob_bridge_host, clob_bridge_port, clob_bridge_path);

        // --- O. 外部客户端：Gamma（市场列表/结算/REST 兜底）+ Binance WS ---
        GammaClient gamma(gamma_ioc, gamma_ctx);
        std::shared_ptr<BinanceFeed> btc_feed;
        std::shared_ptr<BinanceFeed> eth_feed;
        std::shared_ptr<BinanceFeed> sol_feed;
        if (binance_feed_enabled) {
            btc_feed = std::make_shared<BinanceFeed>(feed_ioc, feed_ctx, store, "btcusdt");
            eth_feed = std::make_shared<BinanceFeed>(feed_ioc, feed_ctx, store, "ethusdt");
            sol_feed = std::make_shared<BinanceFeed>(feed_ioc, feed_ctx, store, "solusdt");
        }

        auto feed_work = boost::asio::make_work_guard(feed_ioc);
        std::thread feed_thread([&feed_ioc]() { feed_ioc.run(); });

        // --- P. 策略检测器：DumpHedge（结构对冲）或 LegInHedge（分腿 LIH）---
        std::mutex detector_mutex;
        std::unique_ptr<DumpHedgeDetector> dh_detector;
        std::unique_ptr<LegInHedgeDetector> lih_detector;

        // LIH actions: OrderRouter on live; legacy local sim path if paper_mode
        auto execute_lih_action = [&](const LegInAction& act, double now_sec) {
            if (!paper_mode) {
                const bool ok = router.submit_lih_action(act, now_sec);
                if (ok && lih_enabled && live_state_persist && !live_lih_dry_run) {
                    persistence::save_live_lih_state(risk_manager, live_state_path, false);
                    try_live_chain_reconcile_async(risk_manager, live_state_path, true);
                }
                return;
            }
            auto slip_buy = [&](double px) {
                return apply_paper_slippage(px, true, paper_slippage_pct);
            };
            constexpr double kLihMinUsdc = 1.0;
            switch (act.kind) {
            case LegInAction::Kind::OpenLeg1: {
                const std::string& tok = act.buy_yes ? act.market.yes_token_id : act.market.no_token_id;
                double shares = act.shares;
                double px = act.price;
                if (store.paper_depth_sim()) {
                    const auto wf = store.walk_ask_fill(tok, act.shares);
                    if (wf.shares <= 0.0 || wf.cost_usdc + 1e-6 < kLihMinUsdc) {
                        if (store.paper_realism_enabled()) {
                            spdlog::info("[PAPER REALISM] LEG1 miss {} | depth/partial fill",
                                         act.market.asset);
                            store.push_telemetry(fmt::format(
                                "[PAPER REALISM] LEG1 miss {} | depth/partial", act.market.asset));
                        }
                        return;
                    }
                    shares = wf.shares;
                    px = wf.avg_price;
                    px = apply_paper_slippage(px, true, paper_action_extra_slip(store, act));
                    spdlog::info("[LIH DEPTH] LEG1 {} {:.2f}/{:.2f}sh avg {:.4f} ({} lvls)",
                                 act.market.asset, shares, act.shares, px, wf.levels_used);
                } else {
                    px = slip_buy(apply_paper_slippage(px, true, paper_action_extra_slip(store, act)));
                }
                const double cost = shares * px;
                if (!risk_manager.can_open_lih_leg(
                        cost, false, nullptr, 0.0, &act.market.asset, act.market.window_minutes).first) {
                    return;
                }
                if (!risk_manager.try_begin_lih_leg1(act.market.asset, act.market.window_minutes)) {
                    return;
                }
                risk_manager.register_lih_open_leg1(act.market, act.buy_yes, px, shares, now_sec);
                store.push_signal(fmt::format("LIH LEG1 {} {} {:.2f}sh @ {:.4f} ({})",
                    act.market.asset, act.buy_yes ? "YES" : "NO", shares, px, act.note));
                break;
            }
            case LegInAction::Kind::CompleteHedge:
            case LegInAction::Kind::HeavyDilute: {
                const std::string& tok = act.buy_yes ? act.market.yes_token_id : act.market.no_token_id;
                if (act.kind == LegInAction::Kind::CompleteHedge
                    && store.paper_realism_enabled()
                    && act.note.find("force") == std::string::npos
                    && paper_hedge_liquidity_miss(tok, now_sec, store.paper_hedge_fail_rate())) {
                    spdlog::info("[PAPER REALISM] hedge miss {} | {}", act.market.asset, act.note);
                    store.push_telemetry(fmt::format(
                        "[PAPER REALISM] hedge miss {} | {}", act.market.asset, act.note));
                    return;
                }
                double shares = act.shares;
                double px = act.price;
                if (store.paper_depth_sim()) {
                    const auto wf = store.walk_ask_fill(tok, act.shares);
                    if (wf.shares <= 0.0 || wf.cost_usdc + 1e-6 < kLihMinUsdc) {
                        if (store.paper_realism_enabled()) {
                            spdlog::info("[PAPER REALISM] {} miss {} | partial {:.2f}/{:.2f}sh",
                                         act.kind == LegInAction::Kind::HeavyDilute ? "DILUTE" : "HEDGE",
                                         act.market.asset, wf.shares, act.shares);
                            store.push_telemetry(fmt::format(
                                "[PAPER REALISM] {} miss {} | partial {:.2f}/{:.2f}sh",
                                act.kind == LegInAction::Kind::HeavyDilute ? "DILUTE" : "HEDGE",
                                act.market.asset, wf.shares, act.shares));
                        }
                        return;
                    }
                    shares = wf.shares;
                    px = wf.avg_price;
                    px = apply_paper_slippage(px, true, paper_action_extra_slip(store, act));
                    spdlog::info("[LIH DEPTH] {} {} {:.2f}/{:.2f}sh avg {:.4f} ({} lvls)",
                                 act.kind == LegInAction::Kind::HeavyDilute ? "HEAVY-DILUTE" : "HEDGE",
                                 act.market.asset, shares, act.shares, px, wf.levels_used);
                } else {
                    px = slip_buy(apply_paper_slippage(px, true, paper_action_extra_slip(store, act)));
                }
                const double cost = shares * px;
                if (!risk_manager.can_open_lih_leg(cost, true, &act.lih_id, shares).first) return;
                if (!risk_manager.try_begin_lih_rebalance(act.lih_id)) return;
                risk_manager.register_lih_add_leg(act.lih_id, act.buy_yes, px, shares);
                const char* tag = act.kind == LegInAction::Kind::HeavyDilute ? "HEAVY-DILUTE" : "HEDGE";
                store.push_signal(fmt::format("LIH {} {} {} {:.2f}sh @ {:.4f} ({})",
                    tag, act.market.asset, act.buy_yes ? "YES" : "NO", shares, px, act.note));
                break;
            }
            case LegInAction::Kind::ScalePaired:
            case LegInAction::Kind::DilutePaired: {
                double shares = act.shares;
                double yes_p = 0.0;
                double no_p = 0.0;
                if (store.paper_depth_sim()) {
                    const auto pf = store.walk_paired_fill(
                        act.market.yes_token_id, act.market.no_token_id,
                        act.shares, risk_manager.get_max_leg_cost_usdc());
                    if (pf.shares <= 0.0 || pf.cost_usdc + 1e-6 < kLihMinUsdc) return;
                    shares = pf.shares;
                    const auto yes_w = store.walk_ask_fill(act.market.yes_token_id, shares);
                    const auto no_w = store.walk_ask_fill(act.market.no_token_id, shares);
                    yes_p = yes_w.avg_price;
                    no_p = no_w.avg_price;
                    if (!risk_manager.can_open_lih_leg(
                            yes_w.cost_usdc + no_w.cost_usdc, true, &act.lih_id, shares).first) return;
                    spdlog::info("[LIH DEPTH] {} {} {:.2f}/{:.2f} paired avg {:.4f}+{:.4f}={:.4f} ({} lvls)",
                                 act.kind == LegInAction::Kind::DilutePaired ? "DILUTE" : "SCALE",
                                 act.market.asset, shares, act.shares, yes_p, no_p, yes_p + no_p,
                                 yes_w.levels_used + no_w.levels_used);
                } else {
                    if (auto y = store.get_official_buy_ask(act.market.yes_token_id)) yes_p = *y;
                    if (auto n = store.get_official_buy_ask(act.market.no_token_id)) no_p = *n;
                    if (yes_p <= 0 || no_p <= 0) {
                        auto yes = store.get_token_price(act.market.yes_token_id);
                        auto no = store.get_token_price(act.market.no_token_id);
                        if (yes) yes_p = yes->price;
                        if (no) no_p = no->price;
                    }
                    yes_p = slip_buy(yes_p);
                    no_p = slip_buy(no_p);
                    if (!risk_manager.can_open_lih_leg(
                            shares * (yes_p + no_p), true, &act.lih_id, shares).first) return;
                }
                if (!risk_manager.try_begin_lih_rebalance(act.lih_id)) return;
                risk_manager.register_lih_add_paired(act.lih_id, yes_p, no_p, shares);
                const char* tag = act.kind == LegInAction::Kind::DilutePaired ? "DILUTE" : "SCALE";
                store.push_signal(fmt::format("LIH {} {} +{:.2f} paired ({})",
                    tag, act.market.asset, shares, act.note));
                break;
            }
            }
        };

        // 每个 Polymarket tick 触发 LIH evaluate（LIH 模式）或 DH evaluate（DH 模式）
        auto try_lih_evaluate = [&]() {
            if (!lih_enabled || !lih_detector) return;
            std::lock_guard<std::mutex> lock(detector_mutex);
            const double now_ms = std::chrono::duration<double, std::milli>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            const double now_sec = now_ms / 1000.0;
            if (auto act = lih_detector->evaluate(now_ms, risk_manager)) {
                execute_lih_action(*act, now_sec);
            }
        };

        auto poly_feed = std::make_shared<PolymarketFeed>(feed_ioc, feed_ctx, store);

        // Polymarket WS 价格推送回调 → LIH/DH 信号检测 → 下单
        poly_feed->set_tick_callback([&](const std::string& /*token_id*/) {
            try_lih_evaluate();

            if (lih_enabled) return;
            if (!dh_detector) return;
            const double now_ms = std::chrono::duration<double, std::milli>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            auto signal = dh_detector->evaluate(now_ms);
            if (!signal) return;

            for (const auto& [id, p] : risk_manager.get_open_dh_positions()) {
                if (p.asset == signal->asset) return;
            }

            double max_allowed_usdc = risk_manager.get_current_balance() * risk_manager.get_max_position_fraction();
            double size_shares = max_allowed_usdc / signal->combined_price;
            if (!risk_manager.can_open_dh_position(signal->combined_price * size_shares).first) return;

            store.push_signal(fmt::format("DH SIGNAL {} | YES:{:.4f} NO:{:.4f} SUM:{:.4f} DISC:{:.1f}%",
                signal->asset, signal->yes_price, signal->no_price,
                signal->combined_price, signal->discount * 100.0));

            DumpHedgeSignal fill_signal = *signal;
            if (paper_mode && paper_slippage_pct > 0.0) {
                fill_signal.yes_price = apply_paper_slippage(fill_signal.yes_price, true, paper_slippage_pct);
                fill_signal.no_price = apply_paper_slippage(fill_signal.no_price, true, paper_slippage_pct);
                fill_signal.combined_price = fill_signal.yes_price + fill_signal.no_price;
                fill_signal.discount = 1.0 - fill_signal.combined_price - store.compute_dh_entry_fee_per_share(
                    fill_signal.yes_price, fill_signal.no_price,
                    fill_signal.yes_token_id, fill_signal.no_token_id);
            }
            if (!router.submit_dump_hedge_order(fill_signal, size_shares)) return;
        });

        // --- Q. 启动行情 Feed（回调注册完成后再 start，避免竞态）---
        // Start feeds only after all callbacks are ready
        if (binance_feed_enabled) {
            btc_feed->start();
            eth_feed->start();
            sol_feed->start();
        }
        poly_feed->start();

        std::atomic<bool> is_refreshing{false};
        std::vector<std::string> rest_poll_tokens;

        // --- R. 市场刷新：Gamma 拉 5m/15m Up-Down 列表 → 重建检测器 → 订阅 token ---
        auto refresh_markets = [&]() {
            if (is_refreshing.exchange(true)) return;
            try {
                std::vector<MarketInfo> all_m;
                auto b5 = gamma.fetch_updown_markets("btc", 5);
                auto e5 = gamma.fetch_updown_markets("eth", 5);
                auto s5 = gamma.fetch_updown_markets("sol", 5);
                auto b15 = gamma.fetch_updown_markets("btc", 15);
                auto e15 = gamma.fetch_updown_markets("eth", 15);
                all_m.insert(all_m.end(), b5.begin(), b5.end());
                all_m.insert(all_m.end(), e5.begin(), e5.end());
                all_m.insert(all_m.end(), s5.begin(), s5.end());
                all_m.insert(all_m.end(), b15.begin(), b15.end());
                all_m.insert(all_m.end(), e15.begin(), e15.end());

                store.update_markets(all_m);
                std::unordered_set<std::string> fee_seen;
                int fee_markets = 0;
                for (const auto& m : all_m) {
                    if (m.condition_id.empty() || fee_seen.count(m.condition_id)) continue;
                    fee_seen.insert(m.condition_id);
                    if (gamma.fetch_and_cache_market_fees(m.condition_id, store)) ++fee_markets;
                }
                {
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (!lih_enabled) {
                        dh_detector = std::make_unique<DumpHedgeDetector>(
                            store, all_m, dh_sum_target, dh_min_discount, dh_min_secs, dh_cooldown);
                        dh_detector->set_fee_rate(fee_rate);
                    }
                    if (lih_enabled) {
                        lih_detector = std::make_unique<LegInHedgeDetector>(
                            store, all_m, lih_leg1_max, lih_target_combined, lih_min_secs,
                            lih_leg1_min_secs, lih_leg1_start_delay,
                            lih_leg1_cooldown, lih_rebalance_cooldown,
                            lih_use_mirror, lih_leg1_shares, lih_allow_over_target,
                            lih_force_balance_secs, lih_max_rebalance_shares,
                            lih_flex_rebalance, lih_flex_dilute_ratio,
                            lih_leg1_trend_align, lih_trend_lookback_sec,
                            lih_leg1_trend_mode, lih_leg1_trend_max,
                            lih_endgame_secs, lih_endgame_hold_ask, lih_endgame_resume_hedge_ask,
                            lih_endgame_soft_cap, lih_endgame_step_small, lih_endgame_step_large,
                            lih_endgame_gap_large, lih_endgame_override_secs,
                            lih_endgame_override_cooldown);
                    }
                    risk_manager.sync_lih_from_markets(all_m);
                }
                std::vector<std::string> tokens;
                for (const auto& m : all_m) { tokens.push_back(m.yes_token_id); tokens.push_back(m.no_token_id); }
                if (!tokens.empty()) poly_feed->subscribe(tokens);
                {
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    rest_poll_tokens = tokens;
                }
                store.push_telemetry(fmt::format("MARKETS REFRESHED | {} markets | {} tokens | fee_curve {}",
                    all_m.size(), tokens.size(), fee_markets));
            } catch (const std::exception& e) {
                spdlog::error("Refresh markets failed: {}", e.what());
            }
            is_refreshing = false;
        };

        refresh_markets();
        std::this_thread::sleep_for(std::chrono::seconds(3));
        auto last_market_refresh = std::chrono::system_clock::now();
        
        // --- S. 后台线程：实盘余额同步（fetch_balance.py，间隔 WALLET_SYNC_INTERVAL_SEC）---
        std::thread balance_thread([&, wallet_sync_interval_sec]() {
            while (true) {
                if (!paper_mode) {
                    const std::string bal_out = popen_read_first_line(
                        python_script_cmd("fetch_balance.py", "", false));
                    if (!bal_out.empty()) {
                        try {
                            double new_bal = std::stod(bal_out);
                            if (new_bal > 0) risk_manager.update_balance(new_bal);
                        } catch (...) {}
                    }
                }
                std::this_thread::sleep_for(std::chrono::seconds(wallet_sync_interval_sec));
            }
        });
        balance_thread.detach();

        // Binance REST 兜底：WS 不可用时轮询现货价（Docker/地区限制常见）
        auto poll_binance_rest = [&]() {
            struct SymMap { const char* sym; void (StateStore::*upd)(const PriceTick&); };
            SymMap maps[] = {
                {"BTCUSDT", &StateStore::update_btc_price},
                {"ETHUSDT", &StateStore::update_eth_price},
                {"SOLUSDT", &StateStore::update_sol_price},
            };
            for (const auto& m : maps) {
                auto px = gamma.fetch_binance_price(m.sym);
                if (!px || *px <= 0) continue;
                PriceTick tick;
                tick.price = *px;
                tick.timestamp_ms = std::chrono::duration<double, std::milli>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                tick.received_at = std::chrono::duration<double>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                tick.volume = 0;
                (store.*(m.upd))(tick);
            }
        };

        auto last_binance_rest = std::chrono::system_clock::now() - std::chrono::seconds(10);
        bool rest_fallback_logged = false;
        auto last_live_save = std::chrono::system_clock::now();
        auto last_chain_reconcile = std::chrono::system_clock::now();
        auto last_rest_book_poll = std::chrono::system_clock::now() - std::chrono::seconds(5);
        std::atomic<bool> rest_book_refreshing{false};

        // --- T. 主循环（250ms）：REST 订单簿 / 市场刷新 / 配置热更新 / 到期结算 / JSON 输出 ---
        while (true) {
            auto loop_start = std::chrono::system_clock::now();
            const bool poll_rest_book = book_aware_detect || paper_official_book;
            if (poll_rest_book &&
                // ~2.5s REST book poll (DH book-aware)
                !rest_book_refreshing.load(std::memory_order_acquire) &&
                loop_start - last_rest_book_poll > std::chrono::milliseconds(2500)) {
                last_rest_book_poll = loop_start;
                std::vector<std::string> tokens_copy;
                {
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    tokens_copy = rest_poll_tokens;
                }
                if (!tokens_copy.empty()) {
                    rest_book_refreshing.store(true, std::memory_order_release);
                    boost::asio::post(feed_ioc, [&, tokens_copy]() {
                        router.refresh_rest_book(tokens_copy);
                        rest_book_refreshing.store(false, std::memory_order_release);
                    });
                }
            }
            if (loop_start - last_market_refresh > std::chrono::seconds(gamma_market_refresh_sec)) {
                // 定期刷新 Up-Down 市场列表与 token 订阅（GAMMA_MARKET_REFRESH_SEC）
                last_market_refresh = loop_start;
                std::thread([&refresh_markets]() { refresh_markets(); }).detach();
            }
            // REST fallback when Binance WS is blocked (common in Docker/region)
            if (binance_feed_enabled && loop_start - last_binance_rest > std::chrono::seconds(2)) {
                last_binance_rest = loop_start;
                auto btc = store.get_latest_btc_price();
                if (!btc || btc->price <= 0) {
                    if (!rest_fallback_logged) {
                        spdlog::warn("Binance WS unavailable — using REST price polling");
                        rest_fallback_logged = true;
                    }
                    poll_binance_rest();
                }
            }
            risk_manager.is_trading_allowed(); // 检查熔断自动恢复
            apply_runtime_config("logs/runtime_config.json", risk_manager, store, detector_mutex, dh_detector, lih_detector);
            check_and_close_dh_positions(risk_manager, store, gamma, auto_redeem);
            if (lih_enabled) {
                const double now_sec_loop = std::chrono::duration<double>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                risk_manager.purge_expired_lih_open(now_sec_loop, 30.0);
                if (!live_lih_dry_run) {
                    const int pending_resolved = router.poll_lih_pending_fills(now_sec_loop);
                    if (pending_resolved > 0 && live_state_persist) {
                        persistence::save_live_lih_state(risk_manager, live_state_path, false);
                        try_live_chain_reconcile_async(risk_manager, live_state_path, true);
                    }
                }
                risk_manager.scrub_lih_inflight_locks(now_sec_loop);
                check_and_close_lih_positions(
                    risk_manager, store, gamma, auto_redeem,
                    (lih_enabled && live_state_persist && !live_lih_dry_run && !paper_mode)
                        ? &live_state_path : nullptr);
                try_lih_evaluate(); // 主循环也跑 LIH（不依赖 tick）
            }
            if (lih_enabled && live_state_persist && !live_lih_dry_run && !paper_mode
                && loop_start - last_chain_reconcile > std::chrono::seconds(lih_chain_reconcile_sec)) {
                last_chain_reconcile = loop_start;
                try_live_chain_reconcile_async(risk_manager, live_state_path, false);
            }
            if (lih_enabled && live_state_persist && !live_lih_dry_run
                && loop_start - last_live_save > std::chrono::seconds(10)) {
                last_live_save = loop_start;
                persistence::save_live_lih_state(risk_manager, live_state_path, false);
            }
            std::cout << store.get_dashboard_json() << std::endl; // → dashboard_bridge stdout
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }

        feed_work.reset();
        if (feed_thread.joinable()) feed_thread.join();
    } catch (const std::exception& e) {
        spdlog::critical("Fatal error: {}", e.what());
        return 1;
    }
    return 0;
}
