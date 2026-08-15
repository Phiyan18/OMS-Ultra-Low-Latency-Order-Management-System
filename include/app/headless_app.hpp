#pragma once

#include "app/real_data_modes.hpp"
#include "app/session_services.hpp"
#include "ui/html_report.hpp"

#include <cstdio>
#include <string>

namespace oms::app {

inline int run_headless_menu(const std::string& default_report = "oms_report.html") {
    std::string report_path = default_report;
    std::string replay_path = default_lobster_sample_path();
    std::string trades_path = default_binance_trades_path();
    std::size_t max_events = 0;

    for (;;) {
        std::printf("\n=== OMS Headless Console ===\n"
                    " 1  Live simulation desk (text summary)\n"
                    " 2  Matching engine demo\n"
                    " 3  SPSC + WAL session\n"
                    " 4  Alpha decay (simulation)\n"
                    " 5  Quick benchmark note (run oms_benchmark)\n"
                    " 6  Full showcase + HTML report\n"
                    " 7  NASDAQ LOBSTER replay\n"
                    " 8  Binance trades analysis\n"
                    " 9  Data setup / file check\n"
                    " 0  Exit\n"
                    "Choice: ");
        std::fflush(stdout);

        int choice = -1;
        if (std::scanf("%d", &choice) != 1) {
            int ch = 0;
            while ((ch = std::getchar()) != '\n' && ch != EOF) {}
            std::printf("Invalid input.\n");
            continue;
        }

        switch (choice) {
            case 0:
                return 0;
            case 1: {
                const ui::SessionReport rep = run_simulation_session(5000);
                std::printf("Simulation complete: orders=%zu adds=%llu cancels=%llu executes=%llu mid=%.4f\n",
                            rep.order_count,
                            static_cast<unsigned long long>(rep.book_adds),
                            static_cast<unsigned long long>(rep.book_cancels),
                            static_cast<unsigned long long>(rep.book_executes),
                            static_cast<double>(rep.final_book.mid_price()) / 10000.0);
                break;
            }
            case 2: {
                const MatchDemoResult rep = run_matching_demo();
                std::printf("Matching demo: trades=%d bid=%ld ask=%ld\n",
                            rep.trade_count,
                            static_cast<long>(rep.final_book.best_bid),
                            static_cast<long>(rep.final_book.best_ask));
                break;
            }
            case 3: {
                const ui::SessionReport rep = run_spsc_wal_session("oms.wal");
                std::printf("SPSC+WAL: produced=%d trades=%d wal_cmds=%d wal_trades=%d orders=%zu\n",
                            rep.spsc_produced, rep.spsc_trades, rep.wal_commands, rep.wal_trades,
                            rep.order_count);
                break;
            }
            case 4: {
                const ui::SessionReport rep = run_simulation_session(5000);
                std::printf("Alpha decay (IC vs horizon):\n");
                for (const AlphaDecayPoint& pt : rep.decay_curve) {
                    std::printf("  %8.3f ms  IC=%+.4f\n",
                                static_cast<double>(pt.horizon_ns) / 1'000'000.0, pt.ic);
                }
                break;
            }
            case 5:
                std::printf("Run: oms_benchmark (or use the GUI Benchmark panel)\n");
                break;
            case 6: {
                const ui::SessionReport rep = run_full_showcase("oms.wal");
                if (ui::write_html_report(report_path, rep)) {
                    std::printf("Wrote %s\n", report_path.c_str());
                } else {
                    std::printf("Failed to write %s\n", report_path.c_str());
                }
                break;
            }
            case 7: {
                std::printf("Replay path [%s]: ", replay_path.c_str());
                std::fflush(stdout);
                char buf[512] = {};
                if (std::fgets(buf, sizeof(buf), stdin)) {
                    if (buf[0] != '\n') {
                        replay_path.assign(buf);
                        while (!replay_path.empty() &&
                               (replay_path.back() == '\n' || replay_path.back() == '\r')) {
                            replay_path.pop_back();
                        }
                    }
                }
                const ui::SessionReport rep = run_lobster_replay(replay_path, max_events);
                const std::string out = "oms_replay_report.html";
                if (ui::write_html_report(out, rep)) {
                    std::printf("Replay events=%zu ticker=%s -> %s\n",
                                rep.replay_events, rep.ticker.c_str(), out.c_str());
                } else {
                    std::printf("Replay finished but failed to write %s (%s)\n",
                                out.c_str(), rep.data_source.c_str());
                }
                break;
            }
            case 8: {
                std::printf("Trades path [%s]: ", trades_path.c_str());
                std::fflush(stdout);
                char buf[512] = {};
                if (std::fgets(buf, sizeof(buf), stdin)) {
                    if (buf[0] != '\n') {
                        trades_path.assign(buf);
                        while (!trades_path.empty() &&
                               (trades_path.back() == '\n' || trades_path.back() == '\r')) {
                            trades_path.pop_back();
                        }
                    }
                }
                const BinanceTapeResult rep = run_binance_trades(trades_path, max_events ? max_events : 100000);
                if (!rep.ok) {
                    std::printf("Binance analysis failed: %s\n", rep.error.c_str());
                } else {
                    std::printf("%s trades=%zu volume=%llu vpin=%.4f momentum=%.6f\n",
                                rep.symbol.c_str(), rep.trade_count,
                                static_cast<unsigned long long>(rep.total_volume),
                                rep.vpin, rep.momentum);
                }
                break;
            }
            case 9:
                std::printf("LOBSTER sample: %s %s\n", replay_path.c_str(),
                            file_exists(replay_path) ? "[found]" : "[missing]");
                std::printf("Binance trades: %s %s\n", trades_path.c_str(),
                            file_exists(trades_path) ? "[found]" : "[missing]");
                std::printf("Fetch scripts: scripts/fetch_market_data.ps1 / .sh\n");
                break;
            default:
                std::printf("Unknown option %d\n", choice);
                break;
        }
    }
}

inline int run_showcase_cli(const std::string& report_path) {
    std::printf("Running full showcase...\n");
    const ui::SessionReport rep = run_full_showcase("oms.wal");
    if (!ui::write_html_report(report_path, rep)) {
        std::fprintf(stderr, "Failed to write report: %s\n", report_path.c_str());
        return 1;
    }
    std::printf("Wrote %s\n", report_path.c_str());
    return 0;
}

}  // namespace oms::app
