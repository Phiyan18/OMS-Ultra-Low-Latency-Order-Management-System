#pragma once

#include "book/l2_book.hpp"
#include "common/types.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace oms::ui {

inline void enable_ansi_terminal() {
#ifdef _WIN32
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out == INVALID_HANDLE_VALUE) return;
    DWORD mode = 0;
    if (!GetConsoleMode(out, &mode)) return;
    SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#endif
}

inline void clear_screen() {
    std::printf("\033[2J\033[H");
    std::fflush(stdout);
}

inline double price_to_display(Price p) {
    return static_cast<double>(p) / 10000.0;
}

inline std::string format_price(Price p, int decimals = 2) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, price_to_display(p));
    return buf;
}

inline void print_banner() {
    std::printf(
        "\033[1;36m"
        "  ╔══════════════════════════════════════════════════════════╗\n"
        "  ║     OMS — Ultra-Low Latency Order Management System      ║\n"
        "  ║     L3 Book · Matching · Signals · WAL · Backtest        ║\n"
        "  ╚══════════════════════════════════════════════════════════╝\n"
        "\033[0m\n");
}

inline void print_bar(const char* label, double value, double max_abs,
                      int width = 28, char pos = '#', char neg = '-') {
    const int half = width / 2;
    std::printf("  %-10s [", label);
    if (max_abs < 1e-12) max_abs = 1.0;
    double norm = std::clamp(value / max_abs, -1.0, 1.0);
    int filled = static_cast<int>(std::abs(norm) * half);
    if (norm >= 0) {
        for (int i = 0; i < half; ++i) std::putchar(' ');
        for (int i = 0; i < filled; ++i) std::putchar(pos);
        for (int i = filled; i < half; ++i) std::putchar(' ');
    } else {
        for (int i = 0; i < half - filled; ++i) std::putchar(' ');
        for (int i = 0; i < filled; ++i) std::putchar(neg);
        for (int i = half - filled; i < half; ++i) std::putchar(' ');
    }
    std::printf("] %+.4f\n", value);
}

inline void print_sparkline(const std::vector<double>& series, int width = 48) {
    if (series.empty()) {
        std::printf("  (no data)\n");
        return;
    }
    double lo = *std::min_element(series.begin(), series.end());
    double hi = *std::max_element(series.begin(), series.end());
    if (hi - lo < 1e-12) hi = lo + 1.0;

    static const char* levels = " .:-=+*#%@";
    const int nlevels = static_cast<int>(std::strlen(levels)) - 1;

    std::printf("  ");
    std::size_t step = std::max<std::size_t>(1, series.size() / static_cast<std::size_t>(width));
    for (std::size_t i = 0; i < series.size(); i += step) {
        double t = (series[i] - lo) / (hi - lo);
        int idx = static_cast<int>(t * nlevels + 0.5);
        if (idx < 0) idx = 0;
        if (idx > nlevels) idx = nlevels;
        std::putchar(levels[idx]);
    }
    std::printf("  (lo=%.3f hi=%.3f)\n", lo, hi);
}

inline void render_l2_ladder(const L2Snapshot& snap, int depth = 8, int bar_width = 24) {
    Quantity max_qty = 1;
    for (int i = 0; i < snap.ask_count && i < depth; ++i)
        max_qty = std::max(max_qty, snap.asks[i].qty);
    for (int i = 0; i < snap.bid_count && i < depth; ++i)
        max_qty = std::max(max_qty, snap.bids[i].qty);

    std::printf("\033[1m  ── Order Book (L2) ─────────────────────────────────────\033[0m\n");

    for (int i = std::min(depth, snap.ask_count) - 1; i >= 0; --i) {
        const auto& lv = snap.asks[i];
        int filled = static_cast<int>((static_cast<double>(lv.qty) / max_qty) * bar_width);
        std::printf("  \033[31mASK\033[0m ");
        for (int b = 0; b < bar_width - filled; ++b) std::putchar(' ');
        for (int b = 0; b < filled; ++b) std::putchar('#');
        std::printf(" %8s  %6lld\n", format_price(lv.price).c_str(),
                    static_cast<long long>(lv.qty));
    }

    Price mid = snap.mid_price();
    if (mid != INVALID_PRICE) {
        std::printf("  \033[33m──────── mid %s  spread %s ────────\033[0m\n",
                    format_price(mid).c_str(),
                    format_price(snap.spread()).c_str());
    } else {
        std::printf("  \033[33m──────── (empty book) ─────────────────\033[0m\n");
    }

    for (int i = 0; i < std::min(depth, snap.bid_count); ++i) {
        const auto& lv = snap.bids[i];
        int filled = static_cast<int>((static_cast<double>(lv.qty) / max_qty) * bar_width);
        std::printf("  \033[32mBID\033[0m ");
        for (int b = 0; b < bar_width - filled; ++b) std::putchar(' ');
        for (int b = 0; b < filled; ++b) std::putchar('#');
        std::printf(" %8s  %6lld\n", format_price(lv.price).c_str(),
                    static_cast<long long>(lv.qty));
    }
}

inline void print_section(const char* title) {
    std::printf("\n\033[1;34m  ▶ %s\033[0m\n", title);
}

}  // namespace oms::ui
