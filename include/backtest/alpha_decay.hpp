#pragma once

#include "common/types.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace oms {

struct AlphaDecayPoint {
    int64_t horizon_ns{0};
    double ic{0.0};  // information coefficient (Pearson corr)
    int sample_count{0};
};

// Pearson correlation between signal at t and forward return at t+horizon
class AlphaDecayAnalyzer {
public:
    struct Sample {
        double signal;
        Price mid_at_signal;
        TimestampNs timestamp;
    };

    void add_sample(double signal, Price mid, TimestampNs ts) {
        samples_.push_back({signal, mid, ts});
    }

    std::vector<AlphaDecayPoint> compute_decay(
        const std::vector<int64_t>& horizons_ns) const {
        std::vector<AlphaDecayPoint> results;
        results.reserve(horizons_ns.size());

        for (int64_t horizon : horizons_ns) {
            std::vector<double> signals;
            std::vector<double> fwd_returns;
            signals.reserve(samples_.size());
            fwd_returns.reserve(samples_.size());

            for (std::size_t i = 0; i < samples_.size(); ++i) {
                const auto& s = samples_[i];
                TimestampNs target = s.timestamp + horizon;

                // Find first sample at or after target
                for (std::size_t j = i + 1; j < samples_.size(); ++j) {
                    if (samples_[j].timestamp >= target) {
                        if (s.mid_at_signal != 0 && samples_[j].mid_at_signal != 0) {
                            double ret = static_cast<double>(samples_[j].mid_at_signal - s.mid_at_signal) /
                                         static_cast<double>(s.mid_at_signal);
                            signals.push_back(s.signal);
                            fwd_returns.push_back(ret);
                        }
                        break;
                    }
                }
            }

            AlphaDecayPoint pt{};
            pt.horizon_ns = horizon;
            pt.sample_count = static_cast<int>(signals.size());
            pt.ic = pearson_corr(signals, fwd_returns);
            results.push_back(pt);
        }
        return results;
    }

    void clear() { samples_.clear(); }
    std::size_t size() const { return samples_.size(); }

private:
    static double pearson_corr(const std::vector<double>& x,
                               const std::vector<double>& y) {
        if (x.size() != y.size() || x.size() < 2) return 0.0;
        std::size_t n = x.size();
        double sum_x = 0, sum_y = 0, sum_xy = 0, sum_x2 = 0, sum_y2 = 0;
        for (std::size_t i = 0; i < n; ++i) {
            sum_x += x[i];
            sum_y += y[i];
            sum_xy += x[i] * y[i];
            sum_x2 += x[i] * x[i];
            sum_y2 += y[i] * y[i];
        }
        double dn = static_cast<double>(n);
        double num = dn * sum_xy - sum_x * sum_y;
        double den = std::sqrt((dn * sum_x2 - sum_x * sum_x) * (dn * sum_y2 - sum_y * sum_y));
        if (den < 1e-15) return 0.0;
        return num / den;
    }

    std::vector<Sample> samples_;
};

}  // namespace oms
