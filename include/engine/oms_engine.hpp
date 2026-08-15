#pragma once

#include "common/timestamp.hpp"
#include "engine/matching_engine.hpp"
#include "io/order_command.hpp"
#include "io/wal.hpp"
#include "queue/spsc_queue.hpp"

#include <string>

namespace oms {

template <std::size_t QueueCapacity = 65536,
          std::size_t MaxOrders = 1'048'576,
          std::size_t MaxPriceLevels = 65536>
class OmsEngine {
public:
    using Book = OrderBook<MaxOrders, MaxPriceLevels>;
    using Engine = MatchingEngine<MaxOrders, MaxPriceLevels>;
    using Queue = SPSCQueue<OrderCommand, QueueCapacity>;

    OmsEngine() : matcher_(book_) {}

    // Producer thread API — lock-free push
    bool submit(const OrderCommand& cmd) noexcept {
        return queue_.try_push(cmd);
    }

    bool submit_add(OrderId id, Side side, Price price, Quantity qty,
                    TimestampNs ts = 0) noexcept {
        OrderCommand cmd{};
        cmd.type = CommandType::Add;
        cmd.order_id = id;
        cmd.side = side;
        cmd.price = price;
        cmd.qty = qty;
        cmd.timestamp = ts ? ts : NanoClock::now();
        return queue_.try_push(cmd);
    }

    bool submit_cancel(OrderId id, TimestampNs ts = 0) noexcept {
        OrderCommand cmd{};
        cmd.type = CommandType::Cancel;
        cmd.order_id = id;
        cmd.timestamp = ts ? ts : NanoClock::now();
        return queue_.try_push(cmd);
    }

    bool submit_modify(OrderId id, Side side, Price price, Quantity qty,
                       TimestampNs ts = 0) noexcept {
        OrderCommand cmd{};
        cmd.type = CommandType::Modify;
        cmd.order_id = id;
        cmd.side = side;
        cmd.price = price;
        cmd.qty = qty;
        cmd.timestamp = ts ? ts : NanoClock::now();
        return queue_.try_push(cmd);
    }

    // Consumer thread API — drain SPSC and apply to book
    struct ProcessStats {
        int processed{0};
        int trades{0};
        int wal_writes{0};
    };

    ProcessStats process_all(WalWriter* wal = nullptr) {
        ProcessStats ps{};
        OrderCommand cmd{};
        while (queue_.try_pop(cmd)) {
            ++ps.processed;
            if (wal) {
                if (wal->append(cmd)) ++ps.wal_writes;
            }
            apply_command(cmd, wal, ps);
        }
        if (wal) wal->flush();
        return ps;
    }

    int process_one(WalWriter* wal = nullptr) {
        OrderCommand cmd{};
        if (!queue_.try_pop(cmd)) return 0;
        ProcessStats ps{};
        if (wal) wal->append(cmd);
        apply_command(cmd, wal, ps);
        return 1 + ps.trades;
    }

    Book& book() noexcept { return book_; }
    const Book& book() const noexcept { return book_; }
    Engine& matcher() noexcept { return matcher_; }
    const Engine& matcher() const noexcept { return matcher_; }
    Queue& queue() noexcept { return queue_; }
    const Queue& queue() const noexcept { return queue_; }

    std::size_t pending() const noexcept { return queue_.size(); }

private:
    void apply_command(const OrderCommand& cmd, WalWriter* wal, ProcessStats& ps) {
        switch (cmd.type) {
            case CommandType::Add: {
                MatchResult mr = matcher_.submit_order(
                    cmd.order_id, cmd.side, cmd.price, cmd.qty, cmd.timestamp);
                ps.trades += mr.trade_count;
                if (wal) {
                    for (int i = 0; i < mr.trade_count; ++i) {
                        wal->append(mr.trades[i]);
                        ++ps.wal_writes;
                    }
                }
                break;
            }
            case CommandType::Modify: {
                const Order* existing = book_.get_order(cmd.order_id);
                Side side = existing ? existing->side : cmd.side;
                if (matcher_.cancel_order(cmd.order_id)) {
                    MatchResult mr = matcher_.submit_order(
                        cmd.order_id, side, cmd.price, cmd.qty, cmd.timestamp);
                    ps.trades += mr.trade_count;
                    if (wal) {
                        for (int i = 0; i < mr.trade_count; ++i) {
                            wal->append(mr.trades[i]);
                            ++ps.wal_writes;
                        }
                    }
                }
                break;
            }
            case CommandType::Cancel:
                matcher_.cancel_order(cmd.order_id);
                break;
        }
    }

    Book book_;
    Engine matcher_;
    Queue queue_;
};

}  // namespace oms
