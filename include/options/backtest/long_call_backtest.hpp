#pragma once

#include "options/backtest/equity_backtest.hpp"
#include "options/data/option_data.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace options::backtest {

struct EquityPoint {
    std::string date;
    double value{};
};

struct OptionTrade {
    std::string symbol;
    std::string entry_date;
    std::string exit_date;
    double entry_price{};
    double exit_price{};
    std::int64_t multiplier{};
    double pnl{};
};

struct LongCallConfig {
    double initial_cash{10000.0};
    double target_delta{0.50};
    int minimum_dte{30};
    int maximum_dte{45};
    std::size_t hold_sessions{5};
    double maximum_spread_fraction{0.25};
    std::int64_t minimum_open_interest{0};
};

struct LongCallResult {
    std::string underlying;
    LongCallConfig config;
    PerformanceMetrics strategy;
    PerformanceMetrics buy_and_hold;
    std::vector<EquityPoint> strategy_equity;
    std::vector<EquityPoint> buy_hold_equity;
    std::vector<OptionTrade> trades;
    std::size_t rejected_markets{};
};

LongCallResult run_long_call_strategy(std::span<const data::OptionObservation> observations,
                                      std::span<const data::Bar> underlying_bars,
                                      const LongCallConfig& config);

}  // namespace options::backtest

