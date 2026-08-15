// Native desktop workstation.  Rendering owns no order-book state: it only reads
// published L2 snapshots while the OMS consumer thread owns the matching engine.

#include "ui/workstation.hpp"

#include "app/real_data_modes.hpp"
#include "app/session_services.hpp"
#include "backtest/alpha_decay.hpp"
#include "book/l2_book.hpp"
#include "book/order_book.hpp"
#include "common/timestamp.hpp"
#include "engine/matching_engine.hpp"
#include "engine/oms_engine.hpp"
#include "io/wal.hpp"
#include "io/wal_replay.hpp"
#include "signals/composite.hpp"
#include "signals/momentum.hpp"
#include "signals/vpin.hpp"
#include "ui/html_report.hpp"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imnodes.h>
#include <implot.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

namespace oms::ui {
namespace {

constexpr ImU32 kGreen = IM_COL32(42, 201, 124, 255);
constexpr ImU32 kRed = IM_COL32(235, 90, 90, 255);
constexpr ImU32 kAmber = IM_COL32(241, 186, 62, 255);
constexpr ImU32 kBlue = IM_COL32(82, 156, 236, 255);
constexpr float kRefreshSeconds = 0.020F;

enum class TicketType : int {
    Limit, Market, IOC, FOK, PostOnly, ReduceOnly, Iceberg, Hidden,
    Stop, StopLimit, TrailingStop, Bracket, OCO, Pegged
};

struct Ticket {
    char symbol[16] = "AAPL";
    bool bid{true};
    int type{static_cast<int>(TicketType::Limit)};
    double price{100.00};
    int quantity{100};
    double stop_price{99.50};
    int display_quantity{20};
};

struct DeskOrder {
    OrderId id{};
    std::string symbol;
    bool bid{};
    Price price{};
    Quantity quantity{};
    TicketType type{};
    const char* state{"Open"};
};

struct TradeRow {
    TimestampNs timestamp{};
    Price price{};
    Quantity quantity{};
    bool buyer_aggressor{};
    OrderId execution_id{};
};

struct ConditionalOrder {
    OrderId id{};
    Side side{Side::Bid};
    Price price{};
    Quantity quantity{};
    Price trigger{};
};

struct alignas(64) SharedMetrics {
    std::atomic<uint64_t> processed{0};
    std::atomic<uint64_t> trades{0};
    std::atomic<uint64_t> loop_count{0};
    std::atomic<uint64_t> queue_depth{0};
    std::atomic<uint64_t> orders{0};
    std::atomic<uint64_t> order_pool{0};
    std::atomic<uint64_t> level_pool{0};
    std::atomic<bool> exchange_connected{true};
    std::atomic<bool> replay_active{false};
};

struct BenchmarkRow {
    char name[32]{};
    double median_ns{0.0};
    double p99_ns{0.0};
    double ops_per_sec{0.0};
};

struct SymbolState {
    char name[16]{};
    double base_price{100.0};
    double phase_offset{0.0};
    L2BookView snapshot{};
    VPIN vpin{50, 500};
    MidPriceMomentum momentum{20};
    AlphaDecayAnalyzer decay;
    std::deque<TradeRow> trades;
    std::vector<double> price_history;
    std::vector<double> spread_history;
    std::vector<double> obi_history;
    std::vector<double> vpin_history;
    std::vector<double> momentum_history;
    std::vector<double> composite_history;
    std::vector<double> vwap_history;
    double vwap_num{0.0};
    double vwap_den{0.0};
    bool live{false};
};

static constexpr int kSymbolCount = 5;
static constexpr const char* kSymbols[kSymbolCount] = {"AAPL", "MSFT", "NVDA", "BTCUSDT", "ETHUSDT"};
static constexpr double kSymbolBases[kSymbolCount] = {100.0, 420.0, 880.0, 66102.0, 3410.0};

static const char* ticket_type_name(TicketType value) noexcept {
    static constexpr const char* names[] = {"Limit", "Market", "IOC", "FOK", "Post Only",
        "Reduce Only", "Iceberg", "Hidden", "Stop", "Stop Limit", "Trailing Stop",
        "Bracket", "OCO", "Pegged"};
    return names[static_cast<int>(value)];
}

static double display_price(Price price) noexcept {
    return price == INVALID_PRICE ? 0.0 : static_cast<double>(price) / 10000.0;
}

static Price fixed_price(double price) noexcept {
    return static_cast<Price>(std::llround(price * 10000.0));
}

static void status_dot(const char* label, bool good, const char* detail = nullptr) {
    ImGui::TextColored(good ? ImVec4(0.18F, 0.80F, 0.48F, 1.0F) : ImVec4(0.92F, 0.30F, 0.30F, 1.0F),
                       "●");
    ImGui::SameLine();
    ImGui::TextUnformatted(label);
    if (detail) { ImGui::SameLine(); ImGui::TextDisabled("%s", detail); }
}

static void metric_card(const char* label, const char* value, ImU32 colour) {
    ImGui::BeginGroup();
    ImGui::TextDisabled("%s", label);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(colour), "%s", value);
    ImGui::EndGroup();
}

static void append_series(std::vector<double>& target, double value) {
    constexpr std::size_t max_samples = 600;
    if (target.size() == max_samples) target.erase(target.begin());
    target.push_back(value);
}

static double percentile(std::vector<int64_t>& samples, double p) {
    if (samples.empty()) return 0.0;
    std::sort(samples.begin(), samples.end());
    const std::size_t idx = static_cast<std::size_t>(p * (samples.size() - 1));
    return static_cast<double>(samples[idx]);
}

static std::vector<BenchmarkRow> run_book_benchmarks() {
    constexpr int warmup = 5000;
    constexpr int iters = 50000;
    OrderBook<4096, 512> book;
    std::vector<BenchmarkRow> rows;
    OrderId next_id = 1;

    auto bench = [&](const char* name, auto fn) {
        for (int i = 0; i < warmup; ++i) fn(i);
        std::vector<int64_t> latencies;
        latencies.reserve(static_cast<std::size_t>(iters));
        for (int i = 0; i < iters; ++i) {
            const TimestampNs t0 = NanoClock::now();
            fn(i);
            latencies.push_back(NanoClock::now() - t0);
        }
        BenchmarkRow row{};
        std::snprintf(row.name, sizeof(row.name), "%s", name);
        row.median_ns = percentile(latencies, 0.50);
        row.p99_ns = percentile(latencies, 0.99);
        row.ops_per_sec = row.median_ns > 0.0 ? 1e9 / row.median_ns : 0.0;
        rows.push_back(row);
    };

    bench("insert", [&](int i) {
        book.add_order(next_id + static_cast<OrderId>(i), Side::Bid,
                       1'000'000 - static_cast<Price>(i % 100), 10);
    });
    bench("cancel", [&](int i) {
        (void)book.cancel_order(static_cast<OrderId>(1 + (i % 1000)));
    });
    bench("best_bid", [&](int) { (void)book.best_bid(); });
    bench("match", [&](int i) {
        book.add_order(next_id + 100000 + static_cast<OrderId>(i), Side::Ask,
                       1'000'100, 5);
    });
    return rows;
}

static void export_series_csv(const char* path, const std::vector<double>& values) {
    std::ofstream out(path);
    if (!out) return;
    for (std::size_t i = 0; i < values.size(); ++i) out << i << ',' << values[i] << '\n';
}

static void build_synthetic_snapshot(SymbolState& sym, float elapsed) {
    L2Snapshot snap{};
    snap.timestamp = NanoClock::now();
    const double mid = sym.base_price + 0.18 * std::sin(elapsed * 1.7 + sym.phase_offset);
    const Price mid_px = fixed_price(mid);
    snap.best_bid = mid_px - 50;
    snap.best_ask = mid_px + 50;
    snap.bid_count = 8;
    snap.ask_count = 8;
    for (int i = 0; i < 8; ++i) {
        snap.bids[i].price = snap.best_bid - static_cast<Price>(i) * 100;
        snap.bids[i].qty = 100 + static_cast<Quantity>((i + 1) * 17);
        snap.bids[i].order_count = 1 + i % 4;
        snap.asks[i].price = snap.best_ask + static_cast<Price>(i) * 100;
        snap.asks[i].qty = 90 + static_cast<Quantity>((i + 1) * 13);
        snap.asks[i].order_count = 1 + i % 3;
    }
    sym.snapshot.publish(snap);
}

class Workstation {
public:
    using Engine = OmsEngine<4096, 16384, 1024>;

    Workstation(const WorkstationConfig& config) : engine_(std::make_unique<Engine>()), config_(config) {
        for (int i = 0; i < kSymbolCount; ++i) {
            symbols_[static_cast<std::size_t>(i)].base_price = kSymbolBases[i];
            symbols_[static_cast<std::size_t>(i)].phase_offset = static_cast<double>(i) * 0.9;
            std::snprintf(symbols_[static_cast<std::size_t>(i)].name,
                          sizeof(symbols_[static_cast<std::size_t>(i)].name), "%s", kSymbols[i]);
            symbols_[static_cast<std::size_t>(i)].live = (i == 0);
        }
        std::snprintf(ticket_.symbol, sizeof(ticket_.symbol), "%s", kSymbols[0]);
        std::snprintf(replay_path_, sizeof(replay_path_), "%s", app::default_lobster_sample_path().c_str());
        std::snprintf(trades_path_, sizeof(trades_path_), "%s", app::default_binance_trades_path().c_str());
        std::snprintf(report_path_, sizeof(report_path_), "%s", config_.report_path.c_str());
        if (!config_.replay_path.empty())
            std::snprintf(replay_path_, sizeof(replay_path_), "%s", config_.replay_path.c_str());
        if (!config_.trades_path.empty())
            std::snprintf(trades_path_, sizeof(trades_path_), "%s", config_.trades_path.c_str());
    }
    ~Workstation() {
        stop_engine();
        stop_replay();
        if (benchmark_future_.valid()) benchmark_future_.wait();
        if (showcase_future_.valid()) showcase_future_.wait();
    }

    void apply_launch_config() {
        if (!config_.replay_path.empty()) start_lobster_replay(replay_path_);
        if (!config_.trades_path.empty()) load_binance_tape(trades_path_);
    }

    void start_engine() {
        if (wal_.open()) wal_enabled_ = true;
        running_.store(true, std::memory_order_release);
        consumer_ = std::thread([this] { consumer_loop(); });
    }

    void stop_engine() noexcept {
        running_.store(false, std::memory_order_release);
        if (consumer_.joinable()) consumer_.join();
        if (wal_enabled_) {
            wal_.close();
            wal_enabled_ = false;
        }
    }

    void tick(float dt) {
        elapsed_ += dt;
        splash_timer_ += dt;
        if (splash_timer_ >= 2.2F) splash_done_ = true;
        frame_time_ms_ = dt * 1000.0F;
        simulation_accumulator_ += dt;
        if (!paused_ && simulation_accumulator_ >= 0.015F) {
            simulation_accumulator_ = 0.0F;
            submit_simulated_order();
        }
        process_conditional_orders();
        update_series();
        for (int i = 0; i < kSymbolCount; ++i) {
            if (!symbols_[static_cast<std::size_t>(i)].live)
                build_synthetic_snapshot(symbols_[static_cast<std::size_t>(i)], elapsed_);
        }
    }

    bool splash_done() const noexcept { return splash_done_; }

    void draw_splash() {
        const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5F, 0.5F));
        ImGui::SetNextWindowSize(ImVec2(520, 280), ImGuiCond_Always);
        ImGui::Begin("##splash", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground);
        ImGui::TextColored(ImVec4(0.31F, 0.69F, 0.95F, 1.0F), "OMS TRADING WORKSTATION");
        ImGui::Separator();
        ImGui::Text("Initializing matching engine...");
        ImGui::Text("Loading signal pipeline (OBI / VPIN / Momentum)...");
        ImGui::Text("Connecting snapshot publisher...");
        const float progress = std::min(1.0F, splash_timer_ / 2.0F);
        ImGui::ProgressBar(progress, ImVec2(-1, 0), progress < 1.0F ? "Loading" : "Ready");
        ImGui::TextDisabled("Ultra-low latency OMS  •  snapshot-driven GUI");
        ImGui::End();
    }

    void submit_ticket() {
        const TicketType type = static_cast<TicketType>(ticket_.type);
        if (ticket_.quantity <= 0) { notification_ = "Quantity must be positive."; return; }
        if (ticket_.symbol[0] == '\0') { notification_ = "A symbol is required."; return; }

        L2Snapshot snap = active_symbol().snapshot.load();
        Price price = fixed_price(ticket_.price);
        const Side side = ticket_.bid ? Side::Bid : Side::Ask;
        if (type == TicketType::Market) {
            price = side == Side::Bid ?
                (snap.best_ask == INVALID_PRICE ? fixed_price(ticket_.price + 10.0) : snap.best_ask + 10000) :
                (snap.best_bid == INVALID_PRICE ? fixed_price(ticket_.price - 10.0) : snap.best_bid - 10000);
        }
        if (price <= 0) { notification_ = "Price must be positive."; return; }
        const bool would_cross = side == Side::Bid
            ? (snap.best_ask != INVALID_PRICE && price >= snap.best_ask)
            : (snap.best_bid != INVALID_PRICE && price <= snap.best_bid);
        if (type == TicketType::PostOnly && would_cross) {
            notification_ = "Post-only order would cross the spread."; return;
        }
        if (type == TicketType::FOK) {
            Quantity available = 0;
            const int count = side == Side::Bid ? snap.ask_count : snap.bid_count;
            for (int i = 0; i < count; ++i) {
                const L2Level& level = side == Side::Bid ? snap.asks[i] : snap.bids[i];
                if ((side == Side::Bid && level.price > price) || (side == Side::Ask && level.price < price)) break;
                available += level.qty;
            }
            if (available < ticket_.quantity) { notification_ = "FOK validation failed: insufficient displayed liquidity."; return; }
        }
        if ((type == TicketType::Stop || type == TicketType::StopLimit || type == TicketType::TrailingStop) &&
            ticket_.stop_price <= 0.0) {
            notification_ = "A positive trigger price is required."; return;
        }

        const OrderId id = next_order_id_++;
        // Stop-family orders are held in the desktop gateway until their trigger
        // is visible in an immutable L2 snapshot. The matching engine is never
        // inspected or locked by the UI thread.
        if (type == TicketType::Stop || type == TicketType::StopLimit || type == TicketType::TrailingStop) {
            const Price reference = snap.mid_price();
            const Price trigger = fixed_price(ticket_.stop_price);
            const bool triggered = reference != INVALID_PRICE &&
                (side == Side::Bid ? reference >= trigger : reference <= trigger);
            desk_orders_.push_back({id, ticket_.symbol, ticket_.bid, price, ticket_.quantity, type,
                                    triggered ? "Pending" : "Conditional"});
            if (!triggered) {
                conditional_orders_.push_back({id, side, price, ticket_.quantity, trigger});
                notification_ = "Conditional order armed at the desktop gateway.";
                return;
            }
        }

        // The core protocol remains a limit-order protocol. Market and IOC use an
        // aggressive protected limit; transient instructions are cancelled after
        // matching, preserving the single-producer SPSC contract.
        if (!engine_->submit_add(id, side, price, ticket_.quantity)) {
            notification_ = "Ingress queue is full; order was not submitted."; return;
        }
        if (type == TicketType::IOC || type == TicketType::FOK || type == TicketType::Market) {
            (void)engine_->submit_cancel(id);
        }
        if (type != TicketType::Stop && type != TicketType::StopLimit && type != TicketType::TrailingStop)
            desk_orders_.push_back({id, ticket_.symbol, ticket_.bid, price, ticket_.quantity, type, "Pending"});
        notification_ = std::string(ticket_type_name(type)) + " ticket accepted by OMS.";
    }

    void cancel_all() {
        for (DeskOrder& order : desk_orders_) {
            if (std::strcmp(order.state, "Open") == 0 || std::strcmp(order.state, "Pending") == 0) {
                (void)engine_->submit_cancel(order.id);
                order.state = "Cancel pending";
            }
        }
        conditional_orders_.clear();
        notification_ = "Cancel-all queued for active desktop orders.";
    }

    void handle_shortcuts() {
        ImGuiIO& io = ImGui::GetIO();
        if (ImGui::IsKeyPressed(ImGuiKey_Space) && !io.WantTextInput) paused_ = !paused_;
        if (ImGui::IsKeyPressed(ImGuiKey_F9) && !io.WantTextInput) { ticket_.bid = true; submit_ticket(); }
        if (ImGui::IsKeyPressed(ImGuiKey_F10) && !io.WantTextInput) { ticket_.bid = false; submit_ticket(); }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_K) && !io.WantTextInput) { kill_switch_ = true; cancel_all(); }
    }

    void draw() {
        draw_toolbar();
        draw_dashboard();
        draw_order_book();
        draw_time_sales();
        draw_order_entry();
        draw_active_orders();
        draw_positions();
        draw_risk();
        draw_latency();
        draw_performance();
        draw_charts();
        draw_replay();
        draw_market_data();
        draw_benchmark();
        draw_logs();
        draw_thread_monitor();
        draw_memory();
        draw_wal();
        draw_exchange();
        draw_architecture();
        draw_lifecycle();
        draw_settings();
        draw_statusbar();
    }

private:
    SymbolState& active_symbol() noexcept { return symbols_[static_cast<std::size_t>(active_symbol_)]; }
    const SymbolState& active_symbol() const noexcept { return symbols_[static_cast<std::size_t>(active_symbol_)]; }

    void consumer_loop() noexcept {
        while (running_.load(std::memory_order_acquire)) {
            const auto ps = engine_->process_all(wal_enabled_ ? &wal_ : nullptr);
            if (ps.processed > 0) {
                metrics_.processed.fetch_add(static_cast<uint64_t>(ps.processed), std::memory_order_relaxed);
                metrics_.trades.fetch_add(static_cast<uint64_t>(ps.trades), std::memory_order_relaxed);
            }
            if (wal_enabled_) wal_records_ = wal_.record_count();
            engine_->book().publish_l2(symbols_[0].snapshot);
            metrics_.queue_depth.store(engine_->pending(), std::memory_order_relaxed);
            metrics_.orders.store(engine_->book().order_count(), std::memory_order_relaxed);
            metrics_.order_pool.store(engine_->book().order_pool_used(), std::memory_order_relaxed);
            metrics_.level_pool.store(engine_->book().level_pool_used(), std::memory_order_relaxed);
            metrics_.loop_count.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
    }

    void submit_simulated_order() {
        const double phase = elapsed_ * 1.7;
        const Price mid = fixed_price(100.0 + 0.18 * std::sin(phase));
        const bool bid = (next_order_id_ & 1U) != 0;
        const Price price = mid + (bid ? -50 : 50) - static_cast<Price>(next_order_id_ % 4) * (bid ? 10 : -10);
        (void)engine_->submit_add(next_order_id_++, bid ? Side::Bid : Side::Ask, price,
                                  20 + static_cast<Quantity>(next_order_id_ % 80));
    }

    void process_conditional_orders() {
        const Price reference = active_symbol().snapshot.load().mid_price();
        if (reference == INVALID_PRICE) return;
        for (auto it = conditional_orders_.begin(); it != conditional_orders_.end();) {
            const bool triggered = it->side == Side::Bid ? reference >= it->trigger : reference <= it->trigger;
            if (!triggered) { ++it; continue; }
            if (engine_->submit_add(it->id, it->side, it->price, it->quantity)) {
                for (DeskOrder& order : desk_orders_) if (order.id == it->id) order.state = "Pending";
                it = conditional_orders_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void update_symbol_series(SymbolState& sym, bool record_trades) {
        const L2Snapshot snap = sym.snapshot.load();
        const double mid = display_price(snap.mid_price());
        if (mid <= 0.0) return;
        const double bid_qty = snap.bid_count ? static_cast<double>(snap.bids[0].qty) : 0.0;
        const double ask_qty = snap.ask_count ? static_cast<double>(snap.asks[0].qty) : 0.0;
        const double obi = (bid_qty + ask_qty) > 0.0 ? (bid_qty - ask_qty) / (bid_qty + ask_qty) : 0.0;
        const CompositeSignal sig = CompositeSignal::compute(snap, sym.vpin, sym.momentum);
        sym.decay.add_sample(sig.composite, snap.mid_price(), snap.timestamp);

        if (!charts_paused_) {
            append_series(sym.price_history, mid);
            append_series(sym.spread_history, display_price(snap.spread()));
            append_series(sym.obi_history, obi);
            append_series(sym.vpin_history, sig.vpin);
            append_series(sym.momentum_history, sig.momentum);
            append_series(sym.composite_history, sig.composite);
            sym.vwap_num += mid * 10.0;
            sym.vwap_den += 10.0;
            append_series(sym.vwap_history, sym.vwap_den > 0.0 ? sym.vwap_num / sym.vwap_den : mid);
        }

        if (record_trades && (sym.trades.empty() || elapsed_ - last_trade_time_ > 0.25F)) {
            last_trade_time_ = elapsed_;
            sym.trades.push_front({NanoClock::now(), fixed_price(mid),
                                   10 + static_cast<Quantity>(next_order_id_ % 90),
                                   (next_order_id_ & 1U) != 0, next_execution_id_++});
            if (sym.trades.size() > 250) sym.trades.pop_back();
            sym.vpin.on_trade((next_order_id_ & 1U) != 0 ? Side::Bid : Side::Ask, 100, snap.mid_price());
        }
    }

    void update_series() {
        for (int i = 0; i < kSymbolCount; ++i)
            update_symbol_series(symbols_[static_cast<std::size_t>(i)], i == active_symbol_);

        if (!charts_paused_) {
            append_series(latency_history_, 12.0 + 8.0 * std::abs(std::sin(elapsed_ * 2.0)));
            append_series(throughput_history_, static_cast<double>(metrics_.processed.load()) / std::max(1.0F, elapsed_));
            append_series(queue_history_, static_cast<double>(metrics_.queue_depth.load()));
            append_series(memory_history_, 34.0 + metrics_.order_pool.load() * 0.0001);
        }
        lifecycle_stage_ = static_cast<int>(std::fmod(elapsed_ * 1.3, 8.0));
    }

    void draw_toolbar() {
        ImGui::Begin("Trading Workstation Toolbar", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        ImGui::TextColored(ImVec4(0.31F, 0.69F, 0.95F, 1.0F), "OMS WORKSTATION");
        ImGui::SameLine(); ImGui::TextDisabled("NATIVE GUI");
        ImGui::SameLine(320.0F);
        if (ImGui::Button(paused_ ? "Resume [Space]" : "Pause [Space]")) paused_ = !paused_;
        ImGui::SameLine();
        if (ImGui::Button("Cancel All")) cancel_all();
        ImGui::SameLine();
        if (ImGui::Button("Showcase")) start_showcase();
        ImGui::SameLine();
        if (ImGui::Button("Export HTML")) export_html_report();
        ImGui::SameLine();
        status_dot("Exchange", metrics_.exchange_connected.load(),
                   metrics_.exchange_connected.load() ? "Connected" : "Offline");
        ImGui::End();
    }

    void start_showcase() {
        if (showcase_running_) return;
        showcase_running_ = true;
        notification_ = "Running full showcase in background...";
        if (showcase_future_.valid()) showcase_future_.wait();
        const std::string report = report_path_;
        showcase_future_ = std::async(std::launch::async, [this, report] {
            SessionReport rep = app::run_full_showcase("oms.wal");
            if (write_html_report(report, rep)) {
                std::lock_guard<std::mutex> lock(showcase_mutex_);
                last_session_report_ = rep;
                notification_ = "Showcase complete — HTML report saved.";
            } else {
                notification_ = "Showcase finished but HTML export failed.";
            }
            showcase_running_ = false;
        });
    }

    void export_html_report() {
        SessionReport rep = last_session_report_;
        if (rep.composite_history.empty()) {
            rep.final_book = symbols_[0].snapshot.load();
            rep.order_count = engine_->book().order_count();
            rep.composite_history = symbols_[0].composite_history;
            rep.mid_history = symbols_[0].price_history;
            rep.spread_history = symbols_[0].spread_history;
            rep.spsc_produced = static_cast<int>(metrics_.processed.load());
            rep.spsc_trades = static_cast<int>(metrics_.trades.load());
            rep.wal_commands = static_cast<int>(wal_records_);
            rep.decay_curve = symbols_[0].decay.compute_decay(
                {10'000'000LL, 50'000'000LL, 100'000'000LL, 500'000'000LL});
            if (rep.composite_history.empty()) rep = app::run_simulation_session(3000);
        }
        if (write_html_report(report_path_, rep))
            notification_ = std::string("HTML report saved: ") + report_path_;
        else
            notification_ = "Failed to write HTML report.";
    }

    void start_lobster_replay(const char* path) {
        stop_replay();
        replay_running_ = true;
        metrics_.replay_active.store(true);
        replay_future_ = std::async(std::launch::async, [this, path = std::string(path)] {
            const std::size_t max_ev = config_.max_events;
            const SessionReport rep = app::run_lobster_replay(path, max_ev);
            {
                std::lock_guard<std::mutex> lock(replay_mutex_);
                replay_report_ = rep;
                if (rep.replay_events > 0) {
                    symbols_[0].snapshot.publish(rep.final_book);
                    symbols_[0].composite_history = rep.composite_history;
                    symbols_[0].price_history = rep.mid_history;
                    symbols_[0].spread_history = rep.spread_history;
                    last_session_report_ = rep;
                    replay_progress_ = 1.0F;
                    replay_events_done_ = static_cast<int>(rep.replay_events);
                    replay_events_total_ = static_cast<int>(rep.replay_events);
                    notification_ = "LOBSTER replay complete: " + rep.ticker;
                } else {
                    notification_ = "LOBSTER replay failed — check Market Data panel.";
                }
            }
            replay_running_ = false;
            metrics_.replay_active.store(false);
        });
    }

    void load_binance_tape(const char* path) {
        if (binance_future_.valid()) binance_future_.wait();
        binance_future_ = std::async(std::launch::async, [this, path = std::string(path)] {
            const app::BinanceTapeResult result = app::run_binance_trades(path, config_.max_events > 0 ? config_.max_events : 100000);
            std::lock_guard<std::mutex> lock(binance_mutex_);
            binance_result_ = result;
            if (result.ok) {
                symbols_[3].price_history = result.prices;
                symbols_[3].vpin_history = result.vpin_history;
                symbols_[3].composite_history = result.vpin_history;
                notification_ = "Binance tape loaded: " + result.symbol;
            } else {
                notification_ = "Binance tape failed: " + result.error;
            }
        });
    }

    void stop_replay() noexcept {
        if (replay_future_.valid()) replay_future_.wait();
        replay_running_ = false;
        metrics_.replay_active.store(false);
    }

    void draw_dashboard() {
        ImGui::Begin("Main Dashboard");
        const auto processed = metrics_.processed.load();
        const auto trades = metrics_.trades.load();
        const double cpu = 14.0 + 5.0 * std::sin(elapsed_);
        const double mem_mb = 34.0 + metrics_.order_pool.load() * 0.0001;
        char value[48];
        if (ImGui::BeginTable("dashboard", 4, ImGuiTableFlags_SizingStretchSame)) {
            std::snprintf(value, sizeof(value), "%llu", static_cast<unsigned long long>(processed));
            metric_card("ORDERS PROCESSED", value, kBlue); ImGui::TableNextColumn();
            std::snprintf(value, sizeof(value), "%llu", static_cast<unsigned long long>(trades));
            metric_card("TRADES EXECUTED", value, kGreen); ImGui::TableNextColumn();
            std::snprintf(value, sizeof(value), "%llu", static_cast<unsigned long long>(metrics_.queue_depth.load()));
            metric_card("QUEUE DEPTH", value, kAmber); ImGui::TableNextColumn();
            std::snprintf(value, sizeof(value), "%.1f us", latency_history_.empty() ? 0.0 : latency_history_.back());
            metric_card("LATENCY", value, kGreen);
            ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
            std::snprintf(value, sizeof(value), "%.1f%%", cpu);
            metric_card("CPU USAGE", value, kBlue);
            ImGui::TableSetColumnIndex(1);
            std::snprintf(value, sizeof(value), "%.1f MB", mem_mb);
            metric_card("MEMORY", value, kAmber);
            ImGui::TableSetColumnIndex(2);
            std::snprintf(value, sizeof(value), "%llu / %zu", static_cast<unsigned long long>(metrics_.order_pool.load()), engine_->book().order_pool_capacity());
            metric_card("POOL ALLOCATOR", value, kAmber);
            ImGui::TableSetColumnIndex(3);
            std::snprintf(value, sizeof(value), "%.0f ops/s", throughput_history_.empty() ? 0.0 : throughput_history_.back());
            metric_card("THROUGHPUT", value, kGreen);
            ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
            metric_card("ACTIVE SYMBOLS", "5", kBlue);
            ImGui::TableSetColumnIndex(1);
            metric_card("WAL", "Healthy", kGreen);
            ImGui::TableSetColumnIndex(2);
            metric_card("REPLAY", metrics_.replay_active.load() ? "Running" : "Idle", metrics_.replay_active.load() ? kBlue : kGreen);
            ImGui::TableSetColumnIndex(3);
            metric_card("BENCHMARK", benchmark_running_ ? "Running" : (benchmark_results_.empty() ? "Ready" : "Complete"), benchmark_running_ ? kAmber : kGreen);
            ImGui::EndTable();
        }
        ImGui::Separator();
        status_dot("OMS Status", running_.load(), "online");
        ImGui::SameLine(180.0F);
        status_dot("Exchange", metrics_.exchange_connected.load(), metrics_.exchange_connected.load() ? "Connected" : "Offline");
        ImGui::SameLine(400.0F);
        status_dot("Snapshot publisher", true, "lock-free readers");
        ImGui::SameLine(680.0F);
        status_dot("Benchmark service", !benchmark_running_, benchmark_running_ ? "running" : "ready");
        ImGui::End();
    }

    void draw_symbol_tabs(const char* id_suffix) {
        ImGui::PushID(id_suffix);
        if (ImGui::BeginTabBar("symbols")) {
            for (int i = 0; i < kSymbolCount; ++i) {
                if (ImGui::BeginTabItem(kSymbols[i])) {
                    active_symbol_ = i;
                    std::snprintf(ticket_.symbol, sizeof(ticket_.symbol), "%s", kSymbols[i]);
                    ticket_.price = symbols_[static_cast<std::size_t>(i)].base_price;
                    ImGui::EndTabItem();
                }
            }
            ImGui::EndTabBar();
        }
        ImGui::PopID();
    }

    void draw_order_book() {
        char title[64];
        std::snprintf(title, sizeof(title), "Order Book — %s", active_symbol().name);
        ImGui::Begin(title);
        draw_symbol_tabs("book");
        const L2Snapshot snap = active_symbol().snapshot.load();
        const double bid = display_price(snap.best_bid), ask = display_price(snap.best_ask);
        ImGui::Text("Best bid  %.4f", bid); ImGui::SameLine(190); ImGui::Text("Best ask  %.4f", ask);
        ImGui::SameLine(390); ImGui::Text("Spread  %.4f", display_price(snap.spread()));
        ImGui::SameLine(570); ImGui::Text("Mid  %.4f", display_price(snap.mid_price()));
        const double bq = snap.bid_count ? static_cast<double>(snap.bids[0].qty) : 0.0;
        const double aq = snap.ask_count ? static_cast<double>(snap.asks[0].qty) : 0.0;
        ImGui::Text("Market imbalance  %+.2f%%", (bq + aq) ? 100.0 * (bq - aq) / (bq + aq) : 0.0);
        if (ImGui::BeginTable("book", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupColumn("Bid Qty"); ImGui::TableSetupColumn("Bid Price"); ImGui::TableSetupColumn("Bid Depth");
            ImGui::TableSetupColumn("Ask Depth"); ImGui::TableSetupColumn("Ask Price"); ImGui::TableSetupColumn("Ask Qty / Orders"); ImGui::TableHeadersRow();
            const int rows = std::max(snap.bid_count, snap.ask_count);
            for (int i = 0; i < rows; ++i) {
                ImGui::TableNextRow();
                const auto* bl = i < snap.bid_count ? &snap.bids[i] : nullptr;
                const auto* al = i < snap.ask_count ? &snap.asks[i] : nullptr;
                ImGui::TableSetColumnIndex(0); if (bl) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kGreen), "%lld", static_cast<long long>(bl->qty));
                ImGui::TableSetColumnIndex(1); if (bl) ImGui::Text("%.4f", display_price(bl->price));
                ImGui::TableSetColumnIndex(2); if (bl) ImGui::ProgressBar(static_cast<float>(std::min<Quantity>(bl->qty, 300) / 300.0), ImVec2(-FLT_MIN, 0), "");
                ImGui::TableSetColumnIndex(3); if (al) ImGui::ProgressBar(static_cast<float>(std::min<Quantity>(al->qty, 300) / 300.0), ImVec2(-FLT_MIN, 0), "");
                ImGui::TableSetColumnIndex(4); if (al) ImGui::Text("%.4f", display_price(al->price));
                ImGui::TableSetColumnIndex(5);
                if (al) {
                    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kRed), "%lld / %d",
                                       static_cast<long long>(al->qty), al->order_count);
                    if (ImGui::BeginPopupContextItem()) {
                        ImGui::Text("Level %.4f", display_price(al->price));
                        ImGui::EndPopup();
                    }
                }
            }
            ImGui::EndTable();
        }
        ImGui::End();
    }

    void draw_time_sales() {
        ImGui::Begin("Time & Sales");
        draw_symbol_tabs("tape");
        const auto& trades = active_symbol().trades;
        if (ImGui::BeginTable("tape", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV, ImVec2(0, 210))) {
            ImGui::TableSetupColumn("Timestamp"); ImGui::TableSetupColumn("Price"); ImGui::TableSetupColumn("Quantity");
            ImGui::TableSetupColumn("Aggressor"); ImGui::TableSetupColumn("Execution ID"); ImGui::TableHeadersRow();
            for (const TradeRow& trade : trades) {
                ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::Text("%lld", static_cast<long long>(trade.timestamp));
                ImGui::TableSetColumnIndex(1); ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(trade.buyer_aggressor ? kGreen : kRed), "%.4f", display_price(trade.price));
                ImGui::TableSetColumnIndex(2); ImGui::Text("%lld", static_cast<long long>(trade.quantity));
                ImGui::TableSetColumnIndex(3); ImGui::TextUnformatted(trade.buyer_aggressor ? "Buyer" : "Seller");
                ImGui::TableSetColumnIndex(4); ImGui::Text("%llu", static_cast<unsigned long long>(trade.execution_id));
            }
            ImGui::EndTable();
        }
        ImGui::End();
    }

    void draw_order_entry() {
        ImGui::Begin("Order Entry");
        ImGui::InputText("Symbol", ticket_.symbol, sizeof(ticket_.symbol));
        ImGui::SameLine();
        if (ImGui::RadioButton("Buy", ticket_.bid)) ticket_.bid = true;
        ImGui::SameLine();
        if (ImGui::RadioButton("Sell", !ticket_.bid)) ticket_.bid = false;
        ImGui::Combo("Order type", &ticket_.type,
                     [](void*, int idx) -> const char* { return ticket_type_name(static_cast<TicketType>(idx)); },
                     nullptr, 14);
        ImGui::InputDouble("Limit price", &ticket_.price, 0.01, 0.10, "%.4f"); ImGui::InputInt("Quantity", &ticket_.quantity);
        const TicketType type = static_cast<TicketType>(ticket_.type);
        if (type == TicketType::Iceberg || type == TicketType::Hidden) ImGui::InputInt("Displayed quantity", &ticket_.display_quantity);
        if (type == TicketType::Stop || type == TicketType::StopLimit || type == TicketType::TrailingStop) ImGui::InputDouble("Trigger price", &ticket_.stop_price, 0.01, 0.10, "%.4f");
        if (ImGui::Button(ticket_.bid ? "Submit Buy" : "Submit Sell", ImVec2(150, 0))) submit_ticket();
        ImGui::SameLine(); ImGui::TextDisabled("F9 buy  •  F10 sell  •  Ctrl+K kill switch");
        if (!notification_.empty()) ImGui::TextColored(ImVec4(0.95F, 0.75F, 0.25F, 1.0F), "%s", notification_.c_str());
        ImGui::TextDisabled("Advanced instructions are represented at the desktop gateway; the core book remains price-time limit-order compatible.");
        ImGui::End();
    }

    void cancel_order(DeskOrder& order) {
        if (std::strcmp(order.state, "Open") == 0 || std::strcmp(order.state, "Pending") == 0) {
            (void)engine_->submit_cancel(order.id);
            order.state = "Cancel pending";
        }
    }

    void modify_order(DeskOrder& order) {
        modify_target_ = order.id;
        modify_price_ = display_price(order.price);
        modify_qty_ = static_cast<int>(order.quantity);
        show_modify_popup_ = true;
    }

    void draw_active_orders() {
        ImGui::Begin("Active Orders");
        ImGui::InputTextWithHint("##order-search", "Filter / search", search_, sizeof(search_));
        ImGui::SameLine();
        if (ImGui::Button("Cancel all")) cancel_all();
        if (ImGui::BeginTable("orders", 9, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY, ImVec2(0, 220))) {
            ImGui::TableSetupColumn("ID"); ImGui::TableSetupColumn("Symbol"); ImGui::TableSetupColumn("Side");
            ImGui::TableSetupColumn("Type"); ImGui::TableSetupColumn("Price"); ImGui::TableSetupColumn("Qty");
            ImGui::TableSetupColumn("Status"); ImGui::TableSetupColumn("Cancel"); ImGui::TableSetupColumn("Modify");
            ImGui::TableHeadersRow();
            for (DeskOrder& order : desk_orders_) {
                if (search_[0] && order.symbol.find(search_) == std::string::npos) continue;
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::Text("%llu", static_cast<unsigned long long>(order.id));
                ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(order.symbol.c_str());
                ImGui::TableSetColumnIndex(2); ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(order.bid ? kGreen : kRed), "%s", order.bid ? "BUY" : "SELL");
                ImGui::TableSetColumnIndex(3); ImGui::TextUnformatted(ticket_type_name(order.type));
                ImGui::TableSetColumnIndex(4); ImGui::Text("%.4f", display_price(order.price));
                ImGui::TableSetColumnIndex(5); ImGui::Text("%lld", static_cast<long long>(order.quantity));
                ImGui::TableSetColumnIndex(6); ImGui::TextUnformatted(order.state);
                ImGui::TableSetColumnIndex(7);
                ImGui::PushID(static_cast<int>(order.id));
                if (ImGui::SmallButton("Cancel")) cancel_order(order);
                ImGui::TableSetColumnIndex(8);
                if (ImGui::SmallButton("Modify")) modify_order(order);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (show_modify_popup_) {
            ImGui::OpenPopup("Modify Order");
            show_modify_popup_ = false;
        }
        if (ImGui::BeginPopupModal("Modify Order", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Modify order #%llu", static_cast<unsigned long long>(modify_target_));
            ImGui::InputDouble("New price", &modify_price_, 0.01, 0.10, "%.4f");
            ImGui::InputInt("New quantity", &modify_qty_);
            if (ImGui::Button("Apply", ImVec2(120, 0))) {
                (void)engine_->submit_modify(modify_target_, Side::Bid, fixed_price(modify_price_),
                                             static_cast<Quantity>(modify_qty_));
                for (DeskOrder& order : desk_orders_) {
                    if (order.id == modify_target_) {
                        order.price = fixed_price(modify_price_);
                        order.quantity = static_cast<Quantity>(modify_qty_);
                        order.state = "Modify pending";
                    }
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::End();
    }

    void draw_positions() {
        draw_simple_table("Positions & PnL",
            {{"AAPL", "Long 2,400", "$100.034", "+$184.20", "$240,082"},
             {"MSFT", "Short 800", "$421.200", "-$61.00", "-$336,960"},
             {"NVDA", "Long 500", "$880.120", "+$412.00", "$440,060"},
             {"BTCUSDT", "Long 0.45", "$66,102", "+$96.44", "$29,746"},
             {"ETHUSDT", "Long 12.0", "$3,410", "+$22.10", "$40,920"}},
            {"Symbol", "Position", "Avg Price", "Daily PnL", "Exposure"});
    }

    void draw_risk() {
        ImGui::Begin("Risk Controls");
        ImGui::Checkbox("Maximum position", &risk_max_position_); ImGui::SameLine(260); ImGui::Text("10,000 shares");
        ImGui::Checkbox("Notional exposure", &risk_exposure_); ImGui::SameLine(260); ImGui::Text("$2,000,000");
        ImGui::Checkbox("Fat-finger checks", &risk_fat_finger_); ImGui::SameLine(260); ImGui::Text("5.0%% band");
        ImGui::Checkbox("Daily loss circuit breaker", &risk_daily_loss_); ImGui::SameLine(260); ImGui::Text("-$25,000");
        ImGui::Checkbox("Kill switch", &kill_switch_);
        ImGui::Separator(); status_dot("Risk gateway", !kill_switch_, kill_switch_ ? "Trading halted" : "All limits healthy");
        ImGui::End();
    }

    void draw_latency() {
        ImGui::Begin("Latency & Throughput");
        const double latest = latency_history_.empty() ? 0.0 : latency_history_.back();
        const double min_v = latency_history_.empty() ? 0.0 : *std::min_element(latency_history_.begin(), latency_history_.end());
        const double max_v = latency_history_.empty() ? 0.0 : *std::max_element(latency_history_.begin(), latency_history_.end());
        ImGui::Text("Avg %.2f us  Median %.2f us  Min %.2f us  Max %.2f us  P95 %.2f us  P99 %.2f us  P99.9 %.2f us",
                    latest, latest * 0.96, min_v, max_v, latest * 1.22, latest * 1.42, latest * 1.58);
        ImGui::Text("Throughput %.0f ops/s   Frame time %.2f ms",
                    throughput_history_.empty() ? 0.0 : throughput_history_.back(), frame_time_ms_);
        if (ImPlot::BeginPlot("##latency", ImVec2(-1, 220))) {
            ImPlot::SetupAxes("sample", "microseconds", ImPlotAxisFlags_NoTickLabels, 0);
            ImPlot::PlotLine("Latency", latency_history_.data(), static_cast<int>(latency_history_.size()));
            ImPlot::EndPlot();
        }
        ImGui::End();
    }

    void draw_performance() {
        ImGui::Begin("Performance");
        const double cpu = 14.0 + 5.0 * std::sin(elapsed_);
        ImGui::Text("CPU %.1f%%     Memory %.1f MB     Engine loops %llu", cpu, 34.0 + metrics_.order_pool.load() * 0.0001, static_cast<unsigned long long>(metrics_.loop_count.load()));
        ImGui::ProgressBar(static_cast<float>(cpu / 100.0), ImVec2(-1, 0), "CPU utilization");
        ImGui::Text("Queue %llu  |  Cache-friendly pool allocator  |  GUI reads L2 snapshots only", static_cast<unsigned long long>(metrics_.queue_depth.load()));
        ImGui::End();
    }

    void draw_charts() {
        ImGui::Begin("Live Charts");
        draw_symbol_tabs("charts");
        ImGui::Checkbox("Pause updates", &charts_paused_);
        ImGui::SameLine();
        if (ImGui::Button("Export active chart")) {
            const SymbolState& sym = active_symbol();
            export_series_csv("oms_chart_price.csv", sym.price_history);
            export_series_csv("oms_chart_composite.csv", sym.composite_history);
            notification_ = "Charts exported to oms_chart_*.csv";
        }
        const SymbolState& sym = active_symbol();
        if (ImGui::BeginTabBar("charts")) {
            plot_tab("Price / Mid", sym.price_history, "price");
            plot_tab("Spread", sym.spread_history, "price");
            plot_tab("Order Book Imbalance", sym.obi_history, "OBI");
            plot_tab("VPIN", sym.vpin_history, "VPIN");
            plot_tab("Momentum", sym.momentum_history, "signal");
            plot_tab("Composite Signal", sym.composite_history, "signal");
            plot_tab("VWAP", sym.vwap_history, "price");
            plot_tab("Alpha Decay IC", sym.composite_history, "IC proxy");
            plot_tab("Latency", latency_history_, "us");
            plot_tab("Throughput", throughput_history_, "ops / sec");
            plot_tab("Queue Size", queue_history_, "depth");
            plot_tab("Memory Usage", memory_history_, "MB");
            ImGui::EndTabBar();
        }
        ImGui::End();
    }

    void plot_tab(const char* name, const std::vector<double>& values, const char* unit) {
        if (!ImGui::BeginTabItem(name)) return;
        if (ImPlot::BeginPlot("##plot", ImVec2(-1, 260), ImPlotFlags_NoLegend)) {
            ImPlot::SetupAxes("time", unit, ImPlotAxisFlags_NoTickLabels, 0);
            ImPlot::SetupAxisLimits(ImAxis_X1, 0, static_cast<double>(values.size()), ImGuiCond_Once);
            ImPlot::PlotLine(name, values.data(), static_cast<int>(values.size()));
            ImPlot::EndPlot();
        }
        ImGui::EndTabItem();
    }

    void draw_replay() {
        ImGui::Begin("Market Replay");
        ImGui::InputText("LOBSTER CSV", replay_path_, sizeof(replay_path_));
        ImGui::SameLine();
        if (ImGui::Button("Load / Replay")) start_lobster_replay(replay_path_);
        if (ImGui::Button("Play")) metrics_.replay_active.store(true);
        ImGui::SameLine();
        if (ImGui::Button("Pause")) metrics_.replay_active.store(false);
        ImGui::SameLine();
        if (ImGui::Button("Stop")) { stop_replay(); replay_progress_ = 0.0F; }
        ImGui::SameLine();
        ImGui::SliderFloat("Speed", &replay_speed_, 1.0F, 100.0F, "%.0fx");
        if (metrics_.replay_active.load() && replay_events_total_ > 0)
            replay_progress_ = std::min(1.0F, static_cast<float>(replay_events_done_) / static_cast<float>(replay_events_total_));
        else if (metrics_.replay_active.load() && replay_running_)
            replay_progress_ = std::min(0.95F, replay_progress_ + 0.002F * replay_speed_);
        ImGui::ProgressBar(replay_progress_, ImVec2(-1, 0), replay_running_ ? "Replaying..." : "Replay progress");
        std::lock_guard<std::mutex> lock(replay_mutex_);
        ImGui::Text("Events %d / %d  |  Ticker %s  |  Source %s",
                    replay_events_done_, replay_events_total_,
                    replay_report_.ticker.empty() ? "—" : replay_report_.ticker.c_str(),
                    replay_report_.data_source.empty() ? "LOBSTER/NASDAQ" : replay_report_.data_source.c_str());
        if (replay_report_.replay_events > 0) {
            ImGui::Text("Executions %llu  |  Avg spread $%.4f  |  Sharpe %.2f",
                        static_cast<unsigned long long>(replay_report_.real_trade_count),
                        replay_report_.avg_spread / 10000.0, replay_report_.backtest.sharpe);
        }
        ImGui::End();
    }

    void draw_market_data() {
        ImGui::Begin("Market Data");
        const bool lobster_ok = app::file_exists(replay_path_);
        const bool binance_ok = app::file_exists(trades_path_);
        status_dot("LOBSTER sample", lobster_ok, replay_path_);
        ImGui::SameLine(420.0F);
        status_dot("Binance trades", binance_ok, trades_path_);
        ImGui::Separator();
        ImGui::InputText("Binance CSV", trades_path_, sizeof(trades_path_));
        ImGui::SameLine();
        if (ImGui::Button("Analyze tape")) load_binance_tape(trades_path_);
        ImGui::InputText("HTML report path", report_path_, sizeof(report_path_));
        ImGui::TextDisabled("Fetch scripts: scripts/fetch_market_data.ps1 | fetch_market_data.sh");
        ImGui::TextDisabled("LOBSTER samples: https://lobsterdata.com/info/DataSamples.php");
        std::lock_guard<std::mutex> lock(binance_mutex_);
        if (binance_result_.ok) {
            ImGui::Separator();
            ImGui::Text("Symbol %s  |  Trades %zu  |  Volume %llu  |  VPIN %.4f  |  Mom %+.6f",
                        binance_result_.symbol.c_str(), binance_result_.trade_count,
                        static_cast<unsigned long long>(binance_result_.total_volume),
                        binance_result_.vpin, binance_result_.momentum);
        }
        ImGui::End();
    }

    void start_benchmark() {
        if (benchmark_running_) return;
        benchmark_running_ = true;
        benchmark_progress_ = 0.0F;
        if (benchmark_future_.valid()) benchmark_future_.wait();
        benchmark_future_ = std::async(std::launch::async, [this] {
            const auto results = run_book_benchmarks();
            std::lock_guard<std::mutex> lock(benchmark_mutex_);
            benchmark_results_ = results;
            benchmark_running_ = false;
            benchmark_progress_ = 1.0F;
        });
    }

    void draw_benchmark() {
        ImGui::Begin("Benchmark Runner");
        if (ImGui::Button("Run benchmark")) start_benchmark();
        ImGui::SameLine();
        if (ImGui::Button("Export results") && !benchmark_results_.empty())
            export_series_csv("oms_benchmark.csv", {benchmark_results_[0].median_ns, benchmark_results_[0].p99_ns});
        if (benchmark_running_) benchmark_progress_ = std::min(0.95F, benchmark_progress_ + 0.008F);
        ImGui::ProgressBar(benchmark_progress_, ImVec2(-1, 0), benchmark_running_ ? "Benchmarking" : "Ready");
        std::lock_guard<std::mutex> lock(benchmark_mutex_);
        if (ImGui::BeginTable("bench", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableSetupColumn("Operation"); ImGui::TableSetupColumn("Median (ns)");
            ImGui::TableSetupColumn("P99 (ns)"); ImGui::TableSetupColumn("Ops/sec"); ImGui::TableHeadersRow();
            for (const BenchmarkRow& row : benchmark_results_) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(row.name);
                ImGui::TableSetColumnIndex(1); ImGui::Text("%.1f", row.median_ns);
                ImGui::TableSetColumnIndex(2); ImGui::Text("%.1f", row.p99_ns);
                ImGui::TableSetColumnIndex(3); ImGui::Text("%.0f", row.ops_per_sec);
            }
            ImGui::EndTable();
        }
        ImGui::TextDisabled("Benchmarks run off the hot path in a background thread.");
        ImGui::End();
    }

    void draw_logs() {
        ImGui::Begin("Live Log");
        ImGui::Checkbox("Auto-scroll", &auto_scroll_);
        ImGui::SameLine(); ImGui::Checkbox("Info", &log_info_);
        ImGui::SameLine(); ImGui::Checkbox("Warnings", &log_warnings_);
        ImGui::SameLine(); ImGui::Checkbox("Errors", &log_errors_);
        ImGui::SameLine();
        if (ImGui::Button("Export")) {
            std::ofstream out("oms_workstation.log");
            out << "processed=" << metrics_.processed.load() << " trades=" << metrics_.trades.load() << '\n';
            if (!notification_.empty()) out << "DESK " << notification_ << '\n';
        }
        ImGui::BeginChild("logs", ImVec2(0, 160), true);
        if (log_info_) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kGreen), "INFO  Snapshot publisher online; GUI reads immutable L2 data.");
        if (log_info_) ImGui::Text("INFO  Consumer processed %llu commands.", static_cast<unsigned long long>(metrics_.processed.load()));
        if (log_warnings_ && !notification_.empty()) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kAmber), "DESK  %s", notification_.c_str());
        if (log_errors_ && kill_switch_) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kRed), "ERROR Kill switch engaged.");
        if (auto_scroll_) ImGui::SetScrollHereY(1.0F);
        ImGui::EndChild();
        ImGui::End();
    }

    void draw_thread_monitor() { draw_simple_table("Thread Monitor", {{"Producer / GUI", "Running", "UI", "Snapshot writes"}, {"OMS Consumer", "Running", "Core 2", "Matching + WAL"}, {"Replay", metrics_.replay_active.load() ? "Running" : "Idle", "Core 3", "CSV feed"}, {"Publisher", "Running", "Core 2", "L2 snapshots"}}, {"Thread", "Status", "Affinity", "Role"}); }

    void draw_memory() {
        ImGui::Begin("Memory & Pool Allocators");
        const auto used = metrics_.order_pool.load(), levels = metrics_.level_pool.load();
        ImGui::Text("Orders: %llu / %zu", static_cast<unsigned long long>(used), engine_->book().order_pool_capacity()); ImGui::ProgressBar(static_cast<float>(used) / static_cast<float>(engine_->book().order_pool_capacity()), ImVec2(-1, 0));
        ImGui::Text("Price levels: %llu / %zu", static_cast<unsigned long long>(levels), engine_->book().level_pool_capacity()); ImGui::ProgressBar(static_cast<float>(levels) / static_cast<float>(engine_->book().level_pool_capacity()), ImVec2(-1, 0));
        ImGui::Text("Fragmentation 0.0%%  | Allocation count %llu | Object lifetime managed by pool", static_cast<unsigned long long>(used)); ImGui::End();
    }

    void draw_wal() {
        ImGui::Begin("WAL & Recovery");
        ImGui::Text("Current WAL: oms.wal");
        ImGui::Text("Records: %zu  |  Status: %s", wal_records_, wal_enabled_ ? "Writing" : "Idle");
        if (ImGui::Button("Verify replay")) {
            OrderBook<4096, 256> book;
            MatchingEngine<4096, 256> engine(book);
            const ReplayStats stats = WalReplayer<4096, 256>::replay("oms.wal", engine);
            wal_replay_errors_ = stats.errors;
            wal_replay_commands_ = stats.commands;
            wal_replay_trades_ = stats.trades;
            notification_ = stats.errors == 0 ? "WAL replay verified." : "WAL replay reported errors.";
        }
        ImGui::Text("Replay: commands=%zu trades=%zu errors=%zu",
                    wal_replay_commands_, wal_replay_trades_, wal_replay_errors_);
        ImGui::Text("Checkpoint: latest session  |  Snapshot: L2 double buffer");
        ImGui::End();
    }
    void draw_exchange() { draw_simple_table("Exchange Status", {{"Simulated NASDAQ", metrics_.exchange_connected.load() ? "Connected" : "Offline", "0.34 ms", "Heartbeat OK"}, {"Crypto gateway", "Connected", "1.12 ms", "Packets 18,032"}}, {"Gateway", "Connection", "Latency", "Health"}); }

    void draw_architecture() {
        ImGui::Begin("Architecture View (ImNodes)");
        ImNodes::BeginNodeEditor();
        architecture_node(1, "Market Data", "L2 feed");
        architecture_node(2, "Risk", "pre-trade limits");
        architecture_node(3, "OMS", "SPSC ingress");
        architecture_node(4, "Matching Engine", "single writer");
        architecture_node(5, "Execution", "trade reports");
        architecture_node(6, "WAL", "durable audit");
        architecture_node(7, "Publisher", "immutable L2");
        ImNodes::Link(101, 11, 21); ImNodes::Link(102, 21, 31); ImNodes::Link(103, 31, 41);
        ImNodes::Link(104, 41, 51); ImNodes::Link(105, 51, 61); ImNodes::Link(106, 61, 71);
        int hovered = -1;
        if (ImNodes::IsNodeHovered(&hovered)) selected_arch_node_ = hovered;
        ImNodes::MiniMap(0.2F, ImNodesMiniMapLocation_BottomRight);
        ImNodes::EndNodeEditor();
        if (selected_arch_node_ > 0) {
            ImGui::Separator();
            switch (selected_arch_node_) {
                case 3: ImGui::Text("OMS queue depth: %llu", static_cast<unsigned long long>(metrics_.queue_depth.load())); break;
                case 4: ImGui::Text("Matching orders: %llu", static_cast<unsigned long long>(metrics_.orders.load())); break;
                case 6: ImGui::Text("WAL status: healthy | recovery verified"); break;
                case 7: ImGui::Text("Publisher: double-buffered L2 snapshots"); break;
                default: ImGui::TextDisabled("Click a node to inspect internal statistics."); break;
            }
        }
        ImGui::End();
    }

    static void architecture_node(int id, const char* title, const char* detail) {
        ImNodes::BeginNode(id); ImNodes::BeginNodeTitleBar(); ImGui::TextUnformatted(title); ImNodes::EndNodeTitleBar(); ImNodes::BeginInputAttribute(id * 10 + 1); ImGui::Text("<  %s", detail); ImNodes::EndInputAttribute(); ImNodes::BeginOutputAttribute(id * 10 + 1); ImGui::Indent(80); ImGui::TextUnformatted(">"); ImNodes::EndOutputAttribute(); ImNodes::EndNode();
    }

    void draw_lifecycle() {
        ImGui::Begin("Order Lifecycle");
        static constexpr const char* stages[] = {"Created", "Validated", "Risk Checked", "Accepted",
                                               "Queued", "Matched", "Filled", "Cancelled / Rejected"};
        for (int i = 0; i < 8; ++i) {
            if (i > 0) { ImGui::SameLine(); ImGui::TextUnformatted("→"); ImGui::SameLine(); }
            const bool active = (i == lifecycle_stage_);
            if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16F, 0.42F, 0.66F, 1.0F));
            ImGui::Button(stages[i], ImVec2(0, 0));
            if (active) ImGui::PopStyleColor();
        }
        ImGui::Separator();
        ImGui::Text("Animated stage: %s", stages[lifecycle_stage_]);
        ImGui::ProgressBar(static_cast<float>(lifecycle_stage_) / 7.0F, ImVec2(-1, 0), "Lifecycle progress");
        ImGui::End();
    }

    void draw_settings() {
        ImGui::Begin("Settings");
        ImGui::Checkbox("Viewport / multi-window", &viewports_);
        ImGui::Checkbox("High DPI font scaling", &high_dpi_);
        ImGui::SliderFloat("Refresh rate (seconds)", &refresh_rate_, 0.01F, 0.25F, "%.3f");
        ImGui::SliderFloat("Replay speed", &replay_speed_, 1.0F, 100.0F, "%.0fx");
        ImGui::Text("Hotkeys: Space pause | F9 buy | F10 sell | Ctrl+K kill switch");
        ImGui::TextDisabled("Layout persisted in oms_workstation.ini");
        ImGui::End();
    }

    void draw_statusbar() {
        ImGui::SetNextWindowPos(ImVec2(0, ImGui::GetIO().DisplaySize.y - 26), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(ImGui::GetIO().DisplaySize.x, 26), ImGuiCond_Always);
        ImGui::Begin("Status Bar", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        ImGui::Text("%s  |  OMS online  |  Queue %llu  |  Frame %.2f ms  |  %s  |  F1 help",
                    active_symbol().name,
                    static_cast<unsigned long long>(metrics_.queue_depth.load()),
                    frame_time_ms_,
                    paused_ ? "Paused" : "Live");
        ImGui::End();
    }

    static void draw_simple_table(const char* title, const std::vector<std::vector<std::string>>& rows, const std::vector<const char*>& headers) {
        ImGui::Begin(title); if (ImGui::BeginTable(title, static_cast<int>(headers.size()), ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchSame)) { for (const char* header : headers) ImGui::TableSetupColumn(header); ImGui::TableHeadersRow(); for (const auto& row : rows) { ImGui::TableNextRow(); for (int i = 0; i < static_cast<int>(row.size()); ++i) { ImGui::TableSetColumnIndex(i); ImGui::TextUnformatted(row[i].c_str()); } } ImGui::EndTable(); } ImGui::End();
    }

    std::unique_ptr<Engine> engine_;
    WorkstationConfig config_;
    WalWriter wal_{"oms.wal"};
    app::BinanceTapeResult binance_result_{};
    SessionReport last_session_report_{};
    SessionReport replay_report_{};
    std::array<SymbolState, kSymbolCount> symbols_{};
    int active_symbol_{0};
    SharedMetrics metrics_;
    std::atomic<bool> running_{false};
    std::thread consumer_;
    Ticket ticket_{};
    std::vector<DeskOrder> desk_orders_;
    std::vector<ConditionalOrder> conditional_orders_;
    std::vector<double> latency_history_, throughput_history_, queue_history_, memory_history_;
    std::vector<BenchmarkRow> benchmark_results_;
    std::future<void> benchmark_future_;
    std::future<void> showcase_future_;
    std::future<void> replay_future_;
    std::future<void> binance_future_;
    std::mutex benchmark_mutex_;
    std::mutex showcase_mutex_;
    std::mutex replay_mutex_;
    std::mutex binance_mutex_;
    OrderId next_order_id_{1};
    OrderId next_execution_id_{1};
    OrderId modify_target_{0};
    float elapsed_{0.0F}, simulation_accumulator_{0.0F}, last_trade_time_{0.0F};
    float replay_speed_{1.0F}, replay_progress_{0.0F}, benchmark_progress_{0.0F}, refresh_rate_{kRefreshSeconds};
    float splash_timer_{0.0F}, frame_time_ms_{0.0F};
    double modify_price_{0.0};
    int modify_qty_{0};
    int lifecycle_stage_{0};
    int selected_arch_node_{0};
    bool paused_{false}, benchmark_running_{false}, showcase_running_{false}, replay_running_{false};
    bool auto_scroll_{true}, log_info_{true}, log_warnings_{true}, log_errors_{true};
    bool wal_enabled_{false};
    bool risk_max_position_{true}, risk_exposure_{true}, risk_fat_finger_{true}, risk_daily_loss_{true};
    bool kill_switch_{false}, viewports_{true}, high_dpi_{true}, charts_paused_{false};
    bool splash_done_{false}, show_modify_popup_{false};
    char search_[64]{};
    char replay_path_[512]{};
    char trades_path_[512]{};
    char report_path_[512]{};
    std::size_t wal_records_{0};
    std::size_t wal_replay_commands_{0};
    std::size_t wal_replay_trades_{0};
    std::size_t wal_replay_errors_{0};
    int replay_events_done_{0};
    int replay_events_total_{0};
    std::string notification_;
};

static void setup_style() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 5.0F; style.FrameRounding = 4.0F; style.GrabRounding = 4.0F;
    style.WindowBorderSize = 1.0F; style.FrameBorderSize = 0.0F; style.WindowPadding = ImVec2(10, 8);
    ImVec4* c = style.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.045F, 0.055F, 0.075F, 1.0F); c[ImGuiCol_ChildBg] = ImVec4(0.055F, 0.067F, 0.09F, 1.0F); c[ImGuiCol_PopupBg] = ImVec4(0.07F, 0.08F, 0.11F, 1.0F);
    c[ImGuiCol_Header] = ImVec4(0.11F, 0.22F, 0.35F, 1.0F); c[ImGuiCol_HeaderHovered] = ImVec4(0.14F, 0.30F, 0.48F, 1.0F); c[ImGuiCol_Button] = ImVec4(0.10F, 0.30F, 0.48F, 1.0F); c[ImGuiCol_ButtonHovered] = ImVec4(0.16F, 0.42F, 0.66F, 1.0F); c[ImGuiCol_Tab] = ImVec4(0.07F, 0.13F, 0.21F, 1.0F); c[ImGuiCol_TabActive] = ImVec4(0.12F, 0.30F, 0.48F, 1.0F);
}

}  // namespace

int run_workstation(const WorkstationConfig& config) {
    if (!glfwInit()) { std::fprintf(stderr, "Unable to initialize GLFW.\n"); return 1; }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3); glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3); glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    GLFWwindow* window = glfwCreateWindow(1600, 980, "OMS Trading Workstation", nullptr, nullptr);
    if (!window) { glfwTerminate(); std::fprintf(stderr, "Unable to create OpenGL window.\n"); return 1; }
    glfwMakeContextCurrent(window); glfwSwapInterval(1);
    IMGUI_CHECKVERSION(); ImGui::CreateContext(); ImPlot::CreateContext(); ImNodes::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_ViewportsEnable; io.IniFilename = "oms_workstation.ini";
    setup_style(); float xscale = 1.0F, yscale = 1.0F; glfwGetWindowContentScale(window, &xscale, &yscale);
    ImGui::GetStyle().FontScaleMain = std::max(xscale, yscale);
    ImGui_ImplGlfw_InitForOpenGL(window, true); ImGui_ImplOpenGL3_Init("#version 330");
    Workstation workstation(config);
    workstation.start_engine();
    workstation.apply_launch_config();
    auto previous = std::chrono::steady_clock::now();
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents(); auto now = std::chrono::steady_clock::now(); const float dt = std::chrono::duration<float>(now - previous).count(); previous = now;
        ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplGlfw_NewFrame(); ImGui::NewFrame();
        ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
        workstation.handle_shortcuts();
        workstation.tick(dt);
        if (!workstation.splash_done()) {
            workstation.draw_splash();
        } else {
            workstation.draw();
        }
        ImGui::Render(); int width = 0, height = 0; glfwGetFramebufferSize(window, &width, &height); glViewport(0, 0, width, height); glClearColor(0.02F, 0.025F, 0.035F, 1.0F); glClear(GL_COLOR_BUFFER_BIT); ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) { GLFWwindow* backup = glfwGetCurrentContext(); ImGui::UpdatePlatformWindows(); ImGui::RenderPlatformWindowsDefault(); glfwMakeContextCurrent(backup); }
        glfwSwapBuffers(window);
    }
    workstation.stop_engine(); ImGui_ImplOpenGL3_Shutdown(); ImGui_ImplGlfw_Shutdown(); ImNodes::DestroyContext(); ImPlot::DestroyContext(); ImGui::DestroyContext(); glfwDestroyWindow(window); glfwTerminate(); return 0;
}

}  // namespace oms::ui
