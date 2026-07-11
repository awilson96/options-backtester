#include "options/data/bar_validation.hpp"

#include <cmath>
#include <unordered_set>

namespace options::data {

ValidationReport validate_bars(std::span<const Bar> bars) {
    ValidationReport report;
    report.bar_count = bars.size();
    if (bars.empty()) {
        report.errors.emplace_back("dataset contains no bars");
        return report;
    }
    report.first_timestamp = bars.front().timestamp;
    report.last_timestamp = bars.back().timestamp;
    std::unordered_set<std::string> timestamps;
    std::string previous;
    for (const auto& bar : bars) {
        const auto label = bar.symbol + " at " + bar.timestamp;
        if (bar.timestamp.empty()) report.errors.emplace_back(label + ": missing timestamp");
        if (!timestamps.insert(bar.timestamp).second)
            report.errors.emplace_back(label + ": duplicate timestamp");
        if (!previous.empty() && bar.timestamp <= previous)
            report.errors.emplace_back(label + ": timestamps are not strictly increasing");
        previous = bar.timestamp;

        if (!std::isfinite(bar.open) || !std::isfinite(bar.high) ||
            !std::isfinite(bar.low) || !std::isfinite(bar.close) ||
            !std::isfinite(bar.vwap)) {
            report.errors.emplace_back(label + ": non-finite price");
            continue;
        }
        if (bar.open <= 0 || bar.high <= 0 || bar.low <= 0 || bar.close <= 0)
            report.errors.emplace_back(label + ": OHLC prices must be positive");
        if (bar.low > bar.high) report.errors.emplace_back(label + ": low exceeds high");
        if (bar.high < bar.open || bar.high < bar.close)
            report.errors.emplace_back(label + ": high is below open or close");
        if (bar.low > bar.open || bar.low > bar.close)
            report.errors.emplace_back(label + ": low is above open or close");
        if (bar.volume < 0 || bar.trade_count < 0)
            report.errors.emplace_back(label + ": negative volume or trade count");
        if (bar.volume == 0) report.warnings.emplace_back(label + ": zero volume");
    }
    return report;
}

}  // namespace options::data

