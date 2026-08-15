#pragma once

#include "app/session_services.hpp"
#include "io/binance_trades_feed.hpp"
#include "io/lobster_feed.hpp"
#include "io/market_replay.hpp"
#include "ui/html_report.hpp"

#include <string>
#include <vector>

namespace oms::app {

struct BinanceTapeResult {
    std::string symbol;
    std::size_t trade_count{0};
    uint64_t total_volume{0};
    double vpin{0.0};
    double momentum{0.0};
    std::vector<double> prices;
    std::vector<double> vpin_history;
    bool ok{false};
    std::string error;
};

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
    return r;
}

inline ui::SessionReport run_lobster_replay(const std::string& path, std::size_t max_events = 0) {
    ui::SessionReport report{};
    if (!file_exists(path)) {
        report.data_source = "error: file not found";
        return report;
    }

    LobsterFeed feed;
    if (!feed.load(path, max_events)) {
        report.data_source = "error: parse failed";
        return report;
    }

    OrderBook<8192, 512> book;
    ReplayConfig cfg{};
    cfg.max_events = max_events;
    cfg.sample_every = 25;
    cfg.progress_every = 0;

    const ReplayResult rr = replay_lobster(book, feed, cfg);
    return replay_result_to_report(rr);
}

inline BinanceTapeResult run_binance_trades(const std::string& path, std::size_t max_trades = 100000) {
    BinanceTapeResult result{};
    if (!file_exists(path)) {
        result.error = "file not found";
        return result;
    }

    BinanceTradesFeed feed;
    if (!feed.load(path, max_trades)) {
        result.error = "parse failed";
        return result;
    }

    result.symbol = feed.symbol;
    result.trade_count = feed.trades.size();
    result.ok = true;

    VPIN vpin(100, 1000);
    MidPriceMomentum momentum(50);
    MarketStats stats;

    for (const BinanceTrade& t : feed.trades) {
        stats.on_trade(t.qty);
        result.prices.push_back(static_cast<double>(t.price) / 10000.0);
        const Side aggressor = t.buyer_is_maker ? Side::Ask : Side::Bid;
        vpin.on_trade(aggressor, t.qty, t.price);
        momentum.update(t.price);
        if (result.prices.size() % 500 == 0) result.vpin_history.push_back(vpin.value());
    }

    result.total_volume = stats.volume();
    result.vpin = vpin.value();
    result.momentum = momentum.signal();
    return result;
}

}  // namespace oms::app
