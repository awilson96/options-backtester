#pragma once

#include "options/data/bar.hpp"

#include <cstddef>
#include <span>

namespace options::analysis {

struct MomentumResult {
    std::size_t comparisons{};
    std::size_t wins{};
    std::size_t losses{};
    std::size_t ties{};
    double win_percentage{};
};

[[nodiscard]] MomentumResult analyze_momentum(
    std::span<const data::Bar> bars, std::size_t window_months);

}  // namespace options::analysis
