#pragma once

#include "common/types.hpp"

namespace oms {

struct Trade {
    OrderId maker_id{0};
    OrderId taker_id{0};
    Price price{0};
    Quantity qty{0};
    Side aggressor{Side::Bid};
    TimestampNs timestamp{0};
};

struct MatchResult {
    static constexpr int MAX_TRADES = 64;

    Trade trades[MAX_TRADES]{};
    int trade_count{0};
    Quantity remaining_qty{0};
    bool resting_added{false};
};

}  // namespace oms
