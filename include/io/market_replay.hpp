#pragma once

#include "analytics/market_stats.hpp"
#include "backtest/alpha_decay.hpp"
#include "backtest/backtest_engine.hpp"
#include "book/order_book.hpp"
#include "io/lobster_feed.hpp"
#include "signals/composite.hpp"
#include "signals/vpin.hpp"

#include <cstdio>
#include <string>

namespace oms {

struct ReplayConfig {
    std::size_t sample_every{50};     // compute signals every N events
    std::size_t max_events{0};        // 0 = all
    std::size_t progress_every{0};    // print progress every N events (0 = off)
    bool feed_vpin_from_executions{true};
};

struct ReplayResult {
    std::string ticker;
    std::string source;
    std::size_t events_applied{0};
    std::size_t events_skipped{0};
    std::size_t submissions{0};
    std::size_t cancels{0};
    std::size_t executions{0};
    MarketStats stats;
    VolumeProfile volume_profile;
    BacktestResult backtest;
    std::vector<AlphaDecayPoint> decay_curve;
    std::vector<double> composite_history;
    std::vector<double> mid_history;
    L2Snapshot final_book{};
};

template <std::size_t MaxOrders = 65536, std::size_t MaxLevels = 4096>
inline ReplayResult replay_lobster(OrderBook<MaxOrders, MaxLevels>& book,
                                 const LobsterFeed& feed,
                                 const ReplayConfig& cfg = {}) {
    ReplayResult result;
    result.ticker = feed.ticker;
    result.source = "LOBSTER/NASDAQ";

    L2BookView l2_view;
    VPIN vpin(50, 500);
    MidPriceMomentum momentum(20);
    AlphaDecayAnalyzer decay;
    BacktestEngine bt;

    const std::size_t limit =
        cfg.max_events > 0 ? std::min(cfg.max_events, feed.messages.size()) : feed.messages.size();

    for (std::size_t i = 0; i < limit; ++i) {
        const LobsterMessage& msg = feed.messages[i];
        bool ok = true;

        switch (msg.type) {
            case LobsterEventType::Submission: {
                Side side = lobster_direction_to_side(msg.direction);
                ok = book.add_order(msg.order_id, side, msg.price, msg.size, msg.timestamp_ns);
                if (ok) {
                    ++result.submissions;
                    result.volume_profile.add(side, msg.price, msg.size);
                }
                break;
            }
            case LobsterEventType::PartialCancel: {
                const Order* o = book.get_order(msg.order_id);
                if (o) {
                    Quantity new_qty = o->qty - msg.size;
                    if (new_qty > 0)
                        ok = book.modify_order(msg.order_id, o->price, new_qty);
                    else
                        ok = book.cancel_order(msg.order_id);
                } else {
                    ok = false;
                }
                if (ok) ++result.cancels;
                break;
            }
            case LobsterEventType::Deletion:
                ok = book.cancel_order(msg.order_id);
                if (ok) ++result.cancels;
                break;
            case LobsterEventType::VisibleExecution:
            case LobsterEventType::HiddenExecution:
                ok = book.execute_order(msg.order_id, msg.size);
                if (ok) {
                    ++result.executions;
                    result.stats.on_trade(msg.size);
                    if (cfg.feed_vpin_from_executions) {
                        vpin.on_trade(lobster_execution_aggressor(msg.direction), msg.size,
                                      msg.price);
                    }
                }
                break;
            case LobsterEventType::CrossTrade:
                ++result.events_skipped;
                ok = false;
                break;
            default:
                ++result.events_skipped;
                ok = false;
                break;
        }

        if (ok)
            ++result.events_applied;
        else if (msg.type != LobsterEventType::CrossTrade)
            ++result.events_skipped;

        if (cfg.progress_every > 0 && i > 0 && i % cfg.progress_every == 0) {
            std::printf("  ... replayed %zu / %zu events\r", i, limit);
            std::fflush(stdout);
        }

        if (cfg.sample_every > 0 && i % cfg.sample_every == 0) {
            book.publish_l2(l2_view);
            L2Snapshot snap = l2_view.load();
            result.stats.on_l2(snap.best_bid, snap.best_ask, msg.timestamp_ns);

            CompositeSignal sig = CompositeSignal::compute(snap, vpin, momentum);
            decay.add_sample(sig.composite, snap.mid_price(), msg.timestamp_ns);
            bt.on_bar(sig.composite, snap.mid_price(), msg.timestamp_ns);
            result.composite_history.push_back(sig.composite);
            if (snap.mid_price() != INVALID_PRICE)
                result.mid_history.push_back(static_cast<double>(snap.mid_price()) / 10000.0);
        }
    }

    book.publish_l2(l2_view);
    result.final_book = l2_view.load();
    result.backtest = bt.result();  // closes open position, builds equity curve

    std::vector<int64_t> horizons = {10'000'000LL,  50'000'000LL,  100'000'000LL,
                                     500'000'000LL, 1'000'000'000LL, 5'000'000'000LL,
                                     10'000'000'000LL};
    result.decay_curve = decay.compute_decay(horizons);
    return result;
}

inline ReplayResult replay_lobster_file(const std::string& path,
                                        const ReplayConfig& cfg = {}) {
    LobsterFeed feed;
    if (!feed.load(path, cfg.max_events > 0 ? cfg.max_events : 0)) {
        return {};
    }
    OrderBook<8192, 512> book;
    return replay_lobster(book, feed, cfg);
}

}  // namespace oms
