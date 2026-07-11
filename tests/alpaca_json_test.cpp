#include "test.hpp"
#include "options/providers/alpaca/alpaca_json.hpp"

TEST_CASE("parses a multi-symbol Alpaca bars page") {
    const auto page = options::providers::alpaca::parse_bars_page(R"json({
      "bars": {
        "AAPL": [{"c":187.15,"h":188.44,"l":183.89,"n":675432,"o":185.14,
                  "t":"2024-01-03T05:00:00Z","v":58414500,"vw":186.4}],
        "SPY": [{"c":468.79,"h":471.19,"l":468.17,"n":519999,"o":470.43,
                 "t":"2024-01-03T05:00:00Z","v":103585900,"vw":469.51}]
      },
      "next_page_token": "next-token"
    })json");
    REQUIRE(page.bars.size() == 2);
    REQUIRE(page.bars[0].symbol == "AAPL");
    REQUIRE(page.bars[0].timestamp == "2024-01-03T05:00:00Z");
    REQUIRE(page.bars[0].volume == 58414500);
    REQUIRE(page.next_page_token == "next-token");
}

TEST_CASE("rejects malformed Alpaca JSON") {
    bool threw = false;
    try { (void)options::providers::alpaca::parse_bars_page("not json"); }
    catch (...) { threw = true; }
    REQUIRE(threw);
}

