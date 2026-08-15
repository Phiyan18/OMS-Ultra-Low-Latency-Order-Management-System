#pragma once

#include "common/types.hpp"
#include "io/csv_reader.hpp"

#include <cstdlib>
#include <string>
#include <vector>

namespace oms {

// Binance spot trades CSV (data.binance.vision daily exports).
// Header: id,price,qty,quoteQty,time,isBuyerMaker

struct BinanceTrade {
    uint64_t trade_id{0};
    Price price{0};           // OMS fixed-point
    Quantity qty{0};
    TimestampNs timestamp_ns{0};
    bool buyer_is_maker{false};
};

inline Price binance_price_to_oms(const std::string& px) {
  // "42000.50" → 420005000
    double v = std::atof(px.c_str());
    return static_cast<Price>(v * 10000.0 + (v >= 0 ? 0.5 : -0.5));
}

inline bool parse_binance_trade_line(const std::string& line, BinanceTrade& out,
                                     bool& has_header) {
    if (line.rfind("id,", 0) == 0) {
        has_header = true;
        return false;
    }
    auto fields = split_csv_line(line);
    if (fields.size() < 6) return false;

    out.trade_id = static_cast<uint64_t>(std::strtoull(fields[0].c_str(), nullptr, 10));
    out.price = binance_price_to_oms(fields[1]);
    out.qty = static_cast<Quantity>(std::atoll(fields[2].c_str()));
    out.timestamp_ns = static_cast<TimestampNs>(std::atoll(fields[4].c_str())) * 1'000'000LL;
    out.buyer_is_maker = (fields[5] == "true" || fields[5] == "True" || fields[5] == "1");
    return true;
}

struct BinanceTradesFeed {
    std::vector<BinanceTrade> trades;
    std::string source_path;
    std::string symbol;

    bool load(const std::string& path, std::size_t max_trades = 0) {
        source_path = path;
        trades.clear();
        symbol = parse_symbol_from_path(path);

        std::vector<std::string> lines;
        if (!read_csv_lines(path, lines)) return false;

        bool header = false;
        for (const std::string& line : lines) {
            BinanceTrade t{};
            if (!parse_binance_trade_line(line, t, header)) continue;
            trades.push_back(t);
            if (max_trades > 0 && trades.size() >= max_trades) break;
        }
        return !trades.empty();
    }

    static std::string parse_symbol_from_path(const std::string& path) {
        std::size_t slash = path.find_last_of("/\\");
        std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
        std::size_t dash = name.find("-trades-");
        if (dash != std::string::npos) return name.substr(0, dash);
        return "CRYPTO";
    }
};

}  // namespace oms
