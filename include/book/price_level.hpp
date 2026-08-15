#pragma once

#include "book/order.hpp"

namespace oms {

// Doubly-linked price level chain. Best bid = highest price, best ask = lowest price.
struct PriceLevel {
    Price price{0};
    Side side{Side::Bid};
    Quantity total_qty{0};
    int order_count{0};
    Order* head{nullptr};
    Order* tail{nullptr};
    PriceLevel* prev{nullptr};  // worse price
    PriceLevel* next{nullptr};  // better price

    void append(Order* order) noexcept {
        order->prev = tail;
        order->next = nullptr;
        if (tail) {
            tail->next = order;
        } else {
            head = order;
        }
        tail = order;
        order->level = this;
        total_qty += order->qty;
        ++order_count;
    }

    void remove(Order* order) noexcept {
        if (order->prev) {
            order->prev->next = order->next;
        } else {
            head = order->next;
        }
        if (order->next) {
            order->next->prev = order->prev;
        } else {
            tail = order->prev;
        }
        total_qty -= order->qty;
        --order_count;
        order->level = nullptr;
        order->prev = nullptr;
        order->next = nullptr;
    }

    void update_qty(Order* order, Quantity old_qty) noexcept {
        total_qty += order->qty - old_qty;
    }

    bool empty() const noexcept { return order_count == 0; }
};

}  // namespace oms
