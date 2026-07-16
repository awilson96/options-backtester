#include "test.hpp"
#include "options/analysis/momentum.hpp"

#include <cmath>
#include <vector>

TEST_CASE("momentum analysis counts rolling wins losses and ties") {
    const std::vector<options::data::Bar> bars{
        {"TEST","2024-01-01T00:00:00Z",0,0,0,10},
        {"TEST","2024-01-02T00:00:00Z",0,0,0,12},
        {"TEST","2024-01-03T00:00:00Z",0,0,0,11},
        {"TEST","2024-01-04T00:00:00Z",0,0,0,11},
        {"TEST","2024-01-05T00:00:00Z",0,0,0,13},
    };
    const auto result=options::analysis::analyze_momentum(bars,1);
    REQUIRE(result.comparisons==4);
    REQUIRE(result.wins==2);
    REQUIRE(result.losses==1);
    REQUIRE(result.ties==1);
    REQUIRE(result.win_percentage==50.0);
}

TEST_CASE("momentum analysis uses the first trading day after the target date") {
    const std::vector<options::data::Bar> bars{
        {"TEST","2024-01-05T00:00:00Z",0,0,0,10},
        {"TEST","2024-01-08T00:00:00Z",0,0,0,12},
    };
    const auto result=options::analysis::analyze_momentum(bars,1);
    REQUIRE(result.comparisons==1);
    REQUIRE(result.wins==1);
}

TEST_CASE("momentum analysis averages every calendar phase of its skip window") {
    const std::vector<options::data::Bar> bars{
        {"TEST","2024-01-01T00:00:00Z",0,0,0,10},
        {"TEST","2024-01-02T00:00:00Z",0,0,0,11},
        {"TEST","2024-01-03T00:00:00Z",0,0,0,10},
        {"TEST","2024-01-04T00:00:00Z",0,0,0,10},
        {"TEST","2024-01-05T00:00:00Z",0,0,0,12},
        {"TEST","2024-01-06T00:00:00Z",0,0,0,11},
        {"TEST","2024-01-07T00:00:00Z",0,0,0,13},
    };
    const auto result=options::analysis::analyze_momentum(bars,1,3);
    REQUIRE(result.comparisons==2);
    REQUIRE(result.wins==1);
    REQUIRE(std::abs(result.losses-2.0/3.0)<1e-12);
    REQUIRE(std::abs(result.ties-1.0/3.0)<1e-12);
    REQUIRE(result.win_percentage==50);
}

TEST_CASE("momentum analysis compares against the configured strike grid") {
    const std::vector<options::data::Bar> bars{
        {"TEST","2024-01-01T00:00:00Z",0,0,0,12},
        {"TEST","2024-01-02T00:00:00Z",0,0,0,13},
    };
    const auto above=options::analysis::analyze_momentum(
        bars,1,1,options::analysis::StrikeAdjustment{2.5,2.5,1});
    const auto below=options::analysis::analyze_momentum(
        bars,1,1,options::analysis::StrikeAdjustment{2.5,2.5,-1});
    REQUIRE(above.losses==1);
    REQUIRE(above.wins==0);
    REQUIRE(below.wins==1);
    REQUIRE(below.losses==0);
}

TEST_CASE("momentum comparison price uses the second spread leg one strike width higher") {
    const std::vector<options::data::Bar> bars{
        {"TEST","2024-01-01T00:00:00Z",0,0,0,12},
        {"TEST","2024-01-02T00:00:00Z",0,0,0,13},
    };
    const auto result=options::analysis::analyze_momentum(
        bars,1,1,options::analysis::StrikeAdjustment{2.5,2.5,-1},
        options::analysis::SimulatedPricing{100,100,1000},0,0,true);
    REQUIRE(result.trades.size()==1);
    REQUIRE(result.trades.front().comparison_price==12.5);
    REQUIRE(result.trades.front().result==
        options::analysis::MomentumResult::TradeResult::itm);
}

TEST_CASE("momentum strike offsets move along the configured resolution grid") {
    const auto pricing=options::analysis::SimulatedPricing{100,100,1000};
    const std::vector<options::data::Bar> one_dollar_resolution{
        {"SPY","2024-01-01T00:00:00Z",0,0,0,756.56},
        {"SPY","2024-01-02T00:00:00Z",0,0,0,761},
    };
    const auto negative=options::analysis::analyze_momentum(
        one_dollar_resolution,1,1,options::analysis::StrikeAdjustment{5,1,-2},
        pricing,0,0,true);
    REQUIRE(negative.trades.size()==1);
    REQUIRE(negative.trades.front().comparison_price==760);

    const std::vector<options::data::Bar> two_fifty_resolution{
        {"TEST","2024-01-01T00:00:00Z",0,0,0,451.32},
        {"TEST","2024-01-02T00:00:00Z",0,0,0,461},
    };
    const auto positive=options::analysis::analyze_momentum(
        two_fifty_resolution,1,1,options::analysis::StrikeAdjustment{5,2.5,2},
        pricing,0,0,true);
    REQUIRE(positive.trades.size()==1);
    REQUIRE(positive.trades.front().comparison_price==460);
}

TEST_CASE("momentum simulated pricing produces profit and ending allocation percentage") {
    const std::vector<options::data::Bar> bars{
        {"TEST","2024-01-01T00:00:00Z",0,0,0,10},
        {"TEST","2024-01-02T00:00:00Z",0,0,0,11},
        {"TEST","2024-01-03T00:00:00Z",0,0,0,9},
    };
    const auto result=options::analysis::analyze_momentum(
        bars,1,1,std::nullopt,options::analysis::SimulatedPricing{200,300,1000});
    REQUIRE(result.total_profit==-100);
    REQUIRE(result.ending_balance==900);
    REQUIRE(std::abs(result.profit_percentage+10)<1e-12);
    REQUIRE(result.profit_curve.size()==3);
    REQUIRE(result.profit_curve.front().cumulative_profit==0);
    REQUIRE(result.profit_curve.back().cumulative_profit==-100);
}

TEST_CASE("momentum generates an executed trade ledger only when requested") {
    const std::vector<options::data::Bar> bars{
        {"TEST","2024-01-01T00:00:00Z",0,0,0,10},
        {"TEST","2024-01-02T00:00:00Z",0,0,0,11},
        {"TEST","2024-01-03T00:00:00Z",0,0,0,9},
    };
    const auto pricing=options::analysis::SimulatedPricing{200,300,1000};
    const auto normal=options::analysis::analyze_momentum(
        bars,1,1,std::nullopt,pricing);
    const auto with_ledger=options::analysis::analyze_momentum(
        bars,1,1,std::nullopt,pricing,0,0,true);
    REQUIRE(normal.trades.empty());
    REQUIRE(with_ledger.trades.size()==2);
    REQUIRE(with_ledger.trades.front().start_date=="2024-01-01T00:00:00.000Z");
    REQUIRE(with_ledger.trades.front().end_date=="2024-01-02T00:00:00.000Z");
    REQUIRE(with_ledger.trades.front().start_price==10);
    REQUIRE(with_ledger.trades.front().end_price==11);
    REQUIRE(with_ledger.trades.front().result==
        options::analysis::MomentumResult::TradeResult::itm);
    REQUIRE(with_ledger.trades.front().money_at_risk==300);
    REQUIRE(with_ledger.trades.back().result==
        options::analysis::MomentumResult::TradeResult::otm);
}

TEST_CASE("momentum summary mode preserves profits without constructing curves") {
    const std::vector<options::data::Bar> bars{
        {"TEST","2024-01-01T00:00:00Z",0,0,0,10},
        {"TEST","2024-01-02T00:00:00Z",0,0,0,11},
        {"TEST","2024-01-03T00:00:00Z",0,0,0,9},
        {"TEST","2024-01-04T00:00:00Z",0,0,0,12},
    };
    const auto pricing=options::analysis::SimulatedPricing{200,300,1000};
    const auto full=options::analysis::analyze_momentum_drop_scenarios(
        bars,1,1,std::nullopt,pricing,50);
    const auto summary=options::analysis::analyze_momentum_drop_scenarios(
        bars,1,1,std::nullopt,pricing,50,5,false,false);
    REQUIRE(summary.total_profit==full.total_profit);
    REQUIRE(summary.profit_percentage==full.profit_percentage);
    REQUIRE(summary.required_capital==full.required_capital);
    REQUIRE(summary.comparisons==full.comparisons);
    REQUIRE(summary.profit_curve.empty());
    REQUIRE(summary.high_profit_curve.empty());
    REQUIRE(summary.low_profit_curve.empty());
    REQUIRE(summary.no_drop_profit_curve.empty());
    REQUIRE(summary.trades.empty());
}

TEST_CASE("momentum simulated profit is averaged across skip phases") {
    const std::vector<options::data::Bar> bars{
        {"TEST","2024-01-01T00:00:00Z",0,0,0,10},
        {"TEST","2024-01-02T00:00:00Z",0,0,0,11},
        {"TEST","2024-01-03T00:00:00Z",0,0,0,9},
        {"TEST","2024-01-04T00:00:00Z",0,0,0,12},
    };
    const auto result=options::analysis::analyze_momentum(
        bars,1,2,std::nullopt,options::analysis::SimulatedPricing{100,100,1000});
    REQUIRE(result.comparisons==1.5);
    REQUIRE(result.wins==1);
    REQUIRE(result.losses==0.5);
    REQUIRE(result.total_profit==50);
    REQUIRE(result.ending_balance==1050);
    REQUIRE(std::abs(result.profit_percentage-5)<1e-12);
}

TEST_CASE("momentum simulated pricing applies contract slippage on selected sides") {
    const std::vector<options::data::Bar> bars{
        {"TEST","2024-01-01T00:00:00Z",0,0,0,10},
        {"TEST","2024-01-02T00:00:00Z",0,0,0,11},
        {"TEST","2024-01-03T00:00:00Z",0,0,0,9},
    };
    const auto one_side=options::analysis::analyze_momentum(bars,1,1,std::nullopt,
        options::analysis::SimulatedPricing{
            200,300,1000,0.04,0.07,options::analysis::SlippageMode::buy});
    const auto both_sides=options::analysis::analyze_momentum(bars,1,1,std::nullopt,
        options::analysis::SimulatedPricing{
            200,300,1000,0.04,0.07,options::analysis::SlippageMode::buy_and_sell});
    REQUIRE(one_side.total_profit==-108);
    REQUIRE(both_sides.total_profit==-122);
}

TEST_CASE("momentum reserves max loss and skips unfunded overlapping trades") {
    const std::vector<options::data::Bar> bars{
        {"TEST","2024-01-01T00:00:00Z",0,0,0,10},
        {"TEST","2024-01-02T00:00:00Z",0,0,0,10},
        {"TEST","2024-01-03T00:00:00Z",0,0,0,10},
        {"TEST","2024-01-04T00:00:00Z",0,0,0,11},
        {"TEST","2024-01-05T00:00:00Z",0,0,0,11},
    };
    const auto result=options::analysis::analyze_momentum(bars,3,1,std::nullopt,
        options::analysis::SimulatedPricing{200,300,300},0,0,true);
    REQUIRE(result.required_capital==600);
    REQUIRE(result.comparisons==1);
    REQUIRE(result.skipped_comparisons==1);
    REQUIRE(result.wins==1);
    REQUIRE(result.total_profit==200);
    REQUIRE(result.ending_balance==500);
    REQUIRE(result.trades.size()==1);
    REQUIRE(result.trades.front().money_at_risk==300);

    const auto fully_funded=options::analysis::analyze_momentum(bars,3,1,std::nullopt,
        options::analysis::SimulatedPricing{200,300,600},0,0,true);
    REQUIRE(fully_funded.trades.size()==2);
    REQUIRE(fully_funded.trades.front().money_at_risk==300);
    REQUIRE(fully_funded.trades.back().money_at_risk==600);
}

TEST_CASE("momentum required capital includes losses before later entries") {
    const std::vector<options::data::Bar> bars{
        {"TEST","2024-01-01T00:00:00Z",0,0,0,10},
        {"TEST","2024-01-02T00:00:00Z",0,0,0,9},
        {"TEST","2024-01-03T00:00:00Z",0,0,0,10},
    };
    const auto result=options::analysis::analyze_momentum(bars,1,1,std::nullopt,
        options::analysis::SimulatedPricing{200,300,600});
    REQUIRE(result.required_capital==600);
    REQUIRE(result.comparisons==2);
}

TEST_CASE("momentum uses VWAP as its bar analysis price") {
    const std::vector<options::data::Bar> bars{
        {"TEST","2024-01-01T00:00:00Z",0,0,0,10,0,0,10},
        {"TEST","2024-01-02T00:00:00Z",0,0,0,9,0,0,11},
    };
    const auto result=options::analysis::analyze_momentum(bars,1);
    REQUIRE(result.wins==1);
    REQUIRE(result.losses==0);
}

TEST_CASE("momentum preserves minute timestamps when locating the comparison bar") {
    const std::vector<options::data::Bar> bars{
        {"TEST","2024-01-01T14:31:00Z",0,0,0,10,0,0,10},
        {"TEST","2024-01-02T14:30:00Z",0,0,0,9,0,0,9},
        {"TEST","2024-01-02T14:31:00Z",0,0,0,11,0,0,11},
    };
    const auto result=options::analysis::analyze_momentum(bars,1);
    REQUIRE(result.wins==1);
    REQUIRE(result.losses==0);
}

TEST_CASE("momentum drop rate removes unavailable trades") {
    const std::vector<options::data::Bar> bars{
        {"TEST","2024-01-01T00:00:00Z",0,0,0,10},
        {"TEST","2024-01-02T00:00:00Z",0,0,0,11},
    };
    const auto result=options::analysis::analyze_momentum(
        bars,1,1,std::nullopt,std::nullopt,100.0);
    REQUIRE(result.comparisons==0);
    REQUIRE(result.dropped_comparisons==1);
}

TEST_CASE("momentum drop scenarios select a reproducible median and retain chart bounds") {
    const std::vector<options::data::Bar> bars{
        {"TEST","2024-01-01T00:00:00Z",0,0,0,10},
        {"TEST","2024-01-02T00:00:00Z",0,0,0,11},
        {"TEST","2024-01-03T00:00:00Z",0,0,0,9},
        {"TEST","2024-01-04T00:00:00Z",0,0,0,12},
    };
    const auto first=options::analysis::analyze_momentum_drop_scenarios(
        bars,1,1,std::nullopt,options::analysis::SimulatedPricing{100,100,1000},50);
    const auto second=options::analysis::analyze_momentum_drop_scenarios(
        bars,1,1,std::nullopt,options::analysis::SimulatedPricing{100,100,1000},50);
    REQUIRE(first.drop_scenario_count==5);
    REQUIRE(first.total_profit==second.total_profit);
    REQUIRE(!first.high_profit_curve.empty());
    REQUIRE(!first.low_profit_curve.empty());
    REQUIRE(!first.no_drop_profit_curve.empty());
    REQUIRE(first.high_profit_curve.back().cumulative_profit>=first.total_profit);
    REQUIRE(first.low_profit_curve.back().cumulative_profit<=first.total_profit);
}
