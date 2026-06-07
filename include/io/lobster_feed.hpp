#pragma once

#include "common/types.hpp"
#include "io/csv_reader.hpp"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace oms {

// LOBSTER message file — NASDAQ TotalView-ITCH reconstructed limit-order events.
// Spec: https://lobsterdata.com/info/DataStructure.php
// Columns: Time, Type, OrderID, Size, Price, Direction (no header row).

enum class LobsterEventType : int {
    Submission = 1,
    PartialCancel = 2,
    Deletion = 3,
    VisibleExecution = 4,
    HiddenExecution = 5,
    CrossTrade = 6,
    TradingHalt = 7,
};

struct LobsterMessage {
    double time_sec{0.0};       // seconds after midnight (exchange local)
    LobsterEventType type{LobsterEventType::Submission};
    OrderId order_id{0};
    Quantity size{0};
    Price price{0};             // OMS fixed-point (price × 10⁴)
    int direction{0};           // -2 buy limit, -1 sell limit (LOBSTER convention)
    TimestampNs timestamp_ns{0};
};

// LOBSTER prices are USD × 1,000 (e.g. 118600 → $118.60). OMS uses USD × 10,000.
inline Price lobster_price_to_oms(int64_t lobster_px) noexcept {
    return static_cast<Price>(lobster_px * 10);
}

inline TimestampNs lobster_time_to_ns(double time_sec) noexcept {
    return static_cast<TimestampNs>(time_sec * 1'000'000'000.0);
}

inline Side lobster_direction_to_side(int direction) noexcept {
    // LOBSTER: -2 = buy limit, -1 = sell limit (+1 buy in some older files).
    if (direction == -2 || direction == 1) return Side::Bid;
    return Side::Ask;
}

// Aggressor side for VPIN from execution direction.
inline Side lobster_execution_aggressor(int direction) noexcept {
    // Execution direction: buy-initiated trade → Bid aggressor.
    if (direction == -2 || direction == 1) return Side::Bid;
    return Side::Ask;
}

inline bool parse_lobster_line(const std::string& line, LobsterMessage& out) {
    auto fields = split_csv_line(line);
    if (fields.size() < 6) return false;

    out.time_sec = std::atof(fields[0].c_str());
    out.type = static_cast<LobsterEventType>(std::atoi(fields[1].c_str()));
    out.order_id = static_cast<OrderId>(std::strtoull(fields[2].c_str(), nullptr, 10));
    out.size = static_cast<Quantity>(std::atoll(fields[3].c_str()));
    out.price = lobster_price_to_oms(std::atoll(fields[4].c_str()));
    out.direction = std::atoi(fields[5].c_str());
    out.timestamp_ns = lobster_time_to_ns(out.time_sec);
    return true;
}

struct LobsterFeed {
    std::vector<LobsterMessage> messages;
    std::string source_path;
    std::string ticker;  // parsed from filename when possible

    bool load(const std::string& path, std::size_t max_events = 0) {
        source_path = path;
        messages.clear();
        ticker = parse_ticker_from_path(path);

        std::vector<std::string> lines;
        if (!read_csv_lines(path, lines)) return false;

        messages.reserve(lines.size());
        for (const std::string& line : lines) {
            LobsterMessage msg{};
            if (!parse_lobster_line(line, msg)) continue;
            if (msg.type == LobsterEventType::TradingHalt) continue;
            messages.push_back(msg);
            if (max_events > 0 && messages.size() >= max_events) break;
        }
        return !messages.empty();
    }

    static std::string parse_ticker_from_path(const std::string& path) {
        std::size_t slash = path.find_last_of("/\\");
        std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
        std::size_t under = name.find('_');
        if (under != std::string::npos) return name.substr(0, under);
        return "UNKNOWN";
    }
};

}  // namespace oms
