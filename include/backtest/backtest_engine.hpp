#pragma once

#include "common/types.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace oms {

struct BacktestResult {
    double sharpe{0.0};
    double total_return{0.0};   // fractional return (e.g. 0.05 = +5%)
    double turnover{0.0};
    double max_drawdown{0.0};   // fraction capped [0, 1]
    double avg_cost_drag{0.0};
    double final_equity{1.0};
    int trade_count{0};
    int bar_count{0};
    std::vector<double> equity_curve;
};

// Long/short composite signal backtest with linear cost model.
// All PnL and costs are in **return space** (fraction of notional), not dollars.
class BacktestEngine {
public:
    struct Config {
        double signal_threshold{0.05};
        double cost_bps{1.0};       // per-side transaction cost in bps of notional
        double position_limit{1.0}; // max abs position (unitless exposure)
        int64_t bar_interval_ns{1'000'000};
    };

    BacktestEngine() = default;
    explicit BacktestEngine(Config cfg) : cfg_(cfg) {}

    void on_bar(double signal, Price mid, TimestampNs ts) {
        (void)ts;
        if (mid == INVALID_PRICE || mid == 0) return;

        mark_to_market(mid);

        double target_pos = 0.0;
        if (signal > cfg_.signal_threshold) {
            target_pos = cfg_.position_limit;
        } else if (signal < -cfg_.signal_threshold) {
            target_pos = -cfg_.position_limit;
        }

        double delta = target_pos - position_;
        if (std::abs(delta) > 1e-9) {
            // Cost in return space: bps × turnover (do NOT multiply by fixed-point price).
            double cost = std::abs(delta) * (cfg_.cost_bps / 10000.0);
            total_cost_ += cost;
            turnover_ += std::abs(delta);
            ++trade_count_;
            position_ = target_pos;
            entry_mid_ = mid;
        }

        record_equity();
        ++bar_count_;
        last_ts_ = ts;
    }

    BacktestResult result() {
        if (position_ != 0.0 && entry_mid_ != 0 && last_mid_ != 0) {
            mark_to_market(last_mid_);
            record_equity();
        }

        BacktestResult r{};
        r.final_equity = current_equity();
        r.total_return = r.final_equity - 1.0;
        r.turnover = turnover_;
        r.max_drawdown = max_drawdown_;
        r.avg_cost_drag = bar_count_ > 0 ? total_cost_ / static_cast<double>(bar_count_) : 0.0;
        r.trade_count = trade_count_;
        r.bar_count = bar_count_;
        r.equity_curve = equity_curve_;

        if (period_returns_.size() >= 2) {
            double mean = 0.0;
            for (double ret : period_returns_) mean += ret;
            mean /= static_cast<double>(period_returns_.size());

            double var = 0.0;
            for (double ret : period_returns_) {
                double d = ret - mean;
                var += d * d;
            }
            var /= static_cast<double>(period_returns_.size() - 1);
            double stddev = std::sqrt(var);
            // Per-bar Sharpe, annualized with sqrt(bars/year) heuristic when bars are irregular.
            if (stddev > 1e-15) {
                double raw = mean / stddev;
                double ann_factor = std::sqrt(std::max(1.0, static_cast<double>(period_returns_.size())));
                r.sharpe = std::clamp(raw * ann_factor, -20.0, 20.0);
            }
        }
        return r;
    }

    void reset() {
        position_ = 0.0;
        entry_mid_ = 0;
        last_mid_ = 0;
        cumulative_pnl_ = 0.0;
        total_cost_ = 0.0;
        turnover_ = 0.0;
        max_drawdown_ = 0.0;
        peak_equity_ = 1.0;
        trade_count_ = 0;
        bar_count_ = 0;
        last_ts_ = 0;
        period_returns_.clear();
        equity_curve_.clear();
        equity_curve_.push_back(1.0);
    }

    double cost_sensitivity(double cost_bps) const {
        return turnover_ * (cost_bps / 10000.0);
    }

private:
    void mark_to_market(Price mid) {
        if (position_ == 0.0 || entry_mid_ == 0) {
            last_mid_ = mid;
            return;
        }
        double period_ret = position_ * (static_cast<double>(mid - entry_mid_) /
                                        static_cast<double>(entry_mid_));
        cumulative_pnl_ += period_ret;
        period_returns_.push_back(period_ret);
        entry_mid_ = mid;
        last_mid_ = mid;
    }

    double current_equity() const {
        return 1.0 + cumulative_pnl_ - total_cost_;
    }

    void record_equity() {
        double equity = current_equity();
        equity_curve_.push_back(equity);
        if (equity > peak_equity_) peak_equity_ = equity;
        if (peak_equity_ > 1e-12) {
            double dd = (peak_equity_ - equity) / peak_equity_;
            max_drawdown_ = std::max(max_drawdown_, std::clamp(dd, 0.0, 1.0));
        }
    }

    Config cfg_;
    double position_{0.0};
    Price entry_mid_{0};
    Price last_mid_{0};
    double cumulative_pnl_{0.0};
    double total_cost_{0.0};
    double turnover_{0.0};
    double max_drawdown_{0.0};
    double peak_equity_{1.0};
    int trade_count_{0};
    int bar_count_{0};
    TimestampNs last_ts_{0};
    std::vector<double> period_returns_;
    std::vector<double> equity_curve_;
};

}  // namespace oms
