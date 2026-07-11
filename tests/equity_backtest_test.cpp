#include "test.hpp"
#include "options/backtest/equity_backtest.hpp"

#include <vector>

TEST_CASE("SMA backtest executes using the next bar open") {
    std::vector<options::data::Bar> bars;
    for (int day = 1; day <= 8; ++day) {
        const double price = day <= 4 ? static_cast<double>(day) : 5.0;
        bars.push_back({"TEST", "2024-01-0" + std::to_string(day) + "T00:00:00Z",
                        price, price, price, price, 100, 10, price});
    }
    const auto result = options::backtest::run_sma_comparison(bars, 100.0, 2, 3);
    REQUIRE(result.strategy.orders == 2);
    REQUIRE(result.strategy.final_equity == 125.0);
    REQUIRE(result.buy_and_hold.final_equity == 500.0);
}

TEST_CASE("SMA backtest rejects insufficient history") {
    const options::data::Bar bars[]{{"TEST", "2024-01-01T00:00:00Z", 1, 1, 1, 1, 1, 1, 1}};
    bool threw = false;
    try { (void)options::backtest::run_sma_comparison(bars, 100.0, 2, 3); }
    catch (...) { threw = true; }
    REQUIRE(threw);
}
