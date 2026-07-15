#pragma once

#include "options/data/bar.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace options::analysis {

struct MomentumResult {
    double comparisons{};
    double wins{};
    double losses{};
    double ties{};
    double win_percentage{};
    double total_profit{};
    double profit_percentage{};
    double allocation{};
    double ending_balance{};
    struct ProfitPoint {
        std::string date;
        double cumulative_profit{};
    };
    std::vector<ProfitPoint> profit_curve;
};

struct StrikeAdjustment {
    double width{};
    int offset{};
};

struct SimulatedPricing {
    double max_profit{};
    double max_loss{};
    double allocation{};
};

[[nodiscard]] MomentumResult analyze_momentum(
    std::span<const data::Bar> bars, std::size_t window_days, std::size_t skip_days=1,
    std::optional<StrikeAdjustment> strike_adjustment=std::nullopt,
    std::optional<SimulatedPricing> simulated_pricing=std::nullopt);

}  // namespace options::analysis
