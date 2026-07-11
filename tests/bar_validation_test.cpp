#include "test.hpp"
#include "options/data/bar_validation.hpp"

TEST_CASE("valid OHLC bars pass validation") {
    const options::data::Bar bars[]{
        {"SPY", "2024-01-02T05:00:00Z", 470, 472, 469, 471, 100, 10, 470.5},
        {"SPY", "2024-01-03T05:00:00Z", 471, 473, 470, 472, 110, 11, 471.5}};
    const auto report = options::data::validate_bars(bars);
    REQUIRE(report.valid());
    REQUIRE(report.bar_count == 2);
}

TEST_CASE("invalid OHLC and duplicate timestamps fail validation") {
    const options::data::Bar bars[]{
        {"SPY", "2024-01-02T05:00:00Z", 470, 469, 471, 470, 100, 10, 470.5},
        {"SPY", "2024-01-02T05:00:00Z", 471, 473, 470, 472, 110, 11, 471.5}};
    const auto report = options::data::validate_bars(bars);
    REQUIRE(!report.valid());
    REQUIRE(report.errors.size() >= 3);
}

