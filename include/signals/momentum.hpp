#pragma once

#include "common/types.hpp"

#include <cmath>
#include <cstddef>
#include <deque>
#include <vector>

namespace oms {

// Mid-price momentum: EMA-based return signal over rolling window.
class MidPriceMomentum {
public:
    explicit MidPriceMomentum(int window = 20, double alpha = 0.0)
        : window_(window), alpha_(alpha > 0 ? alpha : 2.0 / (window + 1)) {}

    void update(Price mid) noexcept {
        if (mid == INVALID_PRICE) return;
        if (prev_mid_ == INVALID_PRICE) {
            prev_mid_ = mid;
            ema_ = static_cast<double>(mid);
            return;
        }
        double ret = static_cast<double>(mid - prev_mid_) / static_cast<double>(prev_mid_);
        returns_.push_back(ret);
        if (static_cast<int>(returns_.size()) > window_) {
            returns_.pop_front();
        }
        ema_ = alpha_ * static_cast<double>(mid) + (1.0 - alpha_) * ema_;
        prev_mid_ = mid;
    }

    double signal() const noexcept {
        if (returns_.empty()) return 0.0;
        double sum = 0.0;
        for (double r : returns_) sum += r;
        return sum / static_cast<double>(returns_.size());
    }

    double ema() const noexcept { return ema_; }

    void reset() noexcept {
        returns_.clear();
        prev_mid_ = INVALID_PRICE;
        ema_ = 0.0;
    }

private:
    int window_;
    double alpha_;
    Price prev_mid_{INVALID_PRICE};
    double ema_{0.0};
    std::deque<double> returns_;
};

}  // namespace oms
