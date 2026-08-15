#pragma once

#include "engine/matching_engine.hpp"
#include "io/wal.hpp"

namespace oms {

struct ReplayStats {
    std::size_t commands{0};
    std::size_t trades{0};
    std::size_t errors{0};
};

template <std::size_t MaxOrders = 1'048'576, std::size_t MaxPriceLevels = 65536>
class WalReplayer {
public:
    using Engine = MatchingEngine<MaxOrders, MaxPriceLevels>;

    static ReplayStats replay(const std::string& path, Engine& engine) {
        WalReader reader(path);
        ReplayStats stats{};
        if (!reader.open()) {
            ++stats.errors;
            return stats;
        }

        WalRecord rec{};
        while (reader.read_next(rec)) {
            switch (rec.type) {
                case WalRecordType::Add: {
                    MatchResult mr = engine.submit_order(
                        rec.order_id, rec.side, rec.price, rec.qty, rec.timestamp);
                    stats.trades += static_cast<std::size_t>(mr.trade_count);
                    ++stats.commands;
                    break;
                }
                case WalRecordType::Modify:
                    if (engine.modify_order(rec.order_id, rec.price, rec.qty)) {
                        ++stats.commands;
                    } else {
                        ++stats.errors;
                    }
                    break;
                case WalRecordType::Cancel:
                    if (engine.cancel_order(rec.order_id)) {
                        ++stats.commands;
                    } else {
                        ++stats.errors;
                    }
                    break;
                case WalRecordType::Trade:
                    // Trades are derived from Add/Modify matching — skip on replay
                    ++stats.trades;
                    break;
                case WalRecordType::Checkpoint:
                    break;
            }
        }
        return stats;
    }
};

}  // namespace oms
