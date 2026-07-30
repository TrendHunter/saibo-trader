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
#include "feeds/ChainlinkFeed.h"
#include "feeds/PolymarketFeed.h"
#include "feeds/GammaClient.h"
#include "risk/RiskManager.h"
#include "signals/LegInHedgeDetector.h"
#include "signals/RegimeGate.h"
#include "telemetry/ShadowWindowRecorder.h"
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
//   7. LIH 到期平仓         check_and_close_lih_positions
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
    if (risk_manager.shadow_virtual_bankroll()) return;
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
    if (act.kind == LegInAction::Kind::AddLeg1) {
        return store.paper_leg1_extra_slip_pct();
    }
    if (act.kind == LegInAction::Kind::CompleteHedge) {
        double extra = store.paper_hedge_extra_slip_pct();
        if (act.note.find("force") != std::string::npos) {
            extra += store.paper_force_extra_slip_pct();
        }
        return extra;
    }
    if (act.kind == LegInAction::Kind::UnwindLeg1) {
        return store.paper_hedge_extra_slip_pct();
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
                const char* tag = p.is_shadow ? "SHADOW" : "LIVE";
                store.push_telemetry(fmt::format(
                    "[LIH {}] CLOSED {} | {} hedged {:.2f}sh | PnL ~${:+.2f} | YES={:.0f} NO={:.0f}",
                    tag, id, p.asset, matched, proceeds - cost, ey, en));
                if (live_state_path && !live_state_path->empty()) {
                    persistence::save_live_lih_state(risk_manager, *live_state_path, false);
                }
            }
        } else {
            risk_manager.register_lih_close(id, ey, en, "Market resolved (unhedged)", now);
            const char* tag = p.is_shadow ? "SHADOW" : "LIVE";
            store.push_telemetry(fmt::format(
                "[LIH {}] CLOSED {} | {} UNHEDGED | yes={:.2f} no={:.2f} | YES={:.0f} NO={:.0f}",
                tag, id, p.asset, p.yes_shares, p.no_shares, ey, en));
            if (live_state_path && !live_state_path->empty()) {
                persistence::save_live_lih_state(risk_manager, *live_state_path, false);
            }
        }
        if (is_live && auto_redeem_enabled && !condition_id.empty()) {
            attempt_onchain_redeem_async(condition_id, id, p.is_neg_risk, store, risk_manager);
        }
    }
}

// 解析 Web/bridge 写入的布尔配置字符串
static bool parse_config_bool(const std::string& v) {
    std::string lower = v;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    return !(lower == "false" || lower == "0" || lower == "no" || lower == "off");
}

// 按资产/窗口粒度开关市场（DH_ENABLE_5M_BTC 等，LIH 复用）
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

    // patch：Web 策略页保存的 .env 热更新项（风控 / LIH / 市场开关）
    if (obj.contains("patch") && obj.at("patch").is_object()) {
        const auto& patch = obj.at("patch").as_object();

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
                } else if (k == "LIH_SKIP_PARTIAL_WINDOW_ON_START") {
                    const bool enabled = env_flag_true({{k, v}}, k, true);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_skip_partial_window_on_start(enabled);
                    store.push_telemetry(fmt::format("CONFIG LIH_SKIP_PARTIAL_WINDOW_ON_START={}",
                        enabled ? "true" : "false"));
                } else if (k == "LIH_LEG1_SHARES") {
                    const double x = std::stod(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_leg1_shares(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_LEG1_SHARES={}", v));
                } else if (k == "LIH_LEG1_CLIP_SHARES") {
                    const double x = std::stod(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_leg1_clip_shares(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_LEG1_CLIP_SHARES={}", v));
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
                    const bool trigger = (mode == "trigger");
                    const bool trend = (mode == "trend" || mode == "expensive");
                    const bool mm2 = (mode == "mm2");
                    store.set_lih_leg1_mode(mm2 ? "mm2" : (trigger ? "trigger" : (trend ? "trend" : "cheap")));
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) {
                        lih_detector->set_leg1_trigger_mode(trigger);
                        lih_detector->set_leg1_trend_mode(trend);
                        lih_detector->set_mm2_mode(mm2);
                    }
                    store.push_telemetry(fmt::format("CONFIG LIH_LEG1_MODE={}",
                        mm2 ? "mm2" : (trigger ? "trigger" : (trend ? "trend" : "cheap"))));
                } else if (k == "LIH_LEG1_TRIGGER_MIN") {
                    const double x = std::stod(v);
                    store.set_lih_leg1_trigger_min(x);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_leg1_trigger_min(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_LEG1_TRIGGER_MIN={}", v));
                } else if (k == "LIH_LEG1_TRIGGER_MAX") {
                    const double x = std::stod(v);
                    store.set_lih_leg1_trigger_max(x);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_leg1_trigger_max(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_LEG1_TRIGGER_MAX={}", v));
                } else if (k == "LIH_QUOTE_MODE") {
                    std::string mode = v;
                    std::transform(mode.begin(), mode.end(), mode.begin(), ::tolower);
                    if (mode != "rest" && mode != "conservative") mode = "conservative";
                    store.set_lih_quote_mode(mode);
                    store.push_telemetry(fmt::format("CONFIG LIH_QUOTE_MODE={}", mode));
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
                } else if (k == "LIH_ENDGAME_MINIMIZE_GAP") {
                    const bool enabled = env_flag_true({{k, v}}, k, true);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_endgame_minimize_gap(enabled);
                    store.push_telemetry(fmt::format("CONFIG LIH_ENDGAME_MINIMIZE_GAP={}",
                        enabled ? "true" : "false"));
                } else if (k == "LIH_MAX_ENTRY_MARGINAL") {
                    const double x = std::stod(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_max_entry_marginal(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_MAX_ENTRY_MARGINAL={}", v));
                } else if (k == "LIH_MID_SOFT_CAP") {
                    const double x = std::stod(v);
                    store.set_lih_mid_soft_cap(x);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_mid_soft_cap(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_MID_SOFT_CAP={}", v));
                } else if (k == "LIH_MID_SOFT_START_SECS") {
                    const double x = std::stod(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_mid_soft_start_secs(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_MID_SOFT_START_SECS={}", v));
                } else if (k == "LIH_HEDGE_FEASIBLE_ENTRY") {
                    const bool enabled = env_flag_true({{k, v}}, k, false);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_hedge_feasible_entry(enabled);
                    store.push_telemetry(fmt::format("CONFIG LIH_HEDGE_FEASIBLE_ENTRY={}",
                        enabled ? "true" : "false"));
                } else if (k == "LIH_HEDGE_FEASIBLE_CAP") {
                    const double x = std::stod(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_hedge_feasible_cap(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_HEDGE_FEASIBLE_CAP={}", v));
                } else if (k == "LIH_VWAP_ENTRY_GATE") {
                    const bool enabled = env_flag_true({{k, v}}, k, false);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_vwap_entry_gate(enabled);
                    store.push_telemetry(fmt::format("CONFIG LIH_VWAP_ENTRY_GATE={}",
                        enabled ? "true" : "false"));
                } else if (k == "LIH_VWAP_ENTRY_CAP") {
                    const double x = std::stod(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_vwap_entry_cap(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_VWAP_ENTRY_CAP={}", v));
                } else if (k == "LIH_VWAP_DEPTH_RATIO") {
                    const double x = std::stod(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_vwap_depth_ratio(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_VWAP_DEPTH_RATIO={}", v));
                } else if (k == "LIH_MIN_EDGE_USDC") {
                    const double x = std::stod(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_min_edge_usdc(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_MIN_EDGE_USDC={}", v));
                } else if (k == "LIH_MIN_EDGE_PER_SHARE") {
                    const double x = std::stod(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_min_edge_per_share(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_MIN_EDGE_PER_SHARE={}", v));
                } else if (k == "LIH_UNWIND_ENABLED") {
                    const bool enabled = env_flag_true({{k, v}}, k, true);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_unwind_enabled(enabled);
                    store.push_telemetry(fmt::format("CONFIG LIH_UNWIND_ENABLED={}",
                        enabled ? "true" : "false"));
                } else if (k == "LIH_UNWIND_SECS") {
                    const double x = std::stod(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_unwind_secs(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_UNWIND_SECS={}", v));
                } else if (k == "LIH_UNWIND_COOLDOWN") {
                    const double x = std::stod(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_unwind_cooldown(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_UNWIND_COOLDOWN={}", v));
                } else if (k == "LIH_PARALLEL_CLIP_HEDGE") {
                    const bool enabled = env_flag_true({{k, v}}, k, false);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_parallel_clip_hedge(enabled);
                    store.push_telemetry(fmt::format("CONFIG LIH_PARALLEL_CLIP_HEDGE={}",
                        enabled ? "true" : "false"));
                } else if (k == "LIH_PARALLEL_HEDGE_MAX_COMBINED") {
                    const double x = std::stod(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_parallel_hedge_max_combined(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_PARALLEL_HEDGE_MAX_COMBINED={}", v));
                } else if (k == "LIH_EARLY_HEDGE_MAX_COMBINED") {
                    const double x = std::stod(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_early_hedge_max_combined(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_EARLY_HEDGE_MAX_COMBINED={}", v));
                } else if (k == "LIH_OPEN_GAP") {
                    const bool enabled = env_flag_true({{k, v}}, k, false);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_open_gap_mode(enabled);
                    store.push_telemetry(fmt::format("CONFIG LIH_OPEN_GAP={}",
                        enabled ? "true" : "false"));
                } else if (k == "LIH_HEAVY_CLIP_SHARES") {
                    const double x = std::stod(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_heavy_clip_shares(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_HEAVY_CLIP_SHARES={}", v));
                } else if (k == "LIH_HEAVY_MAX_PRICE") {
                    const double x = std::stod(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_heavy_max_price(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_HEAVY_MAX_PRICE={}", v));
                } else if (k == "LIH_MAX_GAP_SHARES") {
                    const double x = std::stod(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_max_gap_shares(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_MAX_GAP_SHARES={}", v));
                } else if (k == "LIH_GAP_HEDGE_MAX_COMBINED") {
                    const double x = std::stod(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_gap_hedge_max_combined(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_GAP_HEDGE_MAX_COMBINED={}", v));
                } else if (k == "LIH_HEDGE_MIN_GAP_TRIGGER") {
                    const double x = std::stod(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_hedge_min_gap_trigger(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_HEDGE_MIN_GAP_TRIGGER={}", v));
                } else if (k == "LIH_HEDGE_TARGET_MIN_GAP") {
                    const double x = std::stod(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_hedge_target_min_gap(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_HEDGE_TARGET_MIN_GAP={}", v));
                } else if (k == "LIH_LEG1_ORDER_MODE") {
                    std::string mode = v;
                    std::transform(mode.begin(), mode.end(), mode.begin(), ::tolower);
                    store.set_lih_leg1_order_mode(mode);
                    store.push_telemetry(fmt::format("CONFIG LIH_LEG1_ORDER_MODE={}", mode));
                } else if (k == "LIH_MM2_MODE") {
                    const bool enabled = parse_config_bool(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_mm2_mode(enabled);
                    store.push_telemetry(fmt::format("CONFIG LIH_MM2_MODE={}",
                        enabled ? "true" : "false"));
                } else if (k == "LIH_MM2_MIN_SPOT_BPS") {
                    const double x = std::stod(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_mm2_min_spot_bps(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_MM2_MIN_SPOT_BPS={}", v));
                } else if (k == "LIH_MM2_ENTRY_MAX_SECS_LEFT") {
                    const double x = std::stod(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_mm2_entry_max_secs_left(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_MM2_ENTRY_MAX_SECS_LEFT={}", v));
                } else if (k == "LIH_MM2_ENTRY_MIN_SECS_LEFT") {
                    const double x = std::stod(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_mm2_entry_min_secs_left(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_MM2_ENTRY_MIN_SECS_LEFT={}", v));
                } else if (k == "LIH_MM2_FAVORITE_MIN") {
                    const double x = std::stod(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_mm2_favorite_min_px(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_MM2_FAVORITE_MIN={}", v));
                } else if (k == "LIH_MM2_SOFT_SPOT_BPS") {
                    const double x = std::stod(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_mm2_soft_spot_bps(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_MM2_SOFT_SPOT_BPS={}", v));
                } else if (k == "LIH_MM2_LATE_TILT_MIN_ASK") {
                    const double x = std::stod(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_mm2_late_tilt_min_ask(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_MM2_LATE_TILT_MIN_ASK={}", v));
                } else if (k == "LIH_MM2_LATE_TILT_MIN_SPREAD") {
                    const double x = std::stod(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_mm2_late_tilt_min_spread(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_MM2_LATE_TILT_MIN_SPREAD={}", v));
                } else if (k == "LIH_MM2_HEAVY_DELAY_SEC") {
                    const double x = std::stod(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_mm2_heavy_delay_sec(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_MM2_HEAVY_DELAY_SEC={}", v));
                } else if (k == "LIH_MM2_SCALE_CLIP") {
                    const double x = std::stod(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_mm2_scale_clip_shares(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_MM2_SCALE_CLIP={}", v));
                } else if (k == "LIH_MM2_HEAVY_MAX_SHARES") {
                    const double x = std::stod(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_mm2_heavy_max_shares(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_MM2_HEAVY_MAX_SHARES={}", v));
                } else if (k == "LIH_MM2_SCALE_BOOST") {
                    const double x = std::stod(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_mm2_scale_boost(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_MM2_SCALE_BOOST={}", v));
                } else if (k == "LIH_MM2_SCALE_AGAINST_STOP_BPS") {
                    const double x = std::stod(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_mm2_scale_against_stop_bps(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_MM2_SCALE_AGAINST_STOP_BPS={}", v));
                } else if (k == "LIH_MM2_HF1E_LATCH_GAP") {
                    const double x = std::stod(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_mm2_hf1e_latch_gap(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_MM2_HF1E_LATCH_GAP={}", v));
                } else if (k == "LIH_MM2_HF1E_LATCH_SECS_LEFT") {
                    const double x = std::stod(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_mm2_hf1e_latch_secs_left(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_MM2_HF1E_LATCH_SECS_LEFT={}", v));
                } else if (k == "LIH_MM2_HEDGE_BOOST") {
                    const double x = std::stod(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_mm2_hedge_boost(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_MM2_HEDGE_BOOST={}", v));
                } else if (k == "LIH_MM2_EARLY_ENTRY_MAX_SECS_LEFT") {
                    const double x = std::stod(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_mm2_early_entry_max_secs_left(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_MM2_EARLY_ENTRY_MAX_SECS_LEFT={}", v));
                } else if (k == "LIH_MM2_EARLY_TILT_MIN_SPREAD") {
                    const double x = std::stod(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_mm2_early_tilt_min_spread(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_MM2_EARLY_TILT_MIN_SPREAD={}", v));
                } else if (k == "LIH_MM2_EARLY_TILT_MIN_FAV") {
                    const double x = std::stod(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_mm2_early_tilt_min_fav(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_MM2_EARLY_TILT_MIN_FAV={}", v));
                } else if (k == "LIH_MM2_EARLY_YES_GUARD") {
                    const bool on = (v == "1" || v == "true" || v == "TRUE" || v == "yes");
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_mm2_early_yes_guard(on);
                    store.push_telemetry(fmt::format("CONFIG LIH_MM2_EARLY_YES_GUARD={}",
                        on ? "true" : "false"));
                } else if (k == "LIH_MM2_FAV_EARLY_BYPASS") {
                    const bool on = (v == "1" || v == "true" || v == "TRUE" || v == "yes");
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_mm2_fav_early_bypass(on);
                    store.push_telemetry(fmt::format("CONFIG LIH_MM2_FAV_EARLY_BYPASS={}",
                        on ? "true" : "false"));
                } else if (k == "LIH_MM2_FAV_EARLY_MODE") {
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_mm2_fav_early_mode(v);
                    store.push_telemetry(fmt::format("CONFIG LIH_MM2_FAV_EARLY_MODE={}", v));
                } else if (k == "LIH_MM2_FAV_EARLY_FAV_LO") {
                    const double x = std::stod(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_mm2_fav_early_fav_lo(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_MM2_FAV_EARLY_FAV_LO={}", v));
                } else if (k == "LIH_MM2_FAV_EARLY_FAV_HI") {
                    const double x = std::stod(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_mm2_fav_early_fav_hi(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_MM2_FAV_EARLY_FAV_HI={}", v));
                } else if (k == "LIH_MM2_FAV_EARLY_MIN_SPREAD") {
                    const double x = std::stod(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_mm2_fav_early_min_spread(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_MM2_FAV_EARLY_MIN_SPREAD={}", v));
                } else if (k == "LIH_MM2_FAV_EARLY_MIN_DTILT") {
                    const double x = std::stod(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_mm2_fav_early_min_dtilt(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_MM2_FAV_EARLY_MIN_DTILT={}", v));
                } else if (k == "LIH_MM2_MAIN_BJ_START") {
                    const int x = std::stoi(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) {
                        lih_detector->set_mm2_main_bj_hours(x, lih_detector->mm2_main_bj_end());
                    }
                    store.set_mm2_secondary_gates(
                        x, store.mm2_main_bj_end(), store.mm2_offhours_min_ask());
                    store.push_telemetry(fmt::format("CONFIG LIH_MM2_MAIN_BJ_START={}", v));
                } else if (k == "LIH_MM2_MAIN_BJ_END") {
                    const int x = std::stoi(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) {
                        lih_detector->set_mm2_main_bj_hours(lih_detector->mm2_main_bj_start(), x);
                    }
                    store.set_mm2_secondary_gates(
                        store.mm2_main_bj_start(), x, store.mm2_offhours_min_ask());
                    store.push_telemetry(fmt::format("CONFIG LIH_MM2_MAIN_BJ_END={}", v));
                } else if (k == "LIH_MM2_OFFHOURS_MIN_ASK") {
                    const double x = std::stod(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_mm2_offhours_min_ask(x);
                    store.set_mm2_secondary_gates(
                        store.mm2_main_bj_start(), store.mm2_main_bj_end(), x);
                    store.push_telemetry(fmt::format("CONFIG LIH_MM2_OFFHOURS_MIN_ASK={}", v));
                } else if (k == "LIH_MM2_MIN_SIDE_DEPTH") {
                    const double x = std::stod(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_mm2_min_side_depth(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_MM2_MIN_SIDE_DEPTH={}", v));
                } else if (k == "LIH_MM2_MIN_SIDE_DEPTH_MAIN_ONLY") {
                    const bool x = (v == "1" || v == "true" || v == "TRUE" || v == "yes");
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_mm2_min_side_depth_main_only(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_MM2_MIN_SIDE_DEPTH_MAIN_ONLY={}", v));
                } else if (k == "LIH_MM2_VOL_GATE") {
                    const bool enabled = env_flag_true({{k, v}}, k, false);
                    if (lih_detector) lih_detector->set_mm2_vol_gate(enabled);
                    store.push_telemetry(fmt::format("CONFIG LIH_MM2_VOL_GATE={}",
                        enabled ? "true" : "false"));
                } else if (k == "LIH_MM2_V2J_ADAPTIVE") {
                    const bool enabled = env_flag_true({{k, v}}, k, false);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_mm2_v2j_adaptive(enabled);
                    store.push_telemetry(fmt::format("CONFIG LIH_MM2_V2J_ADAPTIVE={}",
                        enabled ? "true" : "false"));
                } else if (k == "LIH_MM2_V2J_MOM_LOOKBACK_SEC") {
                    const double x = std::stod(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_mm2_v2j_mom_lookback_sec(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_MM2_V2J_MOM_LOOKBACK_SEC={}", v));
                } else if (k == "LIH_MM2_V2J_MOM_FAV_MAX_ASK") {
                    const double x = std::stod(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_mm2_v2j_mom_fav_max_ask(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_MM2_V2J_MOM_FAV_MAX_ASK={}", v));
                } else if (k == "LIH_MM2_V2J_SPREAD_MIN") {
                    const double x = std::stod(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_mm2_v2j_spread_min(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_MM2_V2J_SPREAD_MIN={}", v));
                } else if (k == "LIH_MM2_V2J_SPREAD_FAV_MAX_ASK") {
                    const double x = std::stod(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_mm2_v2j_spread_fav_max_ask(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_MM2_V2J_SPREAD_FAV_MAX_ASK={}", v));
                } else if (k == "LIH_MM2_TILT_ENTRY") {
                    const bool enabled = (v == "1" || v == "true" || v == "TRUE" || v == "yes");
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_mm2_tilt_entry(enabled);
                    store.push_telemetry(fmt::format("CONFIG LIH_MM2_TILT_ENTRY={}",
                                                     enabled ? "true" : "false"));
                } else if (k == "LIH_MM2_TILT_DELTA") {
                    const double x = std::stod(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_mm2_tilt_delta(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_MM2_TILT_DELTA={}", v));
                } else if (k == "LIH_MM2_TILT_SIDE_FOLLOW") {
                    const bool enabled = (v == "1" || v == "true" || v == "TRUE" || v == "yes");
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_mm2_tilt_side_follow(enabled);
                    store.push_telemetry(fmt::format("CONFIG LIH_MM2_TILT_SIDE_FOLLOW={}",
                                                     enabled ? "true" : "false"));
                } else if (k == "LIH_MM2_CHEAPER_LATER") {
                    const bool enabled = (v == "1" || v == "true" || v == "TRUE" || v == "yes");
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_mm2_cheaper_later(enabled);
                    store.push_telemetry(fmt::format("CONFIG LIH_MM2_CHEAPER_LATER={}",
                                                     enabled ? "true" : "false"));
                } else if (k == "LIH_MM2_SPOT_LEG1_MAX_USDC") {
                    const double x = std::stod(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_mm2_spot_leg1_max_usdc(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_MM2_SPOT_LEG1_MAX_USDC={}", v));
                } else if (k == "LIH_MM2_SPOT_HEAVY_MAX_USDC") {
                    const double x = std::stod(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) lih_detector->set_mm2_spot_heavy_max_usdc(x);
                    store.push_telemetry(fmt::format("CONFIG LIH_MM2_SPOT_HEAVY_MAX_USDC={}", v));
                } else if (k == "LIH_MM2_SESSION_UTC_START") {
                    const int x = std::stoi(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) {
                        lih_detector->set_mm2_session_utc_hours(
                            x, lih_detector->mm2_session_utc_end());
                    }
                    store.push_telemetry(fmt::format("CONFIG LIH_MM2_SESSION_UTC_START={}", v));
                } else if (k == "LIH_MM2_SESSION_UTC_END") {
                    const int x = std::stoi(v);
                    std::lock_guard<std::mutex> lock(detector_mutex);
                    if (lih_detector) {
                        lih_detector->set_mm2_session_utc_hours(
                            lih_detector->mm2_session_utc_start(), x);
                    }
                    store.push_telemetry(fmt::format("CONFIG LIH_MM2_SESSION_UTC_END={}", v));
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

        // --- D. 实盘开关：auto-redeem、LIH dry-run、Python CLOB bridge 路径 ---
        bool auto_redeem = !paper_mode && env_flag_true(env, "AUTO_REDEEM", true);
        bool live_lih_dry_run = !paper_mode && env_flag_true(env, "LIVE_LIH_DRY_RUN", true);
        bool use_python_clob = !paper_mode && env_flag_true(env, "USE_PYTHON_CLOB", true);
        const double shadow_bankroll_usdc = env_double_or(env, "SHADOW_BANKROLL_USDC", 0.0);
        const double wallet_balance_usdc = starting_balance;
        const bool shadow_virtual_bankroll = live_lih_dry_run && shadow_bankroll_usdc > 0.0;
        if (shadow_virtual_bankroll) {
            starting_balance = shadow_bankroll_usdc;
        }
        std::string clob_bridge_host = env.count("CLOB_BRIDGE_HOST") ? env["CLOB_BRIDGE_HOST"] : "127.0.0.1";
        int clob_bridge_port = env.count("CLOB_BRIDGE_PORT") ? std::stoi(env["CLOB_BRIDGE_PORT"]) : 8081;
        std::string clob_bridge_path = env.count("CLOB_BRIDGE_PATH") ? env["CLOB_BRIDGE_PATH"] : "/internal/clob/order";
        const int wallet_sync_interval_sec = env_int(env, "WALLET_SYNC_INTERVAL_SEC", 2, 1, 120);
        const int lih_chain_reconcile_sec = env_int(env, "LIH_CHAIN_RECONCILE_SEC", 10, 5, 600);
        const int gamma_market_refresh_sec = env_int(env, "GAMMA_MARKET_REFRESH_SEC", 5, 3, 120);

        if (shadow_virtual_bankroll) {
            spdlog::info("Shadow virtual bankroll ${:.2f} (wallet ${:.2f}, wallet sync off)",
                           starting_balance, wallet_balance_usdc);
        }
        spdlog::info("Starting Core v3.0 (LIH) | Mode: {} | Bal: ${:.2f} | Auto-redeem: {} | LIH dry-run: {} | wallet_sync={}s | gamma_refresh={}s",
                     live_lih_dry_run ? "SHADOW" : "LIVE", starting_balance,
                     auto_redeem ? "on" : "off",
                     live_lih_dry_run ? "on" : "off",
                     shadow_virtual_bankroll ? 0 : wallet_sync_interval_sec,
                     gamma_market_refresh_sec);

        // --- E. 网络 IO 上下文：Feed 线程（Binance/Polymarket WS）与 Gamma REST ---
        boost::asio::io_context feed_ioc;
        boost::asio::ssl::context feed_ctx{boost::asio::ssl::context::sslv23_client};
        feed_ctx.set_default_verify_paths();

        boost::asio::io_context gamma_ioc;
        boost::asio::ssl::context gamma_ctx{boost::asio::ssl::context::sslv23_client};
        gamma_ctx.set_default_verify_paths();

        // --- F. 风控参数（RiskManager）---
        double max_pos = env.count("RISK_MAX_POSITION_FRACTION") ? std::stod(env["RISK_MAX_POSITION_FRACTION"]) : 0.08;
        double daily_loss = env.count("RISK_DAILY_LOSS_LIMIT") ? std::stod(env["RISK_DAILY_LOSS_LIMIT"]) : 0.20;
        double drawdown = env.count("RISK_TOTAL_DRAWDOWN_KILL") ? std::stod(env["RISK_TOTAL_DRAWDOWN_KILL"]) : 0.40;
        int max_concurrent = env.count("RISK_MAX_CONCURRENT_POSITIONS") ? std::stoi(env["RISK_MAX_CONCURRENT_POSITIONS"]) : 3;
        double fee_rate = env.count("FEE_RATE") ? std::stod(env["FEE_RATE"]) : 0.018;

        // --- G. LIH 分腿对冲参数（leg1 入场 / rebalance / force 配平）---
        if (!paper_mode && !live_lih_dry_run) {
            spdlog::warn("[LIVE LIH] LIVE_LIH_DRY_RUN=false — real CLOB orders WILL be sent");
        }
        bool lih_use_mirror = env_flag_true(env, "LIH_USE_MIRROR", true);
        double lih_leg1_max = env.count("LIH_LEG1_MAX_PRICE") ? std::stod(env["LIH_LEG1_MAX_PRICE"]) : 0.45;
        double lih_target_combined = env.count("LIH_TARGET_COMBINED") ? std::stod(env["LIH_TARGET_COMBINED"]) : 0.94;
        double lih_min_secs = env.count("LIH_MIN_SECONDS_REMAINING") ? std::stod(env["LIH_MIN_SECONDS_REMAINING"]) : 15.0;
        double lih_leg1_min_secs = env.count("LIH_LEG1_MIN_SECONDS_REMAINING")
            ? std::stod(env["LIH_LEG1_MIN_SECONDS_REMAINING"]) : 60.0;
        double lih_leg1_start_delay = env_double_or(env, "LIH_LEG1_START_DELAY_SEC", 5.0);
        bool lih_skip_partial_window = env_flag_true(env, "LIH_SKIP_PARTIAL_WINDOW_ON_START", true);
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
        double lih_leg1_clip_shares = env_double_or(env, "LIH_LEG1_CLIP_SHARES", 0.0);
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
        const bool lih_leg1_trigger_mode = (lih_leg1_mode == "trigger");
        const bool lih_leg1_trend_mode = (lih_leg1_mode == "trend" || lih_leg1_mode == "expensive");
        double lih_leg1_trend_max = env_double_or(env, "LIH_LEG1_TREND_MAX_PRICE", 0.65);
        double lih_leg1_trigger_min = env_double_or(env, "LIH_LEG1_TRIGGER_MIN", 0.70);
        double lih_leg1_trigger_max = env_double_or(env, "LIH_LEG1_TRIGGER_MAX", 0.0);
        std::string lih_quote_mode = env.count("LIH_QUOTE_MODE") ? env["LIH_QUOTE_MODE"] : "conservative";
        std::transform(lih_quote_mode.begin(), lih_quote_mode.end(), lih_quote_mode.begin(), ::tolower);
        if (lih_quote_mode != "rest" && lih_quote_mode != "conservative") {
            lih_quote_mode = "conservative";
        }
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
        bool lih_endgame_minimize_gap = env_flag_true(env, "LIH_ENDGAME_MINIMIZE_GAP", true);
        bool lih_endgame_ladder_enabled = env_flag_true(env, "LIH_ENDGAME_LADDER_ENABLED", true);
        double lih_endgame_ladder_secs = env_double_or(env, "LIH_ENDGAME_LADDER_SECS", 90.0);
        double lih_endgame_ladder_start = env_double_or(env, "LIH_ENDGAME_LADDER_START", 0.95);
        double lih_endgame_ladder_end = env_double_or(env, "LIH_ENDGAME_LADDER_END", 0.97);
        double lih_endgame_ladder_step = env_double_or(env, "LIH_ENDGAME_LADDER_STEP", 0.01);
        double lih_max_entry_marginal = env_double_or(env, "LIH_MAX_ENTRY_MARGINAL", 1.15);
        double lih_mid_soft_cap = env_double_or(env, "LIH_MID_SOFT_CAP", 0.0);
        double lih_mid_soft_start_secs = env_double_or(env, "LIH_MID_SOFT_START_SECS", 300.0);
        bool lih_hedge_feasible_entry = env_flag_true(env, "LIH_HEDGE_FEASIBLE_ENTRY", false);
        double lih_hedge_feasible_cap = env_double_or(env, "LIH_HEDGE_FEASIBLE_CAP", 0.0);
        bool lih_vwap_entry_gate = env_flag_true(env, "LIH_VWAP_ENTRY_GATE", false);
        double lih_vwap_entry_cap = env_double_or(env, "LIH_VWAP_ENTRY_CAP", 0.0);
        double lih_vwap_depth_ratio = env_double_or(env, "LIH_VWAP_DEPTH_RATIO", 0.90);
        double lih_min_edge_usdc = env_double_or(env, "LIH_MIN_EDGE_USDC", 0.0);
        double lih_min_edge_per_share = env_double_or(env, "LIH_MIN_EDGE_PER_SHARE", 0.05);
        bool lih_unwind_enabled = env_flag_true(env, "LIH_UNWIND_ENABLED", true);
        double lih_unwind_secs = env_double_or(env, "LIH_UNWIND_SECS", 120.0);
        double lih_unwind_cooldown = env_double_or(env, "LIH_UNWIND_COOLDOWN", 10.0);
        bool lih_parallel_clip_hedge = env_flag_true(env, "LIH_PARALLEL_CLIP_HEDGE", false);
        double lih_parallel_hedge_max_combined = env_double_or(env, "LIH_PARALLEL_HEDGE_MAX_COMBINED", 1.0);
        double lih_early_hedge_max_combined = env_double_or(env, "LIH_EARLY_HEDGE_MAX_COMBINED", 0.0);
        bool lih_open_gap = env_flag_true(env, "LIH_OPEN_GAP", false);
        double lih_heavy_clip_shares = env_double_or(env, "LIH_HEAVY_CLIP_SHARES", 100.0);
        double lih_heavy_max_price = env_double_or(env, "LIH_HEAVY_MAX_PRICE", 0.75);
        double lih_max_gap_shares = env_double_or(env, "LIH_MAX_GAP_SHARES", 0.0);
        double lih_gap_hedge_max_combined = env_double_or(env, "LIH_GAP_HEDGE_MAX_COMBINED", 0.0);
        bool lih_mm2_mode = env_flag_true(env, "LIH_MM2_MODE", false);
        if (lih_leg1_mode == "mm2") lih_mm2_mode = true;
        double lih_mm2_min_spot_bps = env_double_or(env, "LIH_MM2_MIN_SPOT_BPS", 0.0);
        double lih_mm2_entry_max_secs_left = env_double_or(env, "LIH_MM2_ENTRY_MAX_SECS_LEFT", 180.0);
        double lih_mm2_entry_min_secs_left = env_double_or(env, "LIH_MM2_ENTRY_MIN_SECS_LEFT", 20.0);
        double lih_mm2_favorite_min = env_double_or(env, "LIH_MM2_FAVORITE_MIN", 0.48);
        double lih_mm2_soft_spot_bps = env_double_or(env, "LIH_MM2_SOFT_SPOT_BPS", 0.0);
        double lih_mm2_late_tilt_min_ask = env_double_or(env, "LIH_MM2_LATE_TILT_MIN_ASK", 0.0);
        double lih_mm2_late_tilt_min_spread = env_double_or(env, "LIH_MM2_LATE_TILT_MIN_SPREAD", 0.15);
        double lih_mm2_heavy_delay_sec = env_double_or(env, "LIH_MM2_HEAVY_DELAY_SEC", 40.0);
        double lih_mm2_scale_clip = env_double_or(env, "LIH_MM2_SCALE_CLIP", 10.0);
        double lih_mm2_heavy_max_shares = env_double_or(env, "LIH_MM2_HEAVY_MAX_SHARES", 0.0);
        double lih_mm2_scale_boost = env_double_or(env, "LIH_MM2_SCALE_BOOST", 1.0);
        double lih_mm2_scale_against_stop_bps =
            env_double_or(env, "LIH_MM2_SCALE_AGAINST_STOP_BPS", 0.0);
        double lih_mm2_hf1e_latch_gap = env_double_or(env, "LIH_MM2_HF1E_LATCH_GAP", 0.0);
        double lih_mm2_hf1e_latch_secs_left =
            env_double_or(env, "LIH_MM2_HF1E_LATCH_SECS_LEFT", 100.0);
        double lih_mm2_hedge_boost = env_double_or(env, "LIH_MM2_HEDGE_BOOST", 1.0);
        double lih_mm2_early_entry_max = env_double_or(env, "LIH_MM2_EARLY_ENTRY_MAX_SECS_LEFT", 0.0);
        double lih_mm2_early_tilt_spread = env_double_or(env, "LIH_MM2_EARLY_TILT_MIN_SPREAD", 0.35);
        double lih_mm2_early_tilt_fav = env_double_or(env, "LIH_MM2_EARLY_TILT_MIN_FAV", 0.65);
        bool lih_mm2_early_yes_guard = env_flag_true(env, "LIH_MM2_EARLY_YES_GUARD", false);
        int lih_mm2_main_bj_start = env.count("LIH_MM2_MAIN_BJ_START")
            ? std::stoi(env.at("LIH_MM2_MAIN_BJ_START")) : 8;
        int lih_mm2_main_bj_end = env.count("LIH_MM2_MAIN_BJ_END")
            ? std::stoi(env.at("LIH_MM2_MAIN_BJ_END")) : 17;
        double lih_mm2_offhours_min_ask = env_double_or(env, "LIH_MM2_OFFHOURS_MIN_ASK", 0.0);
        double lih_mm2_min_side_depth = env_double_or(env, "LIH_MM2_MIN_SIDE_DEPTH", 0.0);
        bool lih_mm2_min_side_depth_main_only =
            env_flag_true(env, "LIH_MM2_MIN_SIDE_DEPTH_MAIN_ONLY", true);
        double lih_mm2_spot_leg1_max_usdc = env_double_or(env, "LIH_MM2_SPOT_LEG1_MAX_USDC", 0.0);
        double lih_mm2_spot_heavy_max_usdc = env_double_or(env, "LIH_MM2_SPOT_HEAVY_MAX_USDC", 0.0);
        bool lih_mm2_align = env_flag_true(env, "LIH_MM2_ALIGN", false);
        std::string lih_leg1_order_mode = env.count("LIH_LEG1_ORDER_MODE") ? env["LIH_LEG1_ORDER_MODE"] : "taker";
        std::transform(lih_leg1_order_mode.begin(), lih_leg1_order_mode.end(), lih_leg1_order_mode.begin(), ::tolower);
        double lih_hedge_min_gap_trigger = env_double_or(env, "LIH_HEDGE_MIN_GAP_TRIGGER", 0.0);
        double lih_hedge_target_min_gap = env_double_or(env, "LIH_HEDGE_TARGET_MIN_GAP", 0.0);
        int lih_mm2_session_utc_start = -1;
        int lih_mm2_session_utc_end = -1;
        if (env.count("LIH_MM2_SESSION_UTC_START")) {
            lih_mm2_session_utc_start = std::stoi(env.at("LIH_MM2_SESSION_UTC_START"));
        }
        if (env.count("LIH_MM2_SESSION_UTC_END")) {
            lih_mm2_session_utc_end = std::stoi(env.at("LIH_MM2_SESSION_UTC_END"));
        }
        bool lih_mm2_session_from_obs = env_flag_true(env, "LIH_MM2_SESSION_FROM_OBS", false);
        std::string lih_mm2_session_file = env.count("LIH_MM2_SESSION_FILE")
            ? env["LIH_MM2_SESSION_FILE"] : "data/mm2_session_active.json";
        const bool lih_mm2_session_explicit = env.count("LIH_MM2_SESSION_FILE")
            || lih_mm2_session_utc_start >= 0
            || lih_mm2_session_utc_end >= 0;
        bool lih_mm2_skip_flat = env_flag_true(env, "LIH_MM2_SKIP_FLAT", false);
        double lih_mm2_flat_max_spot_bps = env_double_or(env, "LIH_MM2_FLAT_MAX_SPOT_BPS", 0.5);
        double lih_mm2_flat_max_ask_sum = env_double_or(env, "LIH_MM2_FLAT_MAX_ASK_SUM", 0.0);
        bool lih_mm2_vol_gate = env_flag_true(env, "LIH_MM2_VOL_GATE", false);
        double lih_mm2_vol_min_spot_bps = env_double_or(env, "LIH_MM2_VOL_MIN_SPOT_BPS", 0.0);
        double lih_mm2_vol_max_spot_bps = env_double_or(env, "LIH_MM2_VOL_MAX_SPOT_BPS", 0.0);
        double lih_mm2_vol_max_spot_std = env_double_or(env, "LIH_MM2_VOL_MAX_SPOT_STD", 0.0);
        bool lih_mm2_v2j_adaptive = env_flag_true(env, "LIH_MM2_V2J_ADAPTIVE", false);
        double lih_mm2_v2j_mom_lookback = env_double_or(env, "LIH_MM2_V2J_MOM_LOOKBACK_SEC", 60.0);
        double lih_mm2_v2j_mom_fav_max = env_double_or(env, "LIH_MM2_V2J_MOM_FAV_MAX_ASK", 0.60);
        double lih_mm2_v2j_spread_min = env_double_or(env, "LIH_MM2_V2J_SPREAD_MIN", 0.12);
        double lih_mm2_v2j_spread_fav_max = env_double_or(env, "LIH_MM2_V2J_SPREAD_FAV_MAX_ASK", 0.58);
        bool lih_mm2_tilt_entry = env_flag_true(env, "LIH_MM2_TILT_ENTRY", false);
        double lih_mm2_tilt_delta = env_double_or(env, "LIH_MM2_TILT_DELTA", 0.25);
        bool lih_mm2_tilt_side_follow = env_flag_true(env, "LIH_MM2_TILT_SIDE_FOLLOW", false);
        bool lih_mm2_cheaper_later = env_flag_true(env, "LIH_MM2_CHEAPER_LATER", false);
        // Fav-early clock bypass (research gate B/C). Default OFF — enable only in shadow.
        bool lih_mm2_fav_early_bypass = env_flag_true(env, "LIH_MM2_FAV_EARLY_BYPASS", false);
        std::string lih_mm2_fav_early_mode = env.count("LIH_MM2_FAV_EARLY_MODE")
            ? env["LIH_MM2_FAV_EARLY_MODE"] : "B";
        double lih_mm2_fav_early_fav_lo = env_double_or(env, "LIH_MM2_FAV_EARLY_FAV_LO", 0.52);
        double lih_mm2_fav_early_fav_hi = env_double_or(env, "LIH_MM2_FAV_EARLY_FAV_HI", 0.65);
        double lih_mm2_fav_early_min_spread = env_double_or(env, "LIH_MM2_FAV_EARLY_MIN_SPREAD", 0.15);
        double lih_mm2_fav_early_min_dtilt = env_double_or(env, "LIH_MM2_FAV_EARLY_MIN_DTILT", 0.10);
        bool lih_mm2_obs_skip_from_pack = env_flag_true(env, "LIH_MM2_OBS_SKIP_FROM_PACK", false);
        std::string lih_mm2_obs_m2_root = env.count("LIH_MM2_OBS_M2_ROOT")
            ? env["LIH_MM2_OBS_M2_ROOT"] : "m2";
        // Follow m2 leg1 cue from pack (side + timing); hedge still bot LIH.
        bool lih_mm2_replay_leg1 = env_flag_true(env, "LIH_MM2_REPLAY_LEG1", false);
        double lih_mm2_replay_lag_tol = env_double_or(env, "LIH_MM2_REPLAY_LAG_TOL_SEC", 15.0);
        if (lih_mm2_replay_leg1) {
            // Replay replaces session/flat/obs entry gates; keep hedge mm2 params.
            lih_mm2_session_from_obs = false;
            lih_mm2_obs_skip_from_pack = false;
            lih_mm2_skip_flat = false;
        }
        if (lih_mm2_align || lih_mm2_mode) {
            lih_open_gap = true;
            if (!env.count("LIH_MAX_GAP_SHARES")) lih_max_gap_shares = 150.0;
            if (!env.count("LIH_GAP_HEDGE_MAX_COMBINED")) lih_gap_hedge_max_combined = 1.05;
            if (!env.count("LIH_FORCE_BALANCE_SECS")) lih_force_balance_secs = 0.0;
            if (!env.count("LIH_ENDGAME_MINIMIZE_GAP")) lih_endgame_minimize_gap = false;
            if (!env.count("LIH_ALLOW_OVER_TARGET")) lih_allow_over_target = false;
            if (!env.count("LIH_HEDGE_MIN_GAP_TRIGGER")) lih_hedge_min_gap_trigger = 90.0;
            if (!env.count("LIH_HEDGE_TARGET_MIN_GAP")) lih_hedge_target_min_gap = 144.0;
        }
        if (lih_mm2_align) {
            if (!env.count("LIH_LEG1_ORDER_MODE")) lih_leg1_order_mode = "gtc";
        }
        if (lih_mm2_mode) {
            if (!env.count("LIH_MM2_ENTRY_MAX_SECS_LEFT")) lih_mm2_entry_max_secs_left = 180.0;
            if (!env.count("LIH_MM2_ENTRY_MIN_SECS_LEFT")) lih_mm2_entry_min_secs_left = 45.0;
            if (!env.count("LIH_MM2_FAVORITE_MIN")) lih_mm2_favorite_min = 0.08;
            if (!env.count("LIH_MM2_MIN_SPOT_BPS")) lih_mm2_min_spot_bps = 8.0;
            // Session UTC gate is opt-in only (LIH_MM2_SESSION_UTC_*); bot runs 24h — compare mm2 via same-window overlap.
        }
        if (lih_open_gap) {
            lih_parallel_clip_hedge = true;
        }
        bool regime_gate_enabled = env_flag_true(env, "REGIME_GATE_ENABLED", false);
        double regime_thresh_b = env_double_or(env, "REGIME_THRESH_B", 0.75);
        double regime_thresh_c = env_double_or(env, "REGIME_THRESH_C", 0.70);
        double regime_b_jump_bps = env_double_or(env, "REGIME_B_JUMP_BPS", 25.0);
        double regime_b_window_bps = env_double_or(env, "REGIME_B_WINDOW_BPS", 40.0);
        double regime_c_ask_sum_bad = env_double_or(env, "REGIME_C_ASK_SUM_BAD", 0.015);
        double regime_c_min_depth = env_double_or(env, "REGIME_C_MIN_DEPTH", 50.0);
        double regime_hedge_margin = env_double_or(env, "REGIME_HEDGE_MARGIN", 0.03);
        int regime_pre_cooldown_sec = static_cast<int>(env_double_or(env, "REGIME_PRE_COOLDOWN_SEC", 1800.0));
        int regime_pre_extend_sec = static_cast<int>(env_double_or(env, "REGIME_PRE_EXTEND_SEC", 900.0));
        int regime_pre_max_sec = static_cast<int>(env_double_or(env, "REGIME_PRE_MAX_SEC", 7200.0));
        std::string regime_pre_state_file = env.count("REGIME_STATE_FILE")
            ? env["REGIME_STATE_FILE"] : "data/regime/regime_pre.json";
        std::string mirror_path = env.count("LIVE_MIRROR_PATH") ? env["LIVE_MIRROR_PATH"] : "logs/live_mirror.json";

        const std::string strategy = "leg_in";

        // --- H. Market feeds & optional depth/slippage sim (legacy) ---
        // Spot for LIH/mm2: Chainlink via Polymarket RTDS (settlement oracle). Binance off by default.
        bool chainlink_feed_enabled = true;
        if (env.count("CHAINLINK_FEED_ENABLED")) {
            std::string cf = env["CHAINLINK_FEED_ENABLED"];
            std::transform(cf.begin(), cf.end(), cf.begin(), ::tolower);
            chainlink_feed_enabled = !(cf == "false" || cf == "0" || cf == "no" || cf == "off");
        }
        bool binance_feed_enabled = false;
        if (env.count("BINANCE_FEED_ENABLED")) {
            std::string bf = env["BINANCE_FEED_ENABLED"];
            std::transform(bf.begin(), bf.end(), bf.begin(), ::tolower);
            binance_feed_enabled = !(bf == "false" || bf == "0" || bf == "no" || bf == "off");
        }
        if (chainlink_feed_enabled && binance_feed_enabled) {
            spdlog::warn("CHAINLINK + BINANCE both enabled — preferring Chainlink (settlement), disabling Binance");
            binance_feed_enabled = false;
        }
        bool book_aware_detect = env.count("BOOK_AWARE_DETECT")
            ? env_flag_true(env, "BOOK_AWARE_DETECT", true)
            : env_flag_true(env, "DH_BOOK_AWARE_DETECT", true);
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

        spdlog::info(
            "Strategy: {} | LIH: on | max_pos={:.0f}% | Chainlink spot: {} | Binance: {} | Book-aware: {}",
            strategy,
            max_pos * 100.0,
            chainlink_feed_enabled ? "on" : "off",
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
        {
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
                "LIH config | leg1_mode={} quote={} leg1<={:.2f} trigger>={:.2f} trigger_max<={:.2f} trend_max<={:.2f} target<={:.2f} entry={:.1f} "
                "leg1_delay={:.0f}s mode={} dilute={:.2f} "
                "leg1_min={:.0f}s hedge_min={:.0f}s force={:.0f}s trend_align={} lookback={:.0f}s "
                "endgame={:.0f}s hold>={:.2f} soft_cap={:.2f} step={:.0f}/{:.0f} override={:.0f}s "
                "leg1_cd={} rebal_cd={} max_rebal_sh={} max_matched_sh={} slot_cap={} "
                "pause_after_round={} session_legs={}",
                lih_leg1_trigger_mode ? "trigger" : (lih_leg1_trend_mode ? "trend" : (lih_mm2_mode ? "mm2" : "cheap")),
                lih_quote_mode.c_str(),
                lih_leg1_max, lih_leg1_trigger_min,
                lih_leg1_trigger_max > 1e-6 ? lih_leg1_trigger_max : 0.0,
                lih_leg1_trend_max, lih_target_combined, lih_leg1_shares, lih_leg1_start_delay,
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
        risk::RiskManager risk_manager(starting_balance, max_pos, daily_loss, drawdown, max_concurrent);
        risk_manager.set_fee_rate(fee_rate);
        risk_manager.set_lih_max_matched_shares(lih_max_matched_shares);
        risk_manager.set_lih_max_usdc_per_slot(lih_max_usdc_per_slot);
        risk_manager.set_lih_one_slot_global(lih_one_slot_global);
        risk_manager.set_lih_session_max_legs(lih_session_max_legs);
        risk_manager.set_lih_pause_after_round(lih_pause_after_round);
        risk_manager.set_lih_min_balance_usdc(lih_min_balance_usdc);
        if (shadow_virtual_bankroll) {
            risk_manager.set_shadow_virtual_bankroll(true);
            store.push_telemetry(fmt::format(
                "SHADOW bankroll ${:.0f} (wallet ${:.2f}, sync off)",
                shadow_bankroll_usdc, wallet_balance_usdc));
        }
        if (!paper_mode && lih_min_balance_usdc > 0.0 &&
            starting_balance + 1e-6 < lih_min_balance_usdc) {
            spdlog::warn("[LIH] Wallet ${:.2f} below LIH_MIN_BALANCE_USDC=${:.2f} — new leg1 blocked until topped up",
                         starting_balance, lih_min_balance_usdc);
        }

        // --- K. 状态持久化：实盘 live_state.json ---
        std::string live_state_path = env.count("LIVE_STATE_PATH") ? env["LIVE_STATE_PATH"] : "logs/live_state.json";
        g_live_state_reload_path = live_state_path;
        bool live_state_persist = env_flag_true(env, "LIVE_STATE_PERSIST", true);

        if (live_state_persist) {
            if (persistence::load_live_lih_state(risk_manager, live_state_path, live_lih_dry_run)) {
                if (!live_lih_dry_run) {
                    spdlog::info("Live LIH state loaded from {}", live_state_path);
                }
            } else {
                spdlog::info("Live LIH state: fresh session (no snapshot at {})", live_state_path);
            }
            risk_manager.purge_paper_positions();
        }
        if (shadow_virtual_bankroll) {
            risk_manager.set_live_starting_balance(shadow_bankroll_usdc);
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
        store.set_lih_enabled(true);
        store.set_lih_config(lih_leg1_max, lih_target_combined, lih_use_mirror);
        store.set_lih_mid_soft_cap(lih_mid_soft_cap);
        store.set_lih_leg1_mode(lih_mm2_mode ? "mm2"
            : (lih_leg1_trigger_mode ? "trigger" : (lih_leg1_trend_mode ? "trend" : "cheap")));
        store.set_lih_leg1_trend_max_price(lih_leg1_trend_max);
        store.set_lih_leg1_trigger_min(lih_leg1_trigger_min);
        store.set_lih_leg1_trigger_max(lih_leg1_trigger_max);
        store.set_lih_leg1_order_mode(lih_leg1_order_mode);
        store.set_lih_quote_mode(lih_quote_mode);
        store.set_live_lih_dry_run(live_lih_dry_run);
        store.set_mirror_path(mirror_path);
        if (env.count("LIVE_TRADES_BASELINE_TS")) {
            try {
                const double baseline = std::stod(env["LIVE_TRADES_BASELINE_TS"]);
                if (baseline > 0) store.set_trades_baseline_ts(baseline);
            } catch (...) {}
        }

        // --- N. OrderRouter: live CLOB (NegRisk dual signer) ---
        exec::OrderRouter router(feed_ioc, feed_ctx, store, risk_manager, polymarket_host, polymarket_chain_id, verifying_contract, polymarket_pk, polymarket_signer, polymarket_funder, paper_mode, poly_api_key, poly_api_secret, poly_api_passphrase, neg_risk_exchange, live_lih_dry_run, use_python_clob, clob_bridge_host, clob_bridge_port, clob_bridge_path);

        // --- O. 外部客户端：Gamma + Chainlink RTDS（结算同源）+ 可选 Binance ---
        GammaClient gamma(gamma_ioc, gamma_ctx);
        std::shared_ptr<ChainlinkFeed> chainlink_feed;
        if (chainlink_feed_enabled) {
            chainlink_feed = std::make_shared<ChainlinkFeed>(feed_ioc, feed_ctx, store);
        }
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

        // --- P. 策略检测器：LegInHedge（分腿 LIH）---
        std::mutex detector_mutex;
        std::unique_ptr<LegInHedgeDetector> lih_detector;
        const bool shadow_window_log =
            live_lih_dry_run && env_flag_true(env, "SHADOW_WINDOW_LOG", true);
        std::unique_ptr<ShadowWindowRecorder> shadow_window_recorder;
        std::unique_ptr<RegimeGate> regime_gate;
        if (regime_gate_enabled) {
            regime_gate = std::make_unique<RegimeGate>();
            regime_gate->set_enabled(true);
            regime_gate->set_store(&store);
            regime_gate->configure(
                regime_thresh_b, regime_thresh_c, lih_mm2_min_spot_bps, lih_target_combined,
                regime_b_jump_bps, regime_b_window_bps, regime_c_ask_sum_bad, regime_c_min_depth,
                regime_hedge_margin, regime_pre_cooldown_sec, regime_pre_extend_sec,
                regime_pre_max_sec, regime_pre_state_file);
            spdlog::info(
                "RegimeGate enabled | B>={:.2f} C>={:.2f} pre_file={}",
                regime_thresh_b, regime_thresh_c, regime_pre_state_file);
        }
        if (shadow_window_log) {
            shadow_window_recorder = std::make_unique<ShadowWindowRecorder>();
            shadow_window_recorder->set_enabled(true);
            shadow_window_recorder->set_store(&store);
            std::string sw_asset = env.count("SHADOW_WINDOW_ASSET") ? env["SHADOW_WINDOW_ASSET"] : "btc";
            std::transform(sw_asset.begin(), sw_asset.end(), sw_asset.begin(), ::tolower);
            const int sw_minutes = env.count("SHADOW_WINDOW_MINUTES")
                ? std::stoi(env["SHADOW_WINDOW_MINUTES"]) : 5;
            shadow_window_recorder->set_asset_filter(sw_asset, sw_minutes);
            risk_manager.set_shadow_window_recorder(shadow_window_recorder.get());
            spdlog::info(
                "Shadow window ledger on | asset={} {}m -> logs/shadow_windows.jsonl",
                sw_asset, sw_minutes);
        }

        // LIH actions: OrderRouter on live; legacy local sim path if paper_mode
        auto execute_lih_action = [&](const LegInAction& act, double now_sec) {
            if (!paper_mode) {
                const bool ok = router.submit_lih_action(act, now_sec);
                if (ok && live_state_persist && !live_lih_dry_run) {
                    persistence::save_live_lih_state(risk_manager, live_state_path, false);
                    try_live_chain_reconcile_async(risk_manager, live_state_path, true);
                }
                return;
            }
            auto slip_buy = [&](double px) {
                return apply_paper_slippage(px, true, paper_slippage_pct);
            };
            auto slip_sell = [&](double px, const LegInAction& a) {
                return apply_paper_slippage(px, false, paper_slippage_pct + paper_action_extra_slip(store, a));
            };
            constexpr double kLihMinUsdc = 1.0;
            switch (act.kind) {
            case LegInAction::Kind::OpenLeg1: {
                const std::string& tok = act.buy_yes ? act.market.yes_token_id : act.market.no_token_id;
                double shares = act.shares;
                double px = act.price;
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
            case LegInAction::Kind::AddLeg1: {
                const std::string& tok = act.buy_yes ? act.market.yes_token_id : act.market.no_token_id;
                double shares = act.shares;
                double px = act.price;
                const auto wf = store.walk_ask_fill(tok, act.shares);
                if (wf.shares <= 0.0 || wf.cost_usdc + 1e-6 < kLihMinUsdc) return;
                shares = wf.shares;
                px = wf.avg_price;
                px = apply_paper_slippage(px, true, paper_action_extra_slip(store, act));
                const double cost = shares * px;
                if (!risk_manager.can_open_lih_leg(cost, true, &act.lih_id, shares).first) return;
                if (!risk_manager.try_begin_lih_rebalance(act.lih_id)) return;
                risk_manager.register_lih_add_leg(act.lih_id, act.buy_yes, px, shares, true, true);
                store.push_signal(fmt::format("LIH CLIP {} {} +{:.2f}sh @ {:.4f} ({})",
                    act.market.asset, act.buy_yes ? "YES" : "NO", shares, px, act.note));
                break;
            }
            case LegInAction::Kind::CompleteHedge: {
                const std::string& tok = act.buy_yes ? act.market.yes_token_id : act.market.no_token_id;
                if (store.paper_realism_enabled()
                    && act.note.find("force") == std::string::npos
                    && paper_hedge_liquidity_miss(tok, now_sec, store.paper_hedge_fail_rate())) {
                    spdlog::info("[PAPER REALISM] hedge miss {} | {}", act.market.asset, act.note);
                    store.push_telemetry(fmt::format(
                        "[PAPER REALISM] hedge miss {} | {}", act.market.asset, act.note));
                    return;
                }
                double shares = act.shares;
                double px = act.price;
                const auto wf = store.walk_ask_fill(tok, act.shares);
                if (wf.shares <= 0.0 || wf.cost_usdc + 1e-6 < kLihMinUsdc) {
                    if (store.paper_realism_enabled()) {
                        spdlog::info("[PAPER REALISM] HEDGE miss {} | partial {:.2f}/{:.2f}sh",
                                     act.market.asset, wf.shares, act.shares);
                        store.push_telemetry(fmt::format(
                            "[PAPER REALISM] HEDGE miss {} | partial {:.2f}/{:.2f}sh",
                            act.market.asset, wf.shares, act.shares));
                    }
                    return;
                }
                shares = wf.shares;
                px = wf.avg_price;
                px = apply_paper_slippage(px, true, paper_action_extra_slip(store, act));
                spdlog::info("[LIH DEPTH] HEDGE {} {:.2f}/{:.2f}sh avg {:.4f} ({} lvls)",
                             act.market.asset, shares, act.shares, px, wf.levels_used);
                const double cost = shares * px;
                if (!risk_manager.can_open_lih_leg(cost, true, &act.lih_id, shares).first) return;
                if (!risk_manager.try_begin_lih_rebalance(act.lih_id)) return;
                risk_manager.register_lih_add_leg(act.lih_id, act.buy_yes, px, shares);
                store.push_signal(fmt::format("LIH HEDGE {} {} {:.2f}sh @ {:.4f} ({})",
                    act.market.asset, act.buy_yes ? "YES" : "NO", shares, px, act.note));
                break;
            }
            case LegInAction::Kind::UnwindLeg1: {
                double shares = act.shares;
                double px = act.price;
                px = slip_sell(apply_paper_slippage(px, false, paper_action_extra_slip(store, act)), act);
                if (px <= 0.0) return;
                if (!risk_manager.try_begin_lih_rebalance(act.lih_id)) return;
                if (!risk_manager.register_lih_unwind(
                        act.lih_id, act.buy_yes, px, shares, true, true)) {
                    risk_manager.end_lih_rebalance_inflight(act.lih_id);
                    return;
                }
                store.push_signal(fmt::format("LIH UNWIND {} sell {} {:.2f}sh @ {:.4f} ({})",
                    act.market.asset, act.buy_yes ? "YES" : "NO", shares, px, act.note));
                break;
            }
            }
        };

        // 每个 Polymarket tick 触发 LIH evaluate（LIH 模式）或 DH evaluate（DH 模式）
        auto try_lih_evaluate = [&]() {
            if (!lih_detector) return;
            std::lock_guard<std::mutex> lock(detector_mutex);
            const double now_ms = std::chrono::duration<double, std::milli>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            const double now_sec = now_ms / 1000.0;
            if (shadow_window_recorder) {
                shadow_window_recorder->flush_expired(now_sec);
            }
            if (auto act = lih_detector->evaluate(now_ms, risk_manager)) {
                execute_lih_action(*act, now_sec);
            }
        };

        auto poly_feed = std::make_shared<PolymarketFeed>(feed_ioc, feed_ctx, store);

        // Polymarket WS 价格推送回调 → LIH 信号检测 → 下单
        poly_feed->set_tick_callback([&](const std::string& /*token_id*/) {
            try_lih_evaluate();
        });

        // --- Q. 启动行情 Feed（回调注册完成后再 start，避免竞态）---
        if (chainlink_feed) {
            chainlink_feed->set_tick_callback([&](const std::string& /*asset*/, double /*px*/) {
                try_lih_evaluate();
            });
            chainlink_feed->start();
        }
        if (binance_feed_enabled) {
            btc_feed->start();
            eth_feed->start();
            sol_feed->start();
        }
        poly_feed->start();

        std::atomic<bool> is_refreshing{false};
        std::vector<std::string> rest_poll_tokens;
        const double process_boot_sec = std::chrono::duration<double>(
            std::chrono::system_clock::now().time_since_epoch()).count();

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
                    lih_detector = std::make_unique<LegInHedgeDetector>(
                        store, all_m, lih_leg1_max, lih_target_combined, lih_min_secs,
                        lih_leg1_min_secs, lih_leg1_start_delay,
                        lih_leg1_cooldown, lih_rebalance_cooldown,
                        lih_use_mirror, lih_leg1_shares, lih_allow_over_target,
                        lih_force_balance_secs, lih_max_rebalance_shares,
                        lih_flex_rebalance, lih_flex_dilute_ratio,
                        lih_leg1_trend_align, lih_trend_lookback_sec,
                        lih_leg1_trend_mode, lih_leg1_trend_max,
                        lih_leg1_trigger_mode, lih_leg1_trigger_min, lih_leg1_trigger_max,
                        lih_endgame_secs, lih_endgame_hold_ask, lih_endgame_resume_hedge_ask,
                        lih_endgame_soft_cap, lih_endgame_step_small, lih_endgame_step_large,
                        lih_endgame_gap_large, lih_endgame_override_secs,
                        lih_endgame_override_cooldown, lih_endgame_minimize_gap,
                        lih_endgame_ladder_enabled, lih_endgame_ladder_secs,
                        lih_endgame_ladder_start, lih_endgame_ladder_end,
                        lih_endgame_ladder_step, lih_max_entry_marginal,
                        lih_mid_soft_cap, lih_mid_soft_start_secs,
                        lih_hedge_feasible_entry, lih_hedge_feasible_cap);
                    lih_detector->set_vwap_entry_gate(lih_vwap_entry_gate);
                    lih_detector->set_vwap_entry_cap(lih_vwap_entry_cap);
                    lih_detector->set_vwap_depth_ratio(lih_vwap_depth_ratio);
                    lih_detector->set_min_edge_usdc(lih_min_edge_usdc);
                    lih_detector->set_min_edge_per_share(lih_min_edge_per_share);
                    lih_detector->set_leg1_shares(lih_leg1_shares);
                    lih_detector->set_leg1_clip_shares(lih_leg1_clip_shares);
                    lih_detector->set_unwind_enabled(lih_unwind_enabled);
                    lih_detector->set_unwind_secs(lih_unwind_secs);
                    lih_detector->set_unwind_cooldown(lih_unwind_cooldown);
                    lih_detector->set_parallel_clip_hedge(lih_parallel_clip_hedge);
                    lih_detector->set_parallel_hedge_max_combined(lih_parallel_hedge_max_combined);
                    lih_detector->set_early_hedge_max_combined(lih_early_hedge_max_combined);
                    lih_detector->set_open_gap_mode(lih_open_gap);
                    lih_detector->set_heavy_clip_shares(lih_heavy_clip_shares);
                    lih_detector->set_heavy_max_price(lih_heavy_max_price);
                    lih_detector->set_max_gap_shares(lih_max_gap_shares);
                    lih_detector->set_gap_hedge_max_combined(lih_gap_hedge_max_combined);
                    lih_detector->set_hedge_min_gap_trigger(lih_hedge_min_gap_trigger);
                    lih_detector->set_hedge_target_min_gap(lih_hedge_target_min_gap);
                    lih_detector->set_mm2_mode(lih_mm2_mode);
                    lih_detector->set_mm2_min_spot_bps(lih_mm2_min_spot_bps);
                    lih_detector->set_mm2_entry_max_secs_left(lih_mm2_entry_max_secs_left);
                    lih_detector->set_mm2_entry_min_secs_left(lih_mm2_entry_min_secs_left);
                    lih_detector->set_mm2_favorite_min_px(lih_mm2_favorite_min);
                    lih_detector->set_mm2_soft_spot_bps(lih_mm2_soft_spot_bps);
                    lih_detector->set_mm2_late_tilt_min_ask(lih_mm2_late_tilt_min_ask);
                    lih_detector->set_mm2_late_tilt_min_spread(lih_mm2_late_tilt_min_spread);
                    lih_detector->set_mm2_heavy_delay_sec(lih_mm2_heavy_delay_sec);
                    lih_detector->set_mm2_scale_clip_shares(lih_mm2_scale_clip);
                    lih_detector->set_mm2_heavy_max_shares(lih_mm2_heavy_max_shares);
                    lih_detector->set_mm2_scale_boost(lih_mm2_scale_boost);
                    lih_detector->set_mm2_scale_against_stop_bps(lih_mm2_scale_against_stop_bps);
                    lih_detector->set_mm2_hf1e_latch_gap(lih_mm2_hf1e_latch_gap);
                    lih_detector->set_mm2_hf1e_latch_secs_left(lih_mm2_hf1e_latch_secs_left);
                    lih_detector->set_mm2_hedge_boost(lih_mm2_hedge_boost);
                    lih_detector->set_mm2_early_entry_max_secs_left(lih_mm2_early_entry_max);
                    lih_detector->set_mm2_early_tilt_min_spread(lih_mm2_early_tilt_spread);
                    lih_detector->set_mm2_early_tilt_min_fav(lih_mm2_early_tilt_fav);
                    lih_detector->set_mm2_early_yes_guard(lih_mm2_early_yes_guard);
                    // Re-read secondary gates from .env on every market refresh so a
                    // mid-run .env patch (or incomplete first boot) cannot leave defaults.
                    // Use env_int (no throw) — a bad MAIN_BJ_* must not abort the refresh.
                    try {
                        const auto env_now = load_env(".env");
                        lih_mm2_main_bj_start =
                            env_int(env_now, "LIH_MM2_MAIN_BJ_START", 8, 0, 23);
                        lih_mm2_main_bj_end =
                            env_int(env_now, "LIH_MM2_MAIN_BJ_END", 17, 0, 24);
                        lih_mm2_offhours_min_ask =
                            env_double_or(env_now, "LIH_MM2_OFFHOURS_MIN_ASK", 0.0);
                        lih_mm2_min_side_depth =
                            env_double_or(env_now, "LIH_MM2_MIN_SIDE_DEPTH", 0.0);
                        lih_mm2_min_side_depth_main_only =
                            env_flag_true(env_now, "LIH_MM2_MIN_SIDE_DEPTH_MAIN_ONLY", true);
                    } catch (const std::exception& e) {
                        spdlog::warn("secondary gate env re-read failed: {}", e.what());
                    }
                    lih_detector->set_mm2_main_bj_hours(lih_mm2_main_bj_start, lih_mm2_main_bj_end);
                    lih_detector->set_mm2_offhours_min_ask(lih_mm2_offhours_min_ask);
                    lih_detector->set_mm2_min_side_depth(lih_mm2_min_side_depth);
                    lih_detector->set_mm2_min_side_depth_main_only(lih_mm2_min_side_depth_main_only);
                    store.set_mm2_secondary_gates(
                        lih_mm2_main_bj_start, lih_mm2_main_bj_end, lih_mm2_offhours_min_ask);
                    {
                        const auto gate_line = fmt::format(
                            "CONFIG SECONDARY main_bj=[{},{}) offhours_min_ask={:.4f} "
                            "min_side_depth={:.1f} depth_main_only={}",
                            lih_mm2_main_bj_start, lih_mm2_main_bj_end,
                            lih_mm2_offhours_min_ask, lih_mm2_min_side_depth,
                            lih_mm2_min_side_depth_main_only ? "true" : "false");
                        spdlog::info("LIH secondary gates | {}", gate_line);
                        store.push_telemetry(gate_line);
                        // Durable proof (telemetry ring is only 100 lines; bridge.log
                        // only persists LIVE LIH SHADOW from telemetry).
                        try {
                            std::ofstream gate_f("logs/secondary_gates.last",
                                                 std::ios::out | std::ios::trunc);
                            if (gate_f) gate_f << gate_line << '\n';
                        } catch (...) {
                        }
                    }
                    lih_detector->set_mm2_spot_leg1_max_usdc(lih_mm2_spot_leg1_max_usdc);
                    lih_detector->set_mm2_spot_heavy_max_usdc(lih_mm2_spot_heavy_max_usdc);
                    lih_detector->set_mm2_session_utc_hours(
                        lih_mm2_session_utc_start, lih_mm2_session_utc_end);
                    lih_detector->set_mm2_skip_flat(lih_mm2_skip_flat);
                    lih_detector->set_mm2_flat_max_spot_bps(lih_mm2_flat_max_spot_bps);
                    lih_detector->set_mm2_flat_max_ask_sum(lih_mm2_flat_max_ask_sum);
                    lih_detector->set_mm2_vol_gate(lih_mm2_vol_gate);
                    lih_detector->set_mm2_vol_min_spot_bps(lih_mm2_vol_min_spot_bps);
                    lih_detector->set_mm2_vol_max_spot_bps(lih_mm2_vol_max_spot_bps);
                    lih_detector->set_mm2_vol_max_spot_std(lih_mm2_vol_max_spot_std);
                    lih_detector->set_mm2_v2j_adaptive(lih_mm2_v2j_adaptive);
                    lih_detector->set_mm2_v2j_mom_lookback_sec(lih_mm2_v2j_mom_lookback);
                    lih_detector->set_mm2_v2j_mom_fav_max_ask(lih_mm2_v2j_mom_fav_max);
                    lih_detector->set_mm2_v2j_spread_min(lih_mm2_v2j_spread_min);
                    lih_detector->set_mm2_v2j_spread_fav_max_ask(lih_mm2_v2j_spread_fav_max);
                    lih_detector->set_mm2_tilt_entry(lih_mm2_tilt_entry);
                    lih_detector->set_mm2_tilt_delta(lih_mm2_tilt_delta);
                    lih_detector->set_mm2_tilt_side_follow(lih_mm2_tilt_side_follow);
                    lih_detector->set_mm2_cheaper_later(lih_mm2_cheaper_later);
                    lih_detector->set_mm2_fav_early_bypass(lih_mm2_fav_early_bypass);
                    lih_detector->set_mm2_fav_early_mode(lih_mm2_fav_early_mode);
                    lih_detector->set_mm2_fav_early_fav_lo(lih_mm2_fav_early_fav_lo);
                    lih_detector->set_mm2_fav_early_fav_hi(lih_mm2_fav_early_fav_hi);
                    lih_detector->set_mm2_fav_early_min_spread(lih_mm2_fav_early_min_spread);
                    lih_detector->set_mm2_fav_early_min_dtilt(lih_mm2_fav_early_min_dtilt);
                    lih_detector->set_mm2_obs_skip_from_pack(lih_mm2_obs_skip_from_pack);
                    lih_detector->set_mm2_replay_leg1(lih_mm2_replay_leg1);
                    lih_detector->set_mm2_replay_lag_tol_sec(lih_mm2_replay_lag_tol);
                    if (lih_mm2_session_from_obs && lih_mm2_mode) {
                        if (!lih_detector->load_mm2_session_from_pack(lih_mm2_obs_m2_root)) {
                            lih_detector->load_mm2_session_file(lih_mm2_session_file);
                        }
                    } else if (lih_mm2_mode && lih_mm2_session_explicit) {
                        lih_detector->load_mm2_session_file(lih_mm2_session_file);
                    }
                    if (lih_mm2_obs_skip_from_pack && lih_mm2_mode) {
                        lih_detector->load_mm2_obs_skip_pack(lih_mm2_obs_m2_root);
                    }
                    if (lih_mm2_replay_leg1 && lih_mm2_mode) {
                        lih_detector->load_mm2_replay_leg1_pack(lih_mm2_obs_m2_root);
                    }
                    lih_detector->set_skip_partial_window_on_start(lih_skip_partial_window);
                    lih_detector->set_process_boot_sec(process_boot_sec);
                    if (shadow_window_recorder) {
                        lih_detector->set_shadow_window_recorder(shadow_window_recorder.get());
                    }
                    if (regime_gate) {
                        lih_detector->set_regime_gate(regime_gate.get());
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
        auto last_session_refresh = std::chrono::system_clock::now();
        const int session_refresh_sec =
            (lih_mm2_session_from_obs || lih_mm2_obs_skip_from_pack || lih_mm2_replay_leg1) ? 60 : 0;
        
        // --- S. 后台线程：实盘余额同步（fetch_balance.py，间隔 WALLET_SYNC_INTERVAL_SEC）---
        std::thread balance_thread([&, wallet_sync_interval_sec, shadow_virtual_bankroll]() {
            while (true) {
                if (!paper_mode && !shadow_virtual_bankroll) {
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
                !rest_book_refreshing.load(std::memory_order_acquire) &&
                loop_start - last_rest_book_poll >
                    std::chrono::milliseconds(store.lih_quote_rest_only() ? 2000 : 2500)) {
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
            if (session_refresh_sec > 0 && lih_detector
                && loop_start - last_session_refresh > std::chrono::seconds(session_refresh_sec)) {
                last_session_refresh = loop_start;
                std::lock_guard<std::mutex> lock(detector_mutex);
                if (lih_detector) {
                    if (lih_mm2_session_from_obs) {
                        if (!lih_detector->load_mm2_session_from_pack(lih_mm2_obs_m2_root)) {
                            lih_detector->load_mm2_session_file(lih_mm2_session_file);
                        }
                    }
                    if (lih_mm2_obs_skip_from_pack) {
                        lih_detector->load_mm2_obs_skip_pack(lih_mm2_obs_m2_root);
                    }
                    if (lih_mm2_replay_leg1) {
                        lih_detector->load_mm2_replay_leg1_pack(lih_mm2_obs_m2_root);
                    }
                }
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
            apply_runtime_config("logs/runtime_config.json", risk_manager, store, detector_mutex, lih_detector);
            {
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
                    (live_state_persist && !live_lih_dry_run && !paper_mode)
                        ? &live_state_path : nullptr);
                try_lih_evaluate(); // 主循环也跑 LIH（不依赖 tick）
            }
            if (live_state_persist && !live_lih_dry_run && !paper_mode
                && loop_start - last_chain_reconcile > std::chrono::seconds(lih_chain_reconcile_sec)) {
                last_chain_reconcile = loop_start;
                try_live_chain_reconcile_async(risk_manager, live_state_path, false);
            }
            if (live_state_persist && !live_lih_dry_run
                && loop_start - last_live_save > std::chrono::seconds(10)) {
                last_live_save = loop_start;
                persistence::save_live_lih_state(risk_manager, live_state_path, false);
            }
            try {
                auto dj = boost::json::parse(store.get_dashboard_json());
                if (dj.is_object() && regime_gate) {
                    regime_gate->append_dashboard(dj.as_object());
                }
                std::cout << boost::json::serialize(dj) << std::endl;
            } catch (const std::exception& e) {
                spdlog::warn("dashboard json merge failed: {}", e.what());
                std::cout << store.get_dashboard_json() << std::endl;
            }
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
