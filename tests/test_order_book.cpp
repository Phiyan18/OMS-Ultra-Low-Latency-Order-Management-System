#include "backtest/alpha_decay.hpp"
#include "backtest/backtest_engine.hpp"
#include "book/order_book.hpp"
#include "engine/matching_engine.hpp"
#include "engine/oms_engine.hpp"
#include "io/lobster_feed.hpp"
#include "io/market_replay.hpp"
#include "io/wal.hpp"
#include "io/wal_replay.hpp"
#include "queue/spsc_queue.hpp"
#include "signals/composite.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>

using namespace oms;

static void test_basic_book() {
    OrderBook<1024, 256> book;

    assert(book.add_order(1, Side::Bid, 100000, 50));
    assert(book.add_order(2, Side::Bid, 99900, 30));
    assert(book.add_order(3, Side::Ask, 100100, 40));
    assert(book.add_order(4, Side::Ask, 100200, 20));

    assert(book.best_bid() == 100000);
    assert(book.best_ask() == 100100);
    assert(book.best_bid_qty() == 50);
    assert(book.mid_price() == (100000 + 100100) / 2);

    assert(book.modify_order(2, 100000, 10));
    assert(book.best_bid_qty() == 60);

    assert(book.execute_order(1, 50));
    assert(book.best_bid_qty() == 10);

    assert(book.cancel_order(2));
    assert(book.best_bid() == INVALID_PRICE);

    assert(book.cancel_order(3));
    assert(book.cancel_order(4));
    assert(book.order_count() == 0);

    std::printf("[PASS] basic book operations\n");
}

static void test_l2_snapshot() {
    OrderBook<1024, 256> book;
    L2BookView view;

    for (OrderId i = 1; i <= 10; ++i) {
        book.add_order(i, Side::Bid, 100000 - static_cast<Price>(i) * 100, 10);
    }
    for (OrderId i = 11; i <= 20; ++i) {
        book.add_order(i, Side::Ask, 100100 + static_cast<Price>(i - 10) * 100, 10);
    }

    book.publish_l2(view);
    L2Snapshot snap = view.load();

    assert(snap.bid_count == 10);
    assert(snap.ask_count == 10);
    assert(snap.best_bid == 99900);
    assert(snap.best_ask == 100200);

    std::printf("[PASS] L2 snapshot\n");
}

static void test_signals() {
    L2Snapshot snap{};
    snap.bids[0] = {100000, 100, 1};
    snap.bids[1] = {99900, 50, 1};
    snap.bid_count = 2;
    snap.asks[0] = {100100, 80, 1};
    snap.asks[1] = {100200, 40, 1};
    snap.ask_count = 2;
    snap.best_bid = 100000;
    snap.best_ask = 100100;

    double obi = OBI::compute(snap, 2);
    assert(std::abs(obi - (150.0 - 120.0) / 270.0) < 1e-9);

    VPIN vpin(50, 100);
    vpin.on_trade(Side::Bid, 60, 100000);
    vpin.on_trade(Side::Ask, 50, 100100);
    assert(vpin.value() >= 0.0);

    MidPriceMomentum mom(10);
    mom.update(100000);
    mom.update(100050);
    mom.update(100100);
    assert(mom.signal() > 0.0);

    std::printf("[PASS] signal computation\n");
}

static void test_backtest() {
    BacktestEngine engine;
    std::mt19937 rng(7);
    std::normal_distribution<double> signal_dist(0.0, 0.1);
    Price mid = 100000;

    for (int i = 0; i < 1000; ++i) {
        double sig = signal_dist(rng);
        mid += static_cast<Price>(signal_dist(rng) * 10);
        engine.on_bar(sig, mid, static_cast<TimestampNs>(i) * 1'000'000LL);
    }

    BacktestResult r = engine.result();
    assert(r.bar_count == 1000);
    std::printf("[PASS] backtest (Sharpe=%.2f, turnover=%.1f)\n", r.sharpe, r.turnover);
}

static void test_pool_no_heap() {
    OrderBook<256, 64> book;
    std::size_t before = book.order_count();

    for (OrderId i = 1; i <= 200; ++i) {
        assert(book.add_order(i, Side::Bid, 100000, 1));
    }
    for (OrderId i = 1; i <= 200; ++i) {
        assert(book.cancel_order(i));
    }
    assert(book.order_count() == before);

    std::printf("[PASS] pool allocator reuse\n");
}

static void test_spsc_queue() {
    SPSCQueue<int, 16> queue;
    assert(queue.try_push(1));
    assert(queue.try_push(2));
    int v = 0;
    assert(queue.try_pop(v) && v == 1);
    assert(queue.try_pop(v) && v == 2);
    assert(!queue.try_pop(v));
    std::printf("[PASS] SPSC queue\n");
}

static void test_matching_engine() {
    OrderBook<256, 64> book;
    MatchingEngine<256, 64> engine(book);

    engine.submit_order(1, Side::Ask, 100100, 100);
    engine.submit_order(2, Side::Ask, 100200, 50);
    MatchResult mr = engine.submit_order(3, Side::Bid, 100150, 120);

    assert(mr.trade_count == 1);
    assert(mr.trades[0].maker_id == 1);
    assert(mr.trades[0].qty == 100);
    assert(mr.remaining_qty == 20);
    assert(mr.resting_added);
    assert(book.best_bid() == 100150);

    std::printf("[PASS] matching engine\n");
}

static void test_wal_replay() {
    const char* path = "test_replay.wal";
    {
        WalWriter wal(path);
        assert(wal.open());
        OrderCommand add{};
        add.type = CommandType::Add;
        add.order_id = 10;
        add.side = Side::Ask;
        add.price = 100100;
        add.qty = 50;
        add.timestamp = 1000;
        assert(wal.append(add));

        add.order_id = 11;
        add.side = Side::Bid;
        add.price = 100150;
        add.qty = 30;
        assert(wal.append(add));
        wal.close();
    }

    OrderBook<256, 64> book;
    MatchingEngine<256, 64> engine(book);
    ReplayStats stats = WalReplayer<256, 64>::replay(path, engine);

    assert(stats.commands == 2);
    assert(stats.errors == 0);
    assert(engine.stats().volume == 30);
    assert(book.order_count() == 1);

    std::remove(path);
    std::printf("[PASS] WAL write + replay\n");
}

static void test_lobster_feed() {
    LobsterMessage msg{};
    assert(parse_lobster_line("34713.685155243,1,206833312,100,118600,-2", msg));
    assert(msg.type == LobsterEventType::Submission);
    assert(msg.order_id == 206833312);
    assert(msg.size == 100);
    assert(msg.price == 1186000);  // $118.60 in OMS fixed-point
    assert(lobster_direction_to_side(-2) == Side::Bid);

    const char* sample = "data/lobster/AMZN_sample_message.csv";
    FILE* f = std::fopen(sample, "rb");
    if (f) {
        std::fclose(f);
        LobsterFeed feed;
        assert(feed.load(sample, 3000));
        OrderBook<8192, 512> book;
        ReplayResult rr = replay_lobster(book, feed, {});
        assert(rr.events_applied > 100);
        assert(rr.submissions > 0);
        std::printf("[PASS] LOBSTER NASDAQ replay (%zu events)\n", rr.events_applied);
    } else {
        std::printf("[SKIP] LOBSTER sample not found (%s)\n", sample);
    }
}

static void test_oms_engine() {
    OmsEngine<256, 512, 64> engine;
    assert(engine.submit_add(1, Side::Ask, 100100, 100));
    assert(engine.submit_add(2, Side::Bid, 100150, 40));
    auto ps = engine.process_all();
    assert(ps.processed == 2);
    assert(ps.trades == 1);
    assert(engine.book().order_count() == 1);
    std::printf("[PASS] OMS engine SPSC drain\n");
}

int main() {
    test_basic_book();
    test_l2_snapshot();
    test_signals();
    test_backtest();
    test_pool_no_heap();
    test_spsc_queue();
    test_matching_engine();
    test_wal_replay();
    test_lobster_feed();
    test_oms_engine();
    std::printf("\nAll tests passed.\n");
    return 0;
}
