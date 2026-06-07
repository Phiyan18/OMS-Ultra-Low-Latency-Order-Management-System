#pragma once

#include "common/types.hpp"

namespace oms {

struct PriceLevel;

struct Order {
    OrderId id{0};
    Price price{0};
    Quantity qty{0};
    Side side{Side::Bid};
    PriceLevel* level{nullptr};
    Order* prev{nullptr};
    Order* next{nullptr};

    void reset() noexcept {
        id = 0;
        price = 0;
        qty = 0;
        side = Side::Bid;
        level = nullptr;
        prev = nullptr;
        next = nullptr;
    }
};

}  // namespace oms
