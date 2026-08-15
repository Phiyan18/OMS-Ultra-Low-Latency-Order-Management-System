#pragma once

#include "book/trade.hpp"
#include "common/types.hpp"
#include "io/order_command.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace oms {

enum class WalRecordType : uint8_t {
    Add = 1,
    Modify = 2,
    Cancel = 3,
    Trade = 4,
    Checkpoint = 5,
};

#pragma pack(push, 8)
struct WalRecord {
    WalRecordType type{WalRecordType::Add};
    uint8_t _pad0[7]{};
    OrderId order_id{0};
    OrderId counterparty_id{0};
    Side side{Side::Bid};
    uint8_t _pad1[7]{};
    Price price{0};
    Quantity qty{0};
    TimestampNs timestamp{0};
    uint64_t reserved{0};

    static WalRecord from_command(const OrderCommand& cmd) noexcept {
        WalRecord rec{};
        rec.type = cmd.type == CommandType::Add     ? WalRecordType::Add
                 : cmd.type == CommandType::Modify  ? WalRecordType::Modify
                                                    : WalRecordType::Cancel;
        rec.order_id = cmd.order_id;
        rec.side = cmd.side;
        rec.price = cmd.price;
        rec.qty = cmd.qty;
        rec.timestamp = cmd.timestamp;
        return rec;
    }

    static WalRecord from_trade(const Trade& trade) noexcept {
        WalRecord rec{};
        rec.type = WalRecordType::Trade;
        rec.order_id = trade.taker_id;
        rec.counterparty_id = trade.maker_id;
        rec.side = trade.aggressor;
        rec.price = trade.price;
        rec.qty = trade.qty;
        rec.timestamp = trade.timestamp;
        return rec;
    }
};
#pragma pack(pop)

static_assert(sizeof(WalRecord) == 64, "WalRecord must be 64 bytes for cache-aligned I/O");

class WalWriter {
public:
    static constexpr std::size_t kBufferSize = 1 << 20;  // 1 MiB

    explicit WalWriter(const std::string& path) : path_(path) {
        file_ = std::fopen(path.c_str(), "wb");
        if (file_) {
            setvbuf(file_, buffer_, _IOFBF, kBufferSize);
        }
    }

    ~WalWriter() { close(); }

    WalWriter(const WalWriter&) = delete;
    WalWriter& operator=(const WalWriter&) = delete;

    bool open() const noexcept { return file_ != nullptr; }

    bool append(const WalRecord& record) {
        if (!file_) return false;
        if (std::fwrite(&record, sizeof(WalRecord), 1, file_) != 1) return false;
        ++record_count_;
        return true;
    }

    bool append(const OrderCommand& cmd) {
        return append(WalRecord::from_command(cmd));
    }

    bool append(const Trade& trade) {
        return append(WalRecord::from_trade(trade));
    }

    bool flush() {
        return file_ ? std::fflush(file_) == 0 : false;
    }

    void close() {
        if (file_) {
            std::fflush(file_);
            std::fclose(file_);
            file_ = nullptr;
        }
    }

    const std::string& path() const noexcept { return path_; }
    std::size_t record_count() const noexcept { return record_count_; }

private:
    std::string path_;
    std::FILE* file_{nullptr};
    char buffer_[kBufferSize]{};
    std::size_t record_count_{0};
};

class WalReader {
public:
    explicit WalReader(const std::string& path) : path_(path) {
        file_ = std::fopen(path.c_str(), "rb");
    }

    ~WalReader() {
        if (file_) std::fclose(file_);
    }

    bool open() const noexcept { return file_ != nullptr; }

    bool read_next(WalRecord& record) {
        if (!file_) return false;
        return std::fread(&record, sizeof(WalRecord), 1, file_) == 1;
    }

    std::vector<WalRecord> read_all() {
        std::vector<WalRecord> records;
        if (!file_) return records;
        WalRecord rec{};
        while (read_next(rec)) {
            records.push_back(rec);
        }
        return records;
    }

    const std::string& path() const noexcept { return path_; }

private:
    std::string path_;
    std::FILE* file_{nullptr};
};

}  // namespace oms
