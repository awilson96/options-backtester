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
        {"TEST","2024-01-02T00:00:00Z",0,0,0,12.25},
    };
    const auto above=options::analysis::analyze_momentum(
        bars,1,1,options::analysis::StrikeAdjustment{2.5,1});
    const auto below=options::analysis::analyze_momentum(
        bars,1,1,options::analysis::StrikeAdjustment{2.5,-1});
    REQUIRE(above.losses==1);
    REQUIRE(above.wins==0);
    REQUIRE(below.wins==1);
    REQUIRE(below.losses==0);
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
    REQUIRE(result.profit_curve.size()==2);
    REQUIRE(result.profit_curve.back().cumulative_profit==-100);
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
