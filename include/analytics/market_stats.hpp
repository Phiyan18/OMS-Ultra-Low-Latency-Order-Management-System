#pragma once

#include "common/types.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace oms {

struct SpreadSample {
    TimestampNs ts{0};
    Price spread{0};
    Price mid{0};
};

// Rolling microstructure statistics from L2 snapshots and trade tape.
class MarketStats {
public:
    void on_l2(Price best_bid, Price best_ask, TimestampNs ts) {
        if (best_bid == INVALID_PRICE || best_ask == INVALID_PRICE) return;
        Price spread = best_ask - best_bid;
        Price mid = (best_bid + best_ask) / 2;
        ++snapshot_count_;
        spread_sum_ += static_cast<double>(spread);
        if (spread < min_spread_) min_spread_ = spread;
        if (spread > max_spread_) max_spread_ = spread;

        if (spread_history_.size() < kMaxHistory) {
            spread_history_.push_back({ts, spread, mid});
        }
    }

    void on_trade(Quantity qty) {
        trade_count_++;
        volume_ += qty;
    }

    double avg_spread() const {
        return snapshot_count_ > 0 ? spread_sum_ / static_cast<double>(snapshot_count_) : 0.0;
    }

    Price min_spread() const { return min_spread_; }
    Price max_spread() const { return max_spread_; }
    uint64_t trade_count() const { return trade_count_; }
    uint64_t volume() const { return volume_; }
    uint64_t snapshots() const { return snapshot_count_; }

    const std::vector<SpreadSample>& spread_history() const { return spread_history_; }

    void reset() {
        snapshot_count_ = 0;
        spread_sum_ = 0.0;
        min_spread_ = INVALID_PRICE;
        max_spread_ = 0;
        trade_count_ = 0;
        volume_ = 0;
        spread_history_.clear();
    }

private:
    static constexpr std::size_t kMaxHistory = 2000;

    uint64_t snapshot_count_{0};
    double spread_sum_{0.0};
    Price min_spread_{INVALID_PRICE};
    Price max_spread_{0};
    uint64_t trade_count_{0};
    uint64_t volume_{0};
    std::vector<SpreadSample> spread_history_;
};

struct VolumeBucket {
    Price price{0};
    uint64_t bid_volume{0};
    uint64_t ask_volume{0};
};

// Simple volume-at-price histogram for session visualization.
class VolumeProfile {
public:
    void add(Side side, Price price, Quantity qty) {
        auto& b = buckets_[price];
        b.price = price;
        if (side == Side::Bid)
            b.bid_volume += static_cast<uint64_t>(qty);
        else
            b.ask_volume += static_cast<uint64_t>(qty);
    }

    std::vector<VolumeBucket> top_levels(std::size_t n = 10) const {
        std::vector<VolumeBucket> out = buckets_.data_;
        std::sort(out.begin(), out.end(), [](const VolumeBucket& a, const VolumeBucket& b) {
            return (a.bid_volume + a.ask_volume) > (b.bid_volume + b.ask_volume);
        });
        if (out.size() > n) out.resize(n);
        return out;
    }

    void reset() { buckets_.clear(); }

private:
    // Sparse map via vector would be slow; unordered_map needs header — keep simple sorted vector rebuild on query.
    // For replay sizes <100k events, linear scan bucket list is OK.
    struct BucketMap {
        VolumeBucket& operator[](Price p) {
            for (auto& b : data_) {
                if (b.price == p) return b;
            }
            data_.push_back({p, 0, 0});
            return data_.back();
        }
        auto begin() const { return data_.begin(); }
        auto end() const { return data_.end(); }
        std::size_t size() const { return data_.size(); }
        void clear() { data_.clear(); }
        std::vector<VolumeBucket> data_;
    } buckets_;
};

}  // namespace oms
