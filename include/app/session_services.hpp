#pragma once

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
#include "ui/html_report.hpp"

#include <atomic>
#include <cstdio>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace oms::app {

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

inline void run_simulation_step(SimulationState& s, int event, OrderId& id) {
    std::uniform_int_distribution<Price> price_jitter(-500, 500);
    const Price base_bid = 1'000'000;
    const Price base_ask = 1'000'100;
    const TimestampNs ts = NanoClock::now();

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
        const L2Snapshot snap = s.l2_view.load();
        const CompositeSignal sig = CompositeSignal::compute(snap, s.vpin, s.momentum);
        s.decay.add_sample(sig.composite, snap.mid_price(), ts);
        s.backtest.on_bar(sig.composite, snap.mid_price(), ts);
        s.composite_history.push_back(sig.composite);
        if (snap.mid_price() != INVALID_PRICE)
            s.mid_history.push_back(static_cast<double>(snap.mid_price()) / 10000.0);
    }

    if (event % 7 == 0) {
        s.vpin.on_trade(event % 2 == 0 ? Side::Bid : Side::Ask, 100, s.book.mid_price());
    }
}

inline ui::SessionReport build_session_report(SimulationState& s) {
    ui::SessionReport r{};
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

    const std::vector<int64_t> horizons = {10'000'000LL,  50'000'000LL,  100'000'000LL,
                                           500'000'000LL, 1'000'000'000LL, 5'000'000'000LL,
                                           10'000'000'000LL};
    r.decay_curve = s.decay.compute_decay(horizons);
    return r;
}

inline ui::SessionReport run_simulation_session(int events = 5000) {
    SimulationState sim;
    OrderId id = 1;
    for (int e = 0; e < events; ++e) run_simulation_step(sim, e, id);
    return build_session_report(sim);
}

struct MatchDemoResult {
    MatchResult cross{};
    L2Snapshot final_book{};
    int trade_count{0};
};

inline MatchDemoResult run_matching_demo() {
    OrderBook<1024, 256> book;
    MatchingEngine<1024, 256> engine(book);
    engine.submit_order(1, Side::Ask, 1'000'100, 100);
    engine.submit_order(2, Side::Ask, 1'000'200, 50);
    engine.submit_order(3, Side::Bid, 1'000'000, 80);
    MatchDemoResult result{};
    result.cross = engine.submit_order(4, Side::Bid, 1'000'150, 120);
    result.trade_count = result.cross.trade_count;
    L2BookView view;
    book.publish_l2(view);
    result.final_book = view.load();
    return result;
}

inline ui::SessionReport run_spsc_wal_session(const std::string& wal_path) {
    OmsEngine<2048, 4096, 256> engine;
    WalWriter wal(wal_path);
    ui::SessionReport rep{};
    if (!wal.open()) return rep;

    std::atomic<bool> done{false};
    std::atomic<int> produced{0};
    std::atomic<int> trades{0};

    std::thread consumer([&] {
        while (!done.load(std::memory_order_acquire) || engine.pending() > 0) {
            const auto ps = engine.process_all(&wal);
            trades.fetch_add(ps.trades, std::memory_order_relaxed);
            if (!done.load(std::memory_order_acquire)) std::this_thread::yield();
        }
    });

    std::thread producer([&] {
        const Price bid = 1'000'000;
        const Price ask = 1'000'100;
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

    OrderBook<4096, 256> replay_book;
    MatchingEngine<4096, 256> replay_engine(replay_book);
    const ReplayStats stats = WalReplayer<4096, 256>::replay(wal_path, replay_engine);
    rep.wal_commands = static_cast<int>(stats.commands);
    rep.wal_trades = static_cast<int>(stats.trades);
    return rep;
}

inline ui::SessionReport run_full_showcase(const std::string& wal_path = "oms.wal") {
    ui::SessionReport rep = run_simulation_session(5000);
    (void)run_matching_demo();
    const ui::SessionReport wal_rep = run_spsc_wal_session(wal_path);
    rep.spsc_produced = wal_rep.spsc_produced;
    rep.spsc_trades = wal_rep.spsc_trades;
    rep.wal_commands = wal_rep.wal_commands;
    rep.wal_trades = wal_rep.wal_trades;
    return rep;
}

inline bool file_exists(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

inline std::string default_lobster_sample_path() {
    return "data/lobster/AMZN_sample_message.csv";
}

inline std::string default_binance_trades_path() {
    return "data/binance/BTCUSDT-trades-2024-06-01.csv";
}

}  // namespace oms::app
