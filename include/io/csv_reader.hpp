#pragma once

#include <fstream>
#include <string>
#include <vector>

namespace oms {

// Minimal CSV line splitter (no quoted-field escape — sufficient for LOBSTER/Binance feeds).
inline std::vector<std::string> split_csv_line(const std::string& line) {
    std::vector<std::string> fields;
    std::string field;
    for (char c : line) {
        if (c == ',') {
            fields.push_back(field);
            field.clear();
        } else {
            field += c;
        }
    }
    fields.push_back(field);
    return fields;
}

inline bool read_csv_lines(const std::string& path, std::vector<std::string>& lines) {
    std::ifstream in(path);
    if (!in) return false;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(line);
    }
    return true;
}

}  // namespace oms
