#pragma once

#include "options/data/bar.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace options::analysis {

struct MomentumResult {
    double comparisons{};
    double skipped_comparisons{};
    double dropped_comparisons{};
    double wins{};
    double losses{};
    double ties{};
    double win_percentage{};
    double total_profit{};
    double profit_percentage{};
    double allocation{};
    double ending_balance{};
    double required_capital{};
    struct ProfitPoint {
        std::string date;
        double cumulative_profit{};
    };
    std::vector<ProfitPoint> profit_curve;
    std::vector<ProfitPoint> high_profit_curve;
    std::vector<ProfitPoint> low_profit_curve;
    std::vector<ProfitPoint> no_drop_profit_curve;
    std::size_t drop_scenario_count{};
};

struct StrikeAdjustment {
    double width{};
    int offset{};
};

enum class SlippageMode {
    none,
    buy,
    sell,
    buy_and_sell,
};

struct SimulatedPricing {
    double max_profit{};
    double max_loss{};
    double allocation{};
    double buy_slippage_per_share{};
    double sell_slippage_per_share{};
    SlippageMode slippage_mode{SlippageMode::none};
};

[[nodiscard]] MomentumResult analyze_momentum(
    std::span<const data::Bar> bars, std::size_t window_days, std::size_t skip_days=1,
    std::optional<StrikeAdjustment> strike_adjustment=std::nullopt,
    std::optional<SimulatedPricing> simulated_pricing=std::nullopt,
    double drop_rate_percent=0.0, std::uint64_t drop_seed=0);

[[nodiscard]] MomentumResult analyze_momentum_drop_scenarios(
    std::span<const data::Bar> bars, std::size_t window_days, std::size_t skip_days,
    std::optional<StrikeAdjustment> strike_adjustment,
    std::optional<SimulatedPricing> simulated_pricing,
    double drop_rate_percent, std::size_t scenario_count=5);

}  // namespace options::analysis
