#include "book/order_book.hpp"
#include "common/timestamp.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <random>
#include <vector>

using namespace oms;

struct BenchResult {
    const char* name;
    double median_ns;
    double p99_ns;
    double ops_per_sec;
};

static double percentile(std::vector<int64_t>& samples, double p) {
    if (samples.empty()) return 0.0;
    std::sort(samples.begin(), samples.end());
    std::size_t idx = static_cast<std::size_t>(p * (samples.size() - 1));
    return static_cast<double>(samples[idx]);
}

template <typename Fn>
BenchResult bench(const char* name, int warmup, int iterations, Fn&& fn) {
    for (int i = 0; i < warmup; ++i) fn();

    std::vector<int64_t> latencies;
    latencies.reserve(static_cast<std::size_t>(iterations));

    for (int i = 0; i < iterations; ++i) {
        TimestampNs t0 = NanoClock::now();
        fn();
        TimestampNs t1 = NanoClock::now();
        latencies.push_back(t1 - t0);
    }

    double median = percentile(latencies, 0.50);
    double p99 = percentile(latencies, 0.99);
    BenchResult r{name, median, p99, median > 0 ? 1e9 / median : 0.0};
    return r;
}

int main() {
    constexpr int WARMUP = 10000;
    constexpr int ITERS = 1'000'000;

    OrderBook<65536, 4096> book;
    L2BookView l2_view;

    std::mt19937_64 rng(42);
    std::uniform_int_distribution<Price> price_dist(990000, 1010000);
    std::uniform_int_distribution<Quantity> qty_dist(1, 100);

    // Pre-populate book
    for (OrderId id = 1; id <= 10000; ++id) {
        Side side = (id % 2 == 0) ? Side::Bid : Side::Ask;
        book.add_order(id, side, price_dist(rng), qty_dist(rng));
    }
    book.publish_l2(l2_view);

    OrderId next_id = 10001;
    std::vector<BenchResult> results;

    // Insert benchmark
    results.push_back(bench("order_insert", WARMUP, ITERS, [&] {
        Side side = (next_id % 2 == 0) ? Side::Bid : Side::Ask;
        book.add_order(next_id, side, price_dist(rng), qty_dist(rng));
        ++next_id;
    }));

    // Cancel benchmark — keep adding then canceling
    OrderId cancel_id = 1;
    results.push_back(bench("order_cancel", WARMUP, ITERS, [&] {
        if (book.cancel_order(cancel_id)) {
            book.add_order(cancel_id, Side::Bid, price_dist(rng), qty_dist(rng));
        }
        ++cancel_id;
        if (cancel_id > 10000) cancel_id = 1;
    }));

    // Top-of-book query
    results.push_back(bench("top_of_book", WARMUP, ITERS, [&] {
        volatile Price bb = book.best_bid();
        volatile Price ba = book.best_ask();
        (void)bb;
        (void)ba;
    }));

    // L2 lock-free read
    results.push_back(bench("l2_snapshot_read", WARMUP, ITERS, [&] {
        volatile Price mid = l2_view.mid_price();
        (void)mid;
    }));

    // Modify benchmark
    OrderId mod_id = 1;
    results.push_back(bench("order_modify", WARMUP, ITERS / 10, [&] {
        book.modify_order(mod_id, price_dist(rng), qty_dist(rng));
        ++mod_id;
        if (mod_id > 10000) mod_id = 1;
    }));

    // Execute benchmark
    OrderId exec_id = 5001;
    results.push_back(bench("order_execute", WARMUP, ITERS / 10, [&] {
        if (book.execute_order(exec_id, 1)) {
            book.add_order(exec_id, Side::Ask, price_dist(rng), qty_dist(rng));
        }
        ++exec_id;
        if (exec_id > 10000) exec_id = 5001;
    }));

    std::printf("\n=== Ultra-Low Latency Order Book Benchmark ===\n\n");
    std::printf("%-22s %12s %12s %14s\n", "Operation", "Median(ns)", "P99(ns)", "Ops/sec");
    std::printf("%-22s %12s %12s %14s\n", "---------", "----------", "-------", "-------");

    for (const auto& r : results) {
        std::printf("%-22s %12.1f %12.1f %14.0f\n",
                    r.name, r.median_ns, r.p99_ns, r.ops_per_sec);
    }

    std::printf("\n--- Target ---\n");
    std::printf("Order insertion:    ~80ns\n");
    std::printf("Order cancellation: ~45ns\n");
    std::printf("Top of book query:  ~12ns\n");
    std::printf("Throughput:         ~12M orders/sec\n");
    std::printf("\nPool: orders=%zu/%zu levels=%zu/%zu\n",
                book.order_count(), static_cast<std::size_t>(65536),
                static_cast<std::size_t>(0), static_cast<std::size_t>(4096));

    return 0;
}
