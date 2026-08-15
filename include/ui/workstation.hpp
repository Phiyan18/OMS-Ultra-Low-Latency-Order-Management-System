#pragma once

#include <cstddef>
#include <string>

namespace oms::ui {

struct WorkstationConfig {
    std::string replay_path;
    std::string trades_path;
    std::string report_path{"oms_report.html"};
    std::size_t max_events{0};
};

// Starts the native desktop workstation. Returns when the user closes the window.
int run_workstation(const WorkstationConfig& config = {});

}  // namespace oms::ui
