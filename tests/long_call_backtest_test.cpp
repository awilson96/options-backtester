#include "test.hpp"
#include "options/backtest/long_call_backtest.hpp"

#include <vector>

namespace {
options::data::OptionObservation observation(const std::string& date, double bid, double ask) {
    options::data::OptionObservation value;
    value.underlying_symbol="SPY"; value.expiration_date="2026-02-15";
    value.type="call"; value.strike_price=600; value.multiplier=100; value.open_interest=1000;
    value.snapshot.symbol="SPY260215C00600000"; value.snapshot.observed_at=date+"T21:00:00Z";
    value.observation_date=date;
    value.snapshot.delta=0.5;
    value.snapshot.latest_quote=options::data::OptionQuote{value.snapshot.symbol,date+"T20:59:00Z",bid,ask,10,10,"",""};
    return value;
}
}

TEST_CASE("long call enters at ask and marks the position at bid") {
    const std::vector<options::data::OptionObservation> observations{
        observation("2026-01-05",1.8,2.0), observation("2026-01-06",2.8,3.0)};
    const options::data::Bar bars[]{
        {"SPY","2026-01-05T05:00:00Z",500,501,499,500,100,10,500},
        {"SPY","2026-01-06T05:00:00Z",500,501,499,501,100,10,500}};
    options::backtest::LongCallConfig config; config.initial_cash=1000; config.hold_sessions=10;
    const auto result=options::backtest::run_long_call_strategy(observations,bars,config);
    REQUIRE(result.strategy_equity[0].value==980.0);
    REQUIRE(result.strategy.final_equity==1080.0);
    REQUIRE(result.strategy.orders==1);
}

TEST_CASE("long call rejects crossed markets") {
    const std::vector<options::data::OptionObservation> observations{
        observation("2026-01-05",2.1,2.0), observation("2026-01-06",2.1,2.0)};
    const options::data::Bar bars[]{
        {"SPY","2026-01-05T05:00:00Z",500,501,499,500,100,10,500},
        {"SPY","2026-01-06T05:00:00Z",500,501,499,501,100,10,500}};
    options::backtest::LongCallConfig config; config.initial_cash=1000;
    const auto result=options::backtest::run_long_call_strategy(observations,bars,config);
    REQUIRE(result.strategy.final_equity==1000.0);
    REQUIRE(result.rejected_markets==2);
}
