#pragma once

#include <cstdint>
#include <limits>

namespace oms {

using OrderId = uint64_t;
using Price = int64_t;      // fixed-point: price * 1e4
using Quantity = int64_t;
using TimestampNs = int64_t;

constexpr Price INVALID_PRICE = std::numeric_limits<Price>::min();

enum class Side : uint8_t { Bid = 0, Ask = 1 };

inline Side opposite(Side s) {
    return s == Side::Bid ? Side::Ask : Side::Bid;
}

}  // namespace oms
