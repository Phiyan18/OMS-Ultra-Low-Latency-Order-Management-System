#pragma once

#include "common/types.hpp"

#include <cmath>
#include <cstddef>
#include <deque>
#include <vector>

namespace oms {

// Volume-synchronized Probability of Informed Trading (VPIN)
// Measures order-flow toxicity via volume-bucketed buy/sell imbalance.
class VPIN {
public:
    explicit VPIN(int bucket_count = 50, int64_t bucket_volume = 1000)
        : bucket_count_(bucket_count), bucket_volume_(bucket_volume) {}

    void on_trade(Side aggressor, Quantity volume, Price /*price*/) noexcept {
        current_buy_ += (aggressor == Side::Bid) ? volume : 0;
        current_sell_ += (aggressor == Side::Ask) ? volume : 0;
        current_vol_ += volume;

        while (current_vol_ >= bucket_volume_) {
            int64_t overflow = current_vol_ - bucket_volume_;
            double buy_frac = static_cast<double>(current_buy_) / static_cast<double>(current_vol_);
            double sell_frac = static_cast<double>(current_sell_) / static_cast<double>(current_vol_);
            double imbalance = std::abs(buy_frac - sell_frac);
            buckets_.push_back(imbalance);
            if (static_cast<int>(buckets_.size()) > bucket_count_) {
                buckets_.pop_front();
            }
            current_buy_ = static_cast<int64_t>(buy_frac * overflow);
            current_sell_ = static_cast<int64_t>(sell_frac * overflow);
            current_vol_ = overflow;
        }
    }

    double value() const noexcept {
        if (buckets_.empty()) return 0.0;
        double sum = 0.0;
        for (double b : buckets_) sum += b;
        return sum / static_cast<double>(buckets_.size());
    }

    void reset() noexcept {
        buckets_.clear();
        current_buy_ = 0;
        current_sell_ = 0;
        current_vol_ = 0;
    }

    int bucket_count() const noexcept { return bucket_count_; }
    std::size_t filled_buckets() const noexcept { return buckets_.size(); }

private:
    int bucket_count_;
    int64_t bucket_volume_;
    int64_t current_buy_{0};
    int64_t current_sell_{0};
    int64_t current_vol_{0};
    std::deque<double> buckets_;
};

}  // namespace oms
