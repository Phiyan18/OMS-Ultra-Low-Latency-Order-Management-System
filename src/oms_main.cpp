// OMS — native GUI trading workstation entry point.

#include "app/headless_app.hpp"
#include "ui/workstation.hpp"

#include <cstdio>
#include <cstring>
#include <string>

using oms::ui::WorkstationConfig;

namespace {

enum class LaunchMode { Gui, Headless, Showcase };

void print_usage(const char* prog) {
    std::fprintf(stderr,
                 "Usage: %s [options]\n"
                 "\n"
                 "  (no args)       Launch the trading workstation\n"
                 "  --headless      Terminal menu (alias: --cli)\n"
                 "  --showcase      Run automated demo and write HTML report\n"
                 "  --replay PATH   Pre-load LOBSTER NASDAQ message CSV replay\n"
                 "  --trades PATH   Pre-load Binance trades CSV analysis\n"
                 "  --report PATH   HTML report output path (default: oms_report.html)\n"
                 "  --max N         Max events/trades to process\n"
                 "  --help          Show this help\n",
                 prog);
}

}  // namespace

int main(int argc, char* argv[]) {
    WorkstationConfig config;
    LaunchMode mode = LaunchMode::Gui;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        if (std::strcmp(argv[i], "--headless") == 0 || std::strcmp(argv[i], "--cli") == 0) {
            mode = LaunchMode::Headless;
        } else if (std::strcmp(argv[i], "--showcase") == 0) {
            mode = LaunchMode::Showcase;
        } else if (std::strcmp(argv[i], "--report") == 0 && i + 1 < argc) {
            config.report_path = argv[++i];
        } else if (std::strcmp(argv[i], "--replay") == 0 && i + 1 < argc) {
            config.replay_path = argv[++i];
        } else if (std::strcmp(argv[i], "--trades") == 0 && i + 1 < argc) {
            config.trades_path = argv[++i];
        } else if (std::strcmp(argv[i], "--max") == 0 && i + 1 < argc) {
            config.max_events = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
        } else {
            std::fprintf(stderr, "Unknown option: %s (try --help)\n", argv[i]);
            return 1;
        }
    }

    switch (mode) {
        case LaunchMode::Headless:
            return oms::app::run_headless_menu(config.report_path);
        case LaunchMode::Showcase:
            return oms::app::run_showcase_cli(config.report_path);
        case LaunchMode::Gui:
        default:
            return oms::ui::run_workstation(config);
    }
}
