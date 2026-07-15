#include "test.hpp"
#include "options/analysis/momentum.hpp"

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
