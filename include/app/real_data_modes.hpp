#pragma once

#include "io/binance_trades_feed.hpp"
#include "io/lobster_feed.hpp"
#include "io/market_replay.hpp"
#include "ui/console_visual.hpp"
#include "ui/html_report.hpp"

#include <cstdio>
#include <string>

namespace oms::app {

inline ui::SessionReport replay_result_to_report(const ReplayResult& rr) {
    ui::SessionReport r{};
    r.final_book = rr.final_book;
    r.backtest = rr.backtest;
    r.decay_curve = rr.decay_curve;
    r.composite_history = rr.composite_history;
    r.mid_history = rr.mid_history;
    r.data_source = rr.source;
    r.ticker = rr.ticker;
    r.replay_events = rr.events_applied;
    r.real_trade_count = rr.stats.trade_count();
    r.real_volume = rr.stats.volume();
    r.avg_spread = rr.stats.avg_spread();
    r.book_adds = rr.submissions;
    r.book_cancels = rr.cancels;
    r.book_executes = rr.executions;
    for (const auto& s : rr.stats.spread_history()) {
        r.spread_history.push_back(static_cast<double>(s.spread) / 10000.0);
    }
    r.equity_history = rr.backtest.equity_curve;
    r.order_count = 0;  // filled by caller if needed
    return r;
}

inline std::string default_lobster_sample_path() {
    return "data/lobster/AMZN_sample_message.csv";
}

inline std::string default_binance_trades_path() {
    return "data/binance/BTCUSDT-trades-2024-06-01.csv";
}

inline bool file_exists(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

inline ui::SessionReport mode_lobster_replay(const std::string& path, bool visual, std::size_t max_events) {
    using namespace oms::ui;
    print_section("Real NASDAQ Data Replay (LOBSTER format)");

    if (!file_exists(path)) {
        std::printf("  \033[31mFile not found:\033[0m %s\n", path.c_str());
        std::printf("  Run: scripts\\fetch_market_data.ps1  (or fetch_market_data.sh)\n");
        std::printf("  Or download free samples: https://lobsterdata.com/info/DataSamples.php\n");
        return {};
    }

    LobsterFeed feed;
    if (!feed.load(path, max_events)) {
        std::printf("  Failed to parse LOBSTER message file.\n");
        return {};
    }

    std::printf("  Ticker:   %s\n", feed.ticker.c_str());
    std::printf("  Events:   %zu\n", feed.messages.size());
    std::printf("  Source:   NASDAQ TotalView-ITCH via LOBSTER\n\n");

    OrderBook<8192, 512> book;
    ReplayConfig cfg{};
    cfg.max_events = max_events;
    cfg.sample_every = 25;
    cfg.progress_every = visual ? 500 : 2000;

    ReplayResult rr = replay_lobster(book, feed, cfg);
    std::printf("\n");

    std::printf("  Applied: %zu | Skipped: %zu | Executions: %zu | Volume: %llu\n",
                rr.events_applied, rr.events_skipped, rr.executions,
                static_cast<unsigned long long>(rr.stats.volume()));
    std::printf("  Avg spread: $%.4f | Backtest Sharpe: %.2f\n", rr.stats.avg_spread() / 10000.0,
                rr.backtest.sharpe);

    render_l2_ladder(rr.final_book, 8);
    print_sparkline(rr.mid_history);

    if (!rr.composite_history.empty()) {
        std::printf("\n  Composite signal:\n  ");
        print_sparkline(rr.composite_history, 48);
    }

    auto profile = rr.volume_profile.top_levels(5);
    if (!profile.empty()) {
        std::printf("\n  Top volume-at-price (session):\n");
        for (const auto& b : profile) {
            std::printf("    $%.2f  bid=%llu ask=%llu\n", b.price / 10000.0,
                        static_cast<unsigned long long>(b.bid_volume),
                        static_cast<unsigned long long>(b.ask_volume));
        }
    }

    return replay_result_to_report(rr);
}

inline void mode_binance_trades(const std::string& path, std::size_t max_trades) {
    using namespace oms::ui;
    print_section("Real Crypto Trade Tape (Binance public data)");

    if (!file_exists(path)) {
        std::printf("  \033[31mFile not found:\033[0m %s\n", path.c_str());
        std::printf("  Run: scripts\\fetch_market_data.ps1\n");
        return;
    }

    BinanceTradesFeed feed;
    if (!feed.load(path, max_trades)) {
        std::printf("  Failed to parse Binance trades CSV.\n");
        return;
    }

    std::printf("  Symbol:   %s\n", feed.symbol.c_str());
    std::printf("  Trades:   %zu\n", feed.trades.size());
    std::printf("  Source:   https://data.binance.vision\n\n");

    VPIN vpin(100, 1000);
    MidPriceMomentum momentum(50);
    MarketStats stats;
    std::vector<double> prices;
    std::vector<double> vpin_hist;

    for (const BinanceTrade& t : feed.trades) {
        stats.on_trade(t.qty);
        prices.push_back(static_cast<double>(t.price) / 10000.0);
        Side aggressor = t.buyer_is_maker ? Side::Ask : Side::Bid;
        vpin.on_trade(aggressor, t.qty, t.price);
        momentum.update(t.price);
        if (prices.size() % 500 == 0) vpin_hist.push_back(vpin.value());
    }

    std::printf("  Total volume: %llu | VPIN: %.4f | Momentum: %+.6f\n",
                static_cast<unsigned long long>(stats.volume()), vpin.value(), momentum.signal());
    if (!prices.empty()) {
        std::printf("  Price range: $%.2f — $%.2f\n", prices.front(), prices.back());
    }

    std::printf("\n  Price tape:\n  ");
    print_sparkline(prices, 64);
    if (!vpin_hist.empty()) {
        std::printf("\n  VPIN evolution:\n  ");
        print_sparkline(vpin_hist, 48);
    }
}

}  // namespace oms::app
