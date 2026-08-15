#pragma once

#include "book/order_book.hpp"
#include "book/trade.hpp"
#include "common/timestamp.hpp"

#include <algorithm>

namespace oms {

template <std::size_t MaxOrders = 1'048'576, std::size_t MaxPriceLevels = 65536>
class MatchingEngine {
public:
    using Book = OrderBook<MaxOrders, MaxPriceLevels>;

    explicit MatchingEngine(Book& book) : book_(book) {}

    MatchResult submit_order(OrderId taker_id, Side side, Price price, Quantity qty,
                             TimestampNs ts = 0) noexcept {
        MatchResult result{};
        result.remaining_qty = qty;

        if (qty <= 0) return result;

        TimestampNs trade_ts = ts ? ts : NanoClock::now();

        while (result.remaining_qty > 0 && book_.would_cross(side, price)) {
            Side opposite = oms::opposite(side);
            const Order* maker = book_.front_at_best(opposite);
            if (!maker) break;

            if (result.trade_count >= MatchResult::MAX_TRADES) break;

            Quantity fill = std::min(result.remaining_qty, maker->qty);
            Price trade_price = maker->price;

            Trade& trade = result.trades[result.trade_count++];
            trade.maker_id = maker->id;
            trade.taker_id = taker_id;
            trade.price = trade_price;
            trade.qty = fill;
            trade.aggressor = side;
            trade.timestamp = trade_ts;

            book_.execute_order(maker->id, fill);
            result.remaining_qty -= fill;
            ++stats_.trades;
            stats_.volume += fill;
        }

        if (result.remaining_qty > 0) {
            result.resting_added =
                book_.add_order(taker_id, side, price, result.remaining_qty, trade_ts);
        }

        return result;
    }

    bool cancel_order(OrderId id) noexcept {
        return book_.cancel_order(id);
    }

    bool modify_order(OrderId id, Price new_price, Quantity new_qty) noexcept {
        const Order* order = book_.get_order(id);
        if (!order) return false;

        Side side = order->side;
        if (!book_.cancel_order(id)) return false;

        MatchResult matched = submit_order(id, side, new_price, new_qty);
        return matched.resting_added || matched.trade_count > 0;
    }

    Book& book() noexcept { return book_; }
    const Book& book() const noexcept { return book_; }

    struct Stats {
        uint64_t trades{0};
        uint64_t volume{0};
    };

    const Stats& stats() const noexcept { return stats_; }

private:
    Book& book_;
    Stats stats_{};
};

}  // namespace oms
