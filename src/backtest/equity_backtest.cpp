#include "options/backtest/equity_backtest.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace options::backtest {
namespace {

PerformanceMetrics metrics(double initial, std::span<const double> equity, std::size_t orders) {
    PerformanceMetrics result;
    result.initial_equity = initial;
    result.final_equity = equity.back();
    result.total_return = result.final_equity / initial - 1.0;
    if (equity.size() > 1 && result.final_equity > 0) {
        result.annualized_return =
            std::pow(result.final_equity / initial, 252.0 / (equity.size() - 1)) - 1.0;
    }
    double peak = equity.front();
    for (const double value : equity) {
        peak = std::max(peak, value);
        if (peak > 0) result.max_drawdown = std::max(result.max_drawdown, 1.0 - value / peak);
    }
    result.orders = orders;
    return result;
}

}  // namespace

EquityComparison run_sma_comparison(std::span<const data::Bar> bars, double initial_cash,
                                    std::size_t short_window, std::size_t long_window) {
    if (initial_cash <= 0 || !std::isfinite(initial_cash))
        throw std::invalid_argument("initial cash must be positive");
    if (short_window == 0 || long_window == 0 || short_window >= long_window)
        throw std::invalid_argument("SMA windows must satisfy 0 < short < long");
    if (bars.size() <= long_window)
        throw std::invalid_argument("not enough bars: backtest requires more than the long window");

    double cash = initial_cash;
    std::int64_t shares = 0;
    std::size_t orders = 0;
    std::vector<double> strategy_equity;
    strategy_equity.reserve(bars.size());
    strategy_equity.push_back(initial_cash);

    std::vector<double> prefix(bars.size() + 1, 0.0);
    for (std::size_t i = 0; i < bars.size(); ++i) prefix[i + 1] = prefix[i] + bars[i].close;

    for (std::size_t i = 1; i < bars.size(); ++i) {
        if (i >= long_window) {
            const double short_sma = (prefix[i] - prefix[i - short_window]) / short_window;
            const double long_sma = (prefix[i] - prefix[i - long_window]) / long_window;
            const bool invested = short_sma > long_sma;
            if (invested && shares == 0) {
                shares = static_cast<std::int64_t>(std::floor(cash / bars[i].open));
                if (shares > 0) { cash -= shares * bars[i].open; ++orders; }
            } else if (!invested && shares > 0) {
                cash += shares * bars[i].open;
                shares = 0;
                ++orders;
            }
        }
        strategy_equity.push_back(cash + shares * bars[i].close);
    }

    const auto buy_hold_shares = static_cast<std::int64_t>(std::floor(initial_cash / bars.front().open));
    const double buy_hold_cash = initial_cash - buy_hold_shares * bars.front().open;
    std::vector<double> buy_hold_equity;
    buy_hold_equity.reserve(bars.size());
    for (const auto& bar : bars) buy_hold_equity.push_back(buy_hold_cash + buy_hold_shares * bar.close);

    EquityComparison result;
    result.symbol = bars.front().symbol;
    result.start_timestamp = bars.front().timestamp;
    result.end_timestamp = bars.back().timestamp;
    result.bars = bars.size();
    result.short_window = short_window;
    result.long_window = long_window;
    result.strategy = metrics(initial_cash, strategy_equity, orders);
    result.buy_and_hold = metrics(initial_cash, buy_hold_equity, buy_hold_shares > 0 ? 1 : 0);
    return result;
}

}  // namespace options::backtest
