#pragma once

#include "common/types.hpp"

namespace oms {

enum class CommandType : uint8_t {
    Add = 1,
    Modify = 2,
    Cancel = 3,
};

// Fixed-size command for SPSC ingress — no heap allocation.
struct OrderCommand {
    CommandType type{CommandType::Add};
    OrderId order_id{0};
    Side side{Side::Bid};
    Price price{0};
    Quantity qty{0};
    TimestampNs timestamp{0};
};

}  // namespace oms
