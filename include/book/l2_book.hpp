#pragma once

#include "book/price_level.hpp"
#include "common/types.hpp"

#include <atomic>
#include <cstdint>

namespace oms {

// L2 aggregated view — lock-free readable snapshot via atomic pointers.
struct L2Level {
    Price price{0};
    Quantity qty{0};
    int order_count{0};
};

struct L2Snapshot {
    static constexpr int MAX_LEVELS = 64;

    L2Level bids[MAX_LEVELS]{};
    L2Level asks[MAX_LEVELS]{};
    int bid_count{0};
    int ask_count{0};
    Price best_bid{INVALID_PRICE};
    Price best_ask{INVALID_PRICE};
    TimestampNs timestamp{0};

    Price mid_price() const noexcept {
        if (best_bid == INVALID_PRICE || best_ask == INVALID_PRICE) return INVALID_PRICE;
        return (best_bid + best_ask) / 2;
    }

    Price spread() const noexcept {
        if (best_bid == INVALID_PRICE || best_ask == INVALID_PRICE) return INVALID_PRICE;
        return best_ask - best_bid;
    }
};

class L2BookView {
public:
    void publish(const L2Snapshot& snap) noexcept {
        auto* slot = write_slot();
        *slot = snap;
        std::atomic_thread_fence(std::memory_order_release);
        read_index_.store(1 - read_index_.load(std::memory_order_relaxed),
                           std::memory_order_release);
    }

    L2Snapshot load() const noexcept {
        int idx = read_index_.load(std::memory_order_acquire);
        return buffers_[static_cast<std::size_t>(idx)];
    }

    Price best_bid() const noexcept { return load().best_bid; }
    Price best_ask() const noexcept { return load().best_ask; }
    Price mid_price() const noexcept { return load().mid_price(); }

private:
    L2Snapshot* write_slot() noexcept {
        int idx = 1 - read_index_.load(std::memory_order_relaxed);
        return &buffers_[static_cast<std::size_t>(idx)];
    }

    L2Snapshot buffers_[2]{};
    mutable std::atomic<int> read_index_{0};
};

}  // namespace oms
