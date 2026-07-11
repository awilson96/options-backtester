#pragma once

#include "options/data/bar.hpp"

#include <cstddef>
#include <span>
#include <string>

namespace options::backtest {

struct PerformanceMetrics {
    double initial_equity{};
    double final_equity{};
    double total_return{};
    double annualized_return{};
    double max_drawdown{};
    std::size_t orders{};
};

struct EquityComparison {
    std::string symbol;
    std::string start_timestamp;
    std::string end_timestamp;
    std::size_t bars{};
    std::size_t short_window{};
    std::size_t long_window{};
    PerformanceMetrics strategy;
    PerformanceMetrics buy_and_hold;
};

EquityComparison run_sma_comparison(std::span<const data::Bar> bars, double initial_cash,
                                    std::size_t short_window, std::size_t long_window);

}  // namespace options::backtest

