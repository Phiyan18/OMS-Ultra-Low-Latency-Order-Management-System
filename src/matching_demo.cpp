#include "engine/matching_engine.hpp"
#include "engine/oms_engine.hpp"
#include "io/wal.hpp"
#include "io/wal_replay.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace oms;

static void run_multithreaded_feed(const std::string& wal_path) {
    OmsEngine<65536, 8192, 1024> engine;
    WalWriter wal(wal_path);

    if (!wal.open()) {
        std::fprintf(stderr, "Failed to open WAL: %s\n", wal_path.c_str());
        return;
    }

    std::atomic<bool> done{false};
    std::atomic<int> produced{0};
    std::atomic<int> consumed_trades{0};

    // Consumer thread — single writer to the book
    std::thread consumer([&] {
        while (!done.load(std::memory_order_acquire) || engine.pending() > 0) {
            auto ps = engine.process_all(&wal);
            consumed_trades.fetch_add(ps.trades, std::memory_order_relaxed);
            if (!done.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        }
    });

    // Producer thread — lock-free SPSC push
    std::thread producer([&] {
        Price bid = 1'000'000;
        Price ask = 1'000'100;
        OrderId id = 1;

        for (int i = 0; i < 2000; ++i) {
            while (!engine.submit_add(id, Side::Bid, bid, 10 + (i % 20))) {
                std::this_thread::yield();
            }
            ++produced;
            ++id;

            while (!engine.submit_add(id, Side::Ask, ask, 10 + (i % 20))) {
                std::this_thread::yield();
            }
            ++produced;
            ++id;

            // Aggressive order to cross spread and generate trades
            if (i % 50 == 0) {
                while (!engine.submit_add(id, Side::Bid, ask, 5)) {
                    std::this_thread::yield();
                }
                ++produced;
                ++id;
            }

            if (i % 100 == 0) {
                while (!engine.submit_cancel(id - 10)) {
                    std::this_thread::yield();
                }
                ++produced;
            }
        }
        done.store(true, std::memory_order_release);
    });

    producer.join();
    consumer.join();
    wal.close();

    std::printf("\n=== Multithreaded SPSC Feed ===\n");
    std::printf("  Commands produced:  %d\n", produced.load());
    std::printf("  Trades generated:   %d\n", consumed_trades.load());
    std::printf("  WAL records:        %zu\n", wal.record_count());
    std::printf("  Book orders:        %zu\n", engine.book().order_count());
    std::printf("  Best bid / ask:     %lld / %lld\n",
                static_cast<long long>(engine.book().best_bid()),
                static_cast<long long>(engine.book().best_ask()));
}

static void run_wal_replay(const std::string& wal_path) {
    OrderBook<8192, 1024> book;
    MatchingEngine<8192, 1024> engine(book);

    ReplayStats stats = WalReplayer<8192, 1024>::replay(wal_path, engine);

    std::printf("\n=== WAL Replay ===\n");
    std::printf("  File:               %s\n", wal_path.c_str());
    std::printf("  Commands replayed:  %zu\n", stats.commands);
    std::printf("  Trades (from log):  %zu\n", stats.trades);
    std::printf("  Errors:             %zu\n", stats.errors);
    std::printf("  Rebuilt orders:     %zu\n", book.order_count());
    std::printf("  Best bid / ask:     %lld / %lld\n",
                static_cast<long long>(book.best_bid()),
                static_cast<long long>(book.best_ask()));
    std::printf("  Matcher volume:     %llu\n",
                static_cast<unsigned long long>(engine.stats().volume));
}

static void run_matching_demo() {
    OrderBook<1024, 256> book;
    MatchingEngine<1024, 256> engine(book);

    engine.submit_order(1, Side::Ask, 1'000'100, 100);
    engine.submit_order(2, Side::Ask, 1'000'200, 50);
    engine.submit_order(3, Side::Bid, 1'000'000, 80);

    MatchResult cross = engine.submit_order(4, Side::Bid, 1'000'150, 120);

    std::printf("\n=== Matching Engine ===\n");
    std::printf("  Trades from cross:  %d\n", cross.trade_count);
    for (int i = 0; i < cross.trade_count; ++i) {
        const Trade& t = cross.trades[i];
        std::printf("    Trade: maker=%llu taker=%llu px=%lld qty=%lld\n",
                    static_cast<unsigned long long>(t.maker_id),
                    static_cast<unsigned long long>(t.taker_id),
                    static_cast<long long>(t.price),
                    static_cast<long long>(t.qty));
    }
    std::printf("  Remaining qty:      %lld\n", static_cast<long long>(cross.remaining_qty));
    std::printf("  Resting added:      %s\n", cross.resting_added ? "yes" : "no");
    std::printf("  Book best bid/ask:  %lld / %lld\n",
                static_cast<long long>(book.best_bid()),
                static_cast<long long>(book.best_ask()));
}

int main(int argc, char* argv[]) {
    const std::string wal_path = (argc > 1) ? argv[1] : "oms.wal";

    std::printf("=== OMS Matching + SPSC + WAL Demo ===\n");

    run_matching_demo();
    run_multithreaded_feed(wal_path);
    run_wal_replay(wal_path);

    std::printf("\nDone. WAL saved to: %s\n", wal_path.c_str());
    std::printf("Replay with: oms_matching_demo %s\n", wal_path.c_str());
    return 0;
}
