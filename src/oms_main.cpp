// Unified OMS product — interactive console UI, demos, HTML report export.

#include "backtest/alpha_decay.hpp"
#include "backtest/backtest_engine.hpp"
#include "book/order_book.hpp"
#include "common/timestamp.hpp"
#include "engine/matching_engine.hpp"
#include "engine/oms_engine.hpp"
#include "io/wal.hpp"
#include "io/wal_replay.hpp"
#include "signals/composite.hpp"
#include "signals/vpin.hpp"
#include "app/real_data_modes.hpp"
#include "ui/console_visual.hpp"
#include "ui/html_report.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace oms;
using namespace oms::ui;

namespace {

struct SimulationState {
    OrderBook<4096, 512> book;
    L2BookView l2_view;
    VPIN vpin{50, 500};
    MidPriceMomentum momentum{20};
    AlphaDecayAnalyzer decay;
    BacktestEngine backtest;
    std::vector<double> composite_history;
    std::vector<double> mid_history;
    std::mt19937_64 rng{42};
};

static void run_simulation_step(SimulationState& s, int event, OrderId& id) {
    std::uniform_int_distribution<Price> price_jitter(-500, 500);
    Price base_bid = 1'000'000;
    Price base_ask = 1'000'100;
    TimestampNs ts = NanoClock::now();

    switch (event % 5) {
        case 0:
        case 1:
            s.book.add_order(id++, Side::Bid, base_bid + price_jitter(s.rng), 10 + (event % 50));
            break;
        case 2:
            s.book.add_order(id++, Side::Ask, base_ask + price_jitter(s.rng), 10 + (event % 50));
            break;
        case 3:
            if (id > 100) s.book.cancel_order(id - 50);
            break;
        case 4:
            if (id > 200) s.book.execute_order(id - 100, 5);
            break;
    }

    if (event % 10 == 0) {
        s.book.publish_l2(s.l2_view);
        L2Snapshot snap = s.l2_view.load();
        CompositeSignal sig = CompositeSignal::compute(snap, s.vpin, s.momentum);
        s.decay.add_sample(sig.composite, snap.mid_price(), ts);
        s.backtest.on_bar(sig.composite, snap.mid_price(), ts);
        s.composite_history.push_back(sig.composite);
        if (snap.mid_price() != INVALID_PRICE)
            s.mid_history.push_back(price_to_display(snap.mid_price()));
    }

    if (event % 7 == 0) {
        s.vpin.on_trade(event % 2 == 0 ? Side::Bid : Side::Ask, 100, s.book.mid_price());
    }
}

static SessionReport build_report(SimulationState& s) {
    SessionReport r{};
    s.book.publish_l2(s.l2_view);
    r.final_book = s.l2_view.load();
    r.backtest = s.backtest.result();
    r.equity_history = r.backtest.equity_curve;
    r.composite_history = s.composite_history;
    r.mid_history = s.mid_history;
    r.book_adds = s.book.stats().adds;
    r.book_cancels = s.book.stats().cancels;
    r.book_executes = s.book.stats().executes;
    r.order_count = s.book.order_count();

    std::vector<int64_t> horizons = {10'000'000LL,  50'000'000LL,  100'000'000LL,
                                     500'000'000LL, 1'000'000'000LL, 5'000'000'000LL,
                                     10'000'000'000LL};
    r.decay_curve = s.decay.compute_decay(horizons);
    return r;
}

static void mode_live_desk(bool visual_refresh) {
    print_section("Live Trading Desk (simulated feed)");
    SimulationState sim;
    OrderId id = 1;
    const int total_events = 4000;
    const int refresh_every = visual_refresh ? 80 : total_events + 1;

    CompositeSignal last_sig{};
    L2Snapshot last_snap{};

    for (int event = 0; event < total_events; ++event) {
        run_simulation_step(sim, event, id);

        if (visual_refresh && event > 0 && event % refresh_every == 0) {
            sim.book.publish_l2(sim.l2_view);
            last_snap = sim.l2_view.load();
            last_sig = CompositeSignal::compute(last_snap, sim.vpin, sim.momentum);

            clear_screen();
            print_banner();
            std::printf("  Mode: Live Desk  |  Event %d / %d  |  Press Ctrl+C to abort\n\n",
                        event, total_events);
            render_l2_ladder(last_snap, 6);
            std::printf("\n  \033[1m── Signals ──\033[0m\n");
            print_bar("OBI", last_sig.obi, 1.0);
            print_bar("VPIN", last_sig.vpin, 1.0);
            print_bar("Mom", last_sig.momentum, 0.001);
            print_bar("Composite", last_sig.composite, 1.0);
            std::printf("\n  \033[1m── Composite history ──\033[0m\n");
            print_sparkline(sim.composite_history);
            std::printf("\n  Orders on book: %zu\n", sim.book.order_count());
            std::fflush(stdout);

#ifdef _WIN32
            Sleep(120);
#else
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
#endif
        }
    }

    if (!visual_refresh) {
        sim.book.publish_l2(sim.l2_view);
        last_snap = sim.l2_view.load();
        last_sig = CompositeSignal::compute(last_snap, sim.vpin, sim.momentum);
        render_l2_ladder(last_snap, 8);
        std::printf("\n  Final signals: OBI=%+.4f VPIN=%.4f Mom=%+.6f Composite=%+.4f\n",
                    last_sig.obi, last_sig.vpin, last_sig.momentum, last_sig.composite);
        print_sparkline(sim.composite_history);
    }

    SessionReport rep = build_report(sim);
    std::printf("\n  Backtest Sharpe: %.2f | Return: %.4f | Trades: %d\n", rep.backtest.sharpe,
                rep.backtest.total_return, rep.backtest.trade_count);
}

static void mode_matching() {
    print_section("Matching Engine");
    OrderBook<1024, 256> book;
    MatchingEngine<1024, 256> engine(book);

    engine.submit_order(1, Side::Ask, 1'000'100, 100);
    engine.submit_order(2, Side::Ask, 1'000'200, 50);
    engine.submit_order(3, Side::Bid, 1'000'000, 80);
    MatchResult cross = engine.submit_order(4, Side::Bid, 1'000'150, 120);

    L2BookView view;
    book.publish_l2(view);
    render_l2_ladder(view.load(), 6);

    std::printf("\n  Trades from aggressive cross: %d\n", cross.trade_count);
    for (int i = 0; i < cross.trade_count; ++i) {
        const Trade& t = cross.trades[i];
        std::printf("    #%d  maker=%llu taker=%llu  px=%s  qty=%lld\n", i + 1,
                    static_cast<unsigned long long>(t.maker_id),
                    static_cast<unsigned long long>(t.taker_id),
                    format_price(t.price).c_str(), static_cast<long long>(t.qty));
    }
    std::printf("  Remaining qty: %lld | Resting: %s\n",
                static_cast<long long>(cross.remaining_qty),
                cross.resting_added ? "yes" : "no");
}

static SessionReport mode_spsc_wal(const std::string& wal_path) {
    print_section("Multithreaded SPSC + WAL");
    // Keep template sizes modest — book + SPSC ring are stack-allocated inside OmsEngine.
    OmsEngine<2048, 4096, 256> engine;
    WalWriter wal(wal_path);

    SessionReport rep{};
    if (!wal.open()) {
        std::fprintf(stderr, "  Failed to open WAL: %s\n", wal_path.c_str());
        return rep;
    }

    std::atomic<bool> done{false};
    std::atomic<int> produced{0};
    std::atomic<int> trades{0};

    std::thread consumer([&] {
        while (!done.load(std::memory_order_acquire) || engine.pending() > 0) {
            auto ps = engine.process_all(&wal);
            trades.fetch_add(ps.trades, std::memory_order_relaxed);
            if (!done.load(std::memory_order_acquire)) std::this_thread::yield();
        }
    });

    std::thread producer([&] {
        Price bid = 1'000'000;
        Price ask = 1'000'100;
        OrderId id = 1;
        for (int i = 0; i < 1500; ++i) {
            while (!engine.submit_add(id++, Side::Bid, bid, 10 + (i % 20)))
                std::this_thread::yield();
            ++produced;
            while (!engine.submit_add(id++, Side::Ask, ask, 10 + (i % 20)))
                std::this_thread::yield();
            ++produced;
            if (i % 50 == 0) {
                while (!engine.submit_add(id++, Side::Bid, ask, 5)) std::this_thread::yield();
                ++produced;
            }
        }
        done.store(true, std::memory_order_release);
    });

    producer.join();
    consumer.join();
    wal.close();

    rep.spsc_produced = produced.load();
    rep.spsc_trades = trades.load();
    rep.wal_commands = static_cast<int>(wal.record_count());

    L2BookView view;
    engine.book().publish_l2(view);
    rep.final_book = view.load();
    rep.order_count = engine.book().order_count();

    std::printf("  Commands: %d | Trades: %d | WAL records: %zu | Book orders: %zu\n",
                rep.spsc_produced, rep.spsc_trades, wal.record_count(), rep.order_count);
    render_l2_ladder(rep.final_book, 5);

    OrderBook<4096, 256> replay_book;
    MatchingEngine<4096, 256> replay_engine(replay_book);
    ReplayStats stats = WalReplayer<4096, 256>::replay(wal_path, replay_engine);
    rep.wal_commands = static_cast<int>(stats.commands);
    rep.wal_trades = static_cast<int>(stats.trades);

    std::printf("\n  WAL replay: commands=%zu trades=%zu errors=%zu\n", stats.commands,
                stats.trades, stats.errors);
    return rep;
}

static void mode_alpha_decay() {
    print_section("Alpha Decay Curve");
    SimulationState sim;
    OrderId id = 1;
    for (int e = 0; e < 5000; ++e) run_simulation_step(sim, e, id);

    auto curve = build_report(sim).decay_curve;
    double max_ic = 0.01;
    for (const auto& pt : curve) max_ic = std::max(max_ic, std::abs(pt.ic));

    for (const auto& pt : curve) {
        double ms = static_cast<double>(pt.horizon_ns) / 1e6;
        std::printf("  %8.1f ms  ", ms);
        print_bar("IC", pt.ic, max_ic, 32);
        std::printf("           n=%d\n", pt.sample_count);
    }
}

static void mode_benchmark_quick() {
    print_section("Quick Latency Benchmark");
    OrderBook<4096, 512> book;
    constexpr int N = 50000;
    std::vector<TimestampNs> stamps(N);

    auto bench = [&](const char* name, auto fn) {
        TimestampNs t0 = NanoClock::now();
        for (int i = 0; i < N; ++i) fn(i);
        TimestampNs t1 = NanoClock::now();
        double ns = static_cast<double>(t1 - t0) / N;
        std::printf("  %-18s %8.1f ns/op\n", name, ns);
    };

    bench("add_order", [&](int i) {
        book.add_order(100000 + i, Side::Bid, 1'000'000 - (i % 100), 10);
    });
    bench("cancel_order", [&](int i) { book.cancel_order(100000 + (i % N)); });
    bench("best_bid", [&](int) { (void)book.best_bid(); });
}

static void print_menu() {
    std::printf(
        "\033[1m  Main Menu\033[0m\n"
        "  [1] Live Trading Desk (animated L2 + signals)\n"
        "  [2] Matching engine demo\n"
        "  [3] SPSC multithread + WAL + replay\n"
        "  [4] Alpha decay visualization\n"
        "  [5] Quick latency benchmark\n"
        "  [6] Full showcase + export HTML report\n"
        "  [7] Real NASDAQ replay (LOBSTER CSV)\n"
        "  [8] Real crypto trade tape (Binance CSV)\n"
        "  [9] Fetch / verify market data files\n"
        "  [0] Exit\n"
        "\n  Choice: ");
    std::fflush(stdout);
}

static void run_showcase(const std::string& report_path) {
    enable_ansi_terminal();
    clear_screen();
    print_banner();
    std::printf("  Running full showcase...\n\n");

    SessionReport rep;
    {
        SimulationState sim;
        OrderId id = 1;
        for (int e = 0; e < 5000; ++e) run_simulation_step(sim, e, id);
        rep = build_report(sim);
    }

    mode_matching();
    SessionReport wal_rep = mode_spsc_wal("oms.wal");
    rep.spsc_produced = wal_rep.spsc_produced;
    rep.spsc_trades = wal_rep.spsc_trades;
    rep.wal_commands = wal_rep.wal_commands;
    rep.wal_trades = wal_rep.wal_trades;

    print_section("Alpha Decay");
    for (const auto& pt : rep.decay_curve) {
        std::printf("  %8.1f ms  IC=%+.4f  (n=%d)\n",
                    static_cast<double>(pt.horizon_ns) / 1e6, pt.ic, pt.sample_count);
    }

    print_section("Backtest Summary");
    std::printf("  Sharpe:     %.2f\n", rep.backtest.sharpe);
    std::printf("  Return:     %.4f\n", rep.backtest.total_return);
    std::printf("  Turnover:   %.1f\n", rep.backtest.turnover);
    std::printf("  Max DD:     %.2f%%\n", rep.backtest.max_drawdown * 100.0);
    std::printf("  Trades:     %d\n", rep.backtest.trade_count);

    if (write_html_report(report_path, rep)) {
        std::printf("\n  \033[32mHTML report saved:\033[0m %s\n", report_path.c_str());
        std::printf("  Open in Chrome/Edge/Firefox to view charts and book depth.\n");
    } else {
        std::fprintf(stderr, "  Failed to write report: %s\n", report_path.c_str());
    }
}

static void run_interactive() {
    enable_ansi_terminal();
    std::string wal_path = "oms.wal";
    std::string report_path = "oms_report.html";

    for (;;) {
        clear_screen();
        print_banner();
        print_menu();

        char line[32];
        if (!std::fgets(line, sizeof(line), stdin)) break;
        int choice = -1;
        if (std::strlen(line) >= 1) choice = line[0] - '0';

        clear_screen();
        print_banner();

        switch (choice) {
            case 1:
                mode_live_desk(true);
                break;
            case 2:
                mode_matching();
                break;
            case 3:
                mode_spsc_wal(wal_path);
                break;
            case 4:
                mode_alpha_decay();
                break;
            case 5:
                mode_benchmark_quick();
                break;
            case 6:
                run_showcase(report_path);
                break;
            case 7: {
                auto rep = app::mode_lobster_replay(app::default_lobster_sample_path(), true, 0);
                if (rep.replay_events > 0) write_html_report("oms_replay_report.html", rep);
                break;
            }
            case 8:
                app::mode_binance_trades(app::default_binance_trades_path(), 50000);
                break;
            case 9:
                print_section("Market Data Setup");
                std::printf("  Run from project root:\n");
                std::printf("    PowerShell: .\\scripts\\fetch_market_data.ps1\n");
                std::printf("    Bash:       ./scripts/fetch_market_data.sh\n\n");
                std::printf("  LOBSTER sample: %s %s\n", app::default_lobster_sample_path().c_str(),
                            app::file_exists(app::default_lobster_sample_path()) ? "[OK]" : "[missing]");
                std::printf("  Binance trades: %s %s\n", app::default_binance_trades_path().c_str(),
                            app::file_exists(app::default_binance_trades_path()) ? "[OK]" : "[missing]");
                std::printf("\n  Full NASDAQ LOBSTER samples (free):\n");
                std::printf("    https://lobsterdata.com/info/DataSamples.php\n");
                break;
            case 0:
                std::printf("  Goodbye.\n");
                return;
            default:
                std::printf("  Invalid choice.\n");
                break;
        }

        std::printf("\n  Press Enter to continue...");
        std::fflush(stdout);
        std::fgets(line, sizeof(line), stdin);
    }
}

static void print_usage(const char* prog) {
    std::printf(
        "Usage: %s [options]\n"
        "\n"
        "  (no args)       Interactive menu with live visuals\n"
        "  --showcase      Run all demos and write oms_report.html\n"
        "  --replay PATH   Replay LOBSTER NASDAQ message CSV + HTML report\n"
        "  --trades PATH   Analyze Binance trades CSV (VPIN / tape)\n"
        "  --report PATH   HTML report output path\n"
        "  --max N         Max events/trades to process (with --replay/--trades)\n"
        "  --help          Show this help\n"
        "\n"
        "Shippable product: single `oms` executable — console UI + HTML charts.\n",
        prog);
}

}  // namespace

int main(int argc, char* argv[]) {
    std::string report_path = "oms_report.html";
    std::string replay_path;
    std::string trades_path;
    bool showcase = false;
    std::size_t max_events = 0;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        if (std::strcmp(argv[i], "--showcase") == 0) {
            showcase = true;
        } else if (std::strcmp(argv[i], "--report") == 0 && i + 1 < argc) {
            report_path = argv[++i];
        } else if (std::strcmp(argv[i], "--replay") == 0 && i + 1 < argc) {
            replay_path = argv[++i];
        } else if (std::strcmp(argv[i], "--trades") == 0 && i + 1 < argc) {
            trades_path = argv[++i];
        } else if (std::strcmp(argv[i], "--max") == 0 && i + 1 < argc) {
            max_events = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
        } else {
            std::fprintf(stderr, "Unknown option: %s (try --help)\n", argv[i]);
            return 1;
        }
    }

    if (showcase) {
        run_showcase(report_path);
        return 0;
    }

    if (!replay_path.empty()) {
        enable_ansi_terminal();
        print_banner();
        auto rep = app::mode_lobster_replay(replay_path, true, max_events);
        if (rep.replay_events > 0 && write_html_report(report_path, rep)) {
            std::printf("\n  Report: %s\n", report_path.c_str());
        }
        return rep.replay_events > 0 ? 0 : 1;
    }

    if (!trades_path.empty()) {
        enable_ansi_terminal();
        print_banner();
        app::mode_binance_trades(trades_path, max_events > 0 ? max_events : 100000);
        return 0;
    }

    if (argc > 1) {
        std::fprintf(stderr, "Unknown option. Try --help\n");
        return 1;
    }

    run_interactive();
    return 0;
}
