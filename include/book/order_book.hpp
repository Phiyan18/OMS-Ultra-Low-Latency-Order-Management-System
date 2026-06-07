#pragma once

#include "book/l2_book.hpp"
#include "book/price_level.hpp"
#include "common/timestamp.hpp"
#include "memory/pool_allocator.hpp"

#include <array>
#include <cstdint>
#include <unordered_map>

namespace oms {

template <std::size_t MaxOrders = 1'048'576, std::size_t MaxPriceLevels = 65536>
class OrderBook {
public:
    struct Stats {
        uint64_t adds{0};
        uint64_t modifies{0};
        uint64_t cancels{0};
        uint64_t executes{0};
    };

    OrderBook() = default;

    // --- O(1) hot path operations (single-writer, no mutex) ---

    bool add_order(OrderId id, Side side, Price price, Quantity qty,
                   TimestampNs ts = 0) noexcept {
        if (order_index_.count(id)) return false;
        Order* order = order_pool_.acquire();
        if (!order) return false;

        order->id = id;
        order->price = price;
        order->qty = qty;
        order->side = side;

        PriceLevel* level = get_or_create_level(side, price);
        if (!level) {
            order_pool_.release(order);
            return false;
        }
        level->append(order);
        order_index_[id] = order;

        update_best(side, level);
        ++stats_.adds;
        last_ts_ = ts ? ts : NanoClock::now();
        return true;
    }

    bool modify_order(OrderId id, Price new_price, Quantity new_qty) noexcept {
        auto it = order_index_.find(id);
        if (it == order_index_.end()) return false;
        Order* order = it->second;

        if (new_price == order->price) {
            Quantity old = order->qty;
            order->qty = new_qty;
            order->level->update_qty(order, old);
        } else {
            Side side = order->side;
            Price old_price = order->price;
            Quantity old_qty = order->qty;
            PriceLevel* old_level = order->level;
            bool old_level_was_best = (best_levels_[side_index(side)] == old_level);
            old_level->remove(order);

            bool old_level_removed = false;
            if (old_level->empty()) {
                remove_level(old_level);
                old_level_removed = true;
            }

            PriceLevel* new_level = get_or_create_level(side, new_price);
            if (!new_level) {
                order->price = old_price;
                order->qty = old_qty;
                PriceLevel* restore = old_level_removed
                    ? get_or_create_level(side, old_price)
                    : old_level;
                if (restore) {
                    restore->append(order);
                    if (old_level_was_best || !best_levels_[side_index(side)]) {
                        update_best(side, restore);
                    }
                }
                return false;
            }
            order->price = new_price;
            order->qty = new_qty;
            new_level->append(order);
            update_best(side, new_level);
        }

        ++stats_.modifies;
        last_ts_ = NanoClock::now();
        return true;
    }

    bool cancel_order(OrderId id) noexcept {
        auto it = order_index_.find(id);
        if (it == order_index_.end()) return false;
        Order* order = it->second;
        Side side = order->side;
        PriceLevel* level = order->level;

        level->remove(order);
        order_index_.erase(it);

        if (level->empty()) {
            remove_level(level);
        }

        order->reset();
        order_pool_.release(order);
        ++stats_.cancels;
        last_ts_ = NanoClock::now();
        return true;
    }

    bool execute_order(OrderId id, Quantity fill_qty) noexcept {
        auto it = order_index_.find(id);
        if (it == order_index_.end()) return false;
        Order* order = it->second;
        if (fill_qty <= 0 || fill_qty > order->qty) return false;

        Side side = order->side;
        PriceLevel* level = order->level;

        if (fill_qty == order->qty) {
            level->remove(order);
            order_index_.erase(it);
            if (level->empty()) {
                remove_level(level);
            }
            order->reset();
            order_pool_.release(order);
        } else {
            Quantity old = order->qty;
            order->qty -= fill_qty;
            level->update_qty(order, old);
        }

        ++stats_.executes;
        last_ts_ = NanoClock::now();
        return true;
    }

    // --- L2 / top-of-book queries ---

    Price best_bid() const noexcept {
        auto* lvl = best_levels_[0];
        return lvl ? lvl->price : INVALID_PRICE;
    }

    Price best_ask() const noexcept {
        auto* lvl = best_levels_[1];
        return lvl ? lvl->price : INVALID_PRICE;
    }

    Quantity best_bid_qty() const noexcept {
        auto* lvl = best_levels_[0];
        return lvl ? lvl->total_qty : 0;
    }

    Quantity best_ask_qty() const noexcept {
        auto* lvl = best_levels_[1];
        return lvl ? lvl->total_qty : 0;
    }

    Price mid_price() const noexcept {
        Price bb = best_bid();
        Price ba = best_ask();
        if (bb == INVALID_PRICE || ba == INVALID_PRICE) return INVALID_PRICE;
        return (bb + ba) / 2;
    }

    const Order* get_order(OrderId id) const noexcept {
        auto it = order_index_.find(id);
        return it != order_index_.end() ? it->second : nullptr;
    }

    const Order* front_at_best(Side side) const noexcept {
        auto* lvl = best_levels_[side_index(side)];
        return lvl ? lvl->head : nullptr;
    }

    bool would_cross(Side incoming, Price limit_price) const noexcept {
        if (incoming == Side::Bid) {
            Price ask = best_ask();
            return ask != INVALID_PRICE && ask <= limit_price;
        }
        Price bid = best_bid();
        return bid != INVALID_PRICE && bid >= limit_price;
    }

    std::size_t order_count() const noexcept { return order_index_.size(); }

    L2Snapshot build_l2_snapshot(int depth = L2Snapshot::MAX_LEVELS) const noexcept {
        L2Snapshot snap{};
        snap.best_bid = best_bid();
        snap.best_ask = best_ask();
        snap.timestamp = last_ts_;

        int bi = 0;
        for (PriceLevel* lvl = best_levels_[0]; lvl && bi < depth; lvl = lvl->next, ++bi) {
            snap.bids[bi] = {lvl->price, lvl->total_qty, lvl->order_count};
        }
        snap.bid_count = bi;

        int ai = 0;
        for (PriceLevel* lvl = best_levels_[1]; lvl && ai < depth; lvl = lvl->next, ++ai) {
            snap.asks[ai] = {lvl->price, lvl->total_qty, lvl->order_count};
        }
        snap.ask_count = ai;
        return snap;
    }

    void publish_l2(L2BookView& view) const noexcept {
        view.publish(build_l2_snapshot());
    }

    const Stats& stats() const noexcept { return stats_; }
    TimestampNs last_timestamp() const noexcept { return last_ts_; }

private:
    static constexpr std::size_t side_index(Side s) noexcept {
        return s == Side::Bid ? 0 : 1;
    }

    PriceLevel* get_or_create_level(Side side, Price price) noexcept {
        uint64_t key = level_key(side, price);
        auto it = level_index_.find(key);
        if (it != level_index_.end()) return it->second;

        PriceLevel* level = level_pool_.acquire();
        if (!level) return nullptr;

        level->price = price;
        level->side = side;
        level->total_qty = 0;
        level->order_count = 0;
        level->head = nullptr;
        level->tail = nullptr;
        level->prev = nullptr;
        level->next = nullptr;

        insert_level_sorted(level);
        level_index_[key] = level;
        return level;
    }

    void insert_level_sorted(PriceLevel* level) noexcept {
        Side side = level->side;
        PriceLevel*& best = best_levels_[side_index(side)];

        if (!best) {
            best = level;
            return;
        }

        // Bids: higher price is better (insert before worse levels)
        // Asks: lower price is better
        if (side == Side::Bid) {
            if (level->price > best->price) {
                level->next = best;
                best->prev = level;
                best = level;
                return;
            }
            PriceLevel* cur = best;
            while (cur->next && cur->next->price > level->price) {
                cur = cur->next;
            }
            level->next = cur->next;
            level->prev = cur;
            if (cur->next) cur->next->prev = level;
            cur->next = level;
        } else {
            if (level->price < best->price) {
                level->next = best;
                best->prev = level;
                best = level;
                return;
            }
            PriceLevel* cur = best;
            while (cur->next && cur->next->price < level->price) {
                cur = cur->next;
            }
            level->next = cur->next;
            level->prev = cur;
            if (cur->next) cur->next->prev = level;
            cur->next = level;
        }
    }

    void remove_level(PriceLevel* level) noexcept {
        Side side = level->side;
        uint64_t key = level_key(side, level->price);
        level_index_.erase(key);

        if (level->prev) {
            level->prev->next = level->next;
        } else {
            best_levels_[side_index(side)] = level->next;
        }
        if (level->next) {
            level->next->prev = level->prev;
        }

        level->price = 0;
        level->total_qty = 0;
        level->order_count = 0;
        level->head = nullptr;
        level->tail = nullptr;
        level->prev = nullptr;
        level->next = nullptr;
        level_pool_.release(level);
    }

    void update_best(Side side, PriceLevel* level) noexcept {
        PriceLevel*& best = best_levels_[side_index(side)];
        if (!best) {
            best = level;
            return;
        }
        if (side == Side::Bid) {
            if (level->price > best->price) best = level;
        } else {
            if (level->price < best->price) best = level;
        }
    }


    static uint64_t level_key(Side side, Price price) noexcept {
        return (static_cast<uint64_t>(static_cast<uint8_t>(side)) << 56) |
               (static_cast<uint64_t>(price) & 0x00FFFFFFFFFFFFFFULL);
    }

    PoolAllocator<Order, MaxOrders> order_pool_;
    PoolAllocator<PriceLevel, MaxPriceLevels> level_pool_;
    std::unordered_map<OrderId, Order*> order_index_;
    std::unordered_map<uint64_t, PriceLevel*> level_index_;
    std::array<PriceLevel*, 2> best_levels_{nullptr, nullptr};
    Stats stats_{};
    TimestampNs last_ts_{0};
};

}  // namespace oms
