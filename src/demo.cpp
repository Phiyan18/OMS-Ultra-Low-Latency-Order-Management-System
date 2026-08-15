#include "backtest/alpha_decay.hpp"
#include "backtest/backtest_engine.hpp"
#include "book/order_book.hpp"
#include "common/timestamp.hpp"
#include "signals/composite.hpp"
#include "signals/obi.hpp"
#include "signals/vpin.hpp"

#include <cstdio>
#include <random>
#include <vector>

using namespace oms;

int main() {
    OrderBook<8192, 1024> book;
    L2BookView l2_view;
    VPIN vpin(50, 500);
    MidPriceMomentum momentum(20);
    AlphaDecayAnalyzer decay;
    BacktestEngine backtest;

    std::mt19937_64 rng(123);
    std::uniform_int_distribution<Price> price_jitter(-500, 500);
    Price base_bid = 1000000;
    Price base_ask = 1000100;

    std::printf("=== OMS Demo: Order Book + Signals + Backtest ===\n\n");

    // Simulate 5000 order events
    OrderId id = 1;
    for (int event = 0; event < 5000; ++event) {
        int action = event % 5;
        TimestampNs ts = NanoClock::now();

        switch (action) {
            case 0:
            case 1:
                book.add_order(id++, Side::Bid, base_bid + price_jitter(rng),
                               10 + (event % 50));
                break;
            case 2:
                book.add_order(id++, Side::Ask, base_ask + price_jitter(rng),
                               10 + (event % 50));
                break;
            case 3:
                if (id > 100) book.cancel_order(id - 50);
                break;
            case 4:
                if (id > 200) book.execute_order(id - 100, 5);
                break;
        }

        if (event % 10 == 0) {
            book.publish_l2(l2_view);
            L2Snapshot snap = l2_view.load();

            CompositeSignal sig = CompositeSignal::compute(snap, vpin, momentum);
            decay.add_sample(sig.composite, snap.mid_price(), ts);
            backtest.on_bar(sig.composite, snap.mid_price(), ts);

            if (event % 500 == 0) {
                std::printf("Event %5d | mid=%ld | OBI=%+.4f VPIN=%.4f Mom=%+.6f Composite=%+.4f\n",
                            event, static_cast<long>(snap.mid_price()),
                            sig.obi, sig.vpin, sig.momentum, sig.composite);
            }
        }

        // Simulate trades for VPIN
        if (event % 7 == 0) {
            vpin.on_trade(event % 2 == 0 ? Side::Bid : Side::Ask, 100,
                          book.mid_price());
        }
    }

    // Alpha decay curves: 10ms → 10s
    std::vector<int64_t> horizons = {
        10'000'000LL,       // 10ms
        50'000'000LL,       // 50ms
        100'000'000LL,      // 100ms
        500'000'000LL,      // 500ms
        1'000'000'000LL,    // 1s
        5'000'000'000LL,     // 5s
        10'000'000'000LL,    // 10s
    };

    auto decay_curve = decay.compute_decay(horizons);

    std::printf("\n--- Alpha Decay (IC vs Horizon) ---\n");
    for (const auto& pt : decay_curve) {
        double ms = static_cast<double>(pt.horizon_ns) / 1e6;
        std::printf("  Horizon %8.1fms | IC=%+.4f | n=%d\n",
                    ms, pt.ic, pt.sample_count);
    }

    BacktestResult bt = backtest.result();
    std::printf("\n--- Composite Signal Backtest ---\n");
    std::printf("  Sharpe:       %.2f\n", bt.sharpe);
    std::printf("  Return:       %.4f\n", bt.total_return);
    std::printf("  Turnover:     %.1f\n", bt.turnover);
    std::printf("  Max DD:       %.2f%%\n", bt.max_drawdown * 100);
    std::printf("  Trades:       %d\n", bt.trade_count);

    std::printf("\n--- Cost Sensitivity ---\n");
    for (double bps : {0.5, 1.0, 2.0, 5.0}) {
        std::printf("  Cost %.1f bps → est drag %.4f\n", bps, backtest.cost_sensitivity(bps));
    }

    std::printf("\nBook stats: adds=%llu cancels=%llu executes=%llu orders=%zu\n",
                book.stats().adds, book.stats().cancels, book.stats().executes,
                book.order_count());

    return 0;
}
