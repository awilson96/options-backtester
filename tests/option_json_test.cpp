#include "test.hpp"
#include "options/providers/alpaca/option_json.hpp"

TEST_CASE("parses option contracts with dated open interest") {
    const auto page = options::providers::alpaca::parse_option_contracts_page(R"json({
      "option_contracts":[{
        "id":"id-1","symbol":"SPY260116C00600000","name":"SPY Jan 16 2026 600 Call",
        "underlying_symbol":"SPY","root_symbol":"SPY","expiration_date":"2026-01-16",
        "type":"call","style":"american","status":"active","strike_price":"600",
        "size":"100","tradable":true,"open_interest":"1234",
        "open_interest_date":"2025-07-09","close_price":"12.34",
        "close_price_date":"2025-07-09"
      }],"next_page_token":"next"
    })json");
    REQUIRE(page.contracts.size() == 1);
    REQUIRE(page.contracts[0].symbol == "SPY260116C00600000");
    REQUIRE(page.contracts[0].strike_price == 600.0);
    REQUIRE(page.contracts[0].open_interest.value() == 1234);
    REQUIRE(page.next_page_token == "next");
}

TEST_CASE("parses latest option quotes") {
    const auto page = options::providers::alpaca::parse_option_quotes_page(R"json({
      "quotes":{"SPY260116C00600000":{"t":"2026-07-09T19:59:59Z","bp":10.1,
       "ap":10.3,"bs":20,"as":15,"bx":"C","ax":"P"}}
    })json");
    REQUIRE(page.quotes.size() == 1);
    REQUIRE(page.quotes[0].bid_price == 10.1);
    REQUIRE(page.quotes[0].ask_size == 15);
}

TEST_CASE("parses option snapshot IV and greeks") {
    const auto page = options::providers::alpaca::parse_option_snapshots_page(R"json({
      "snapshots":{"SPY260116C00600000":{
        "latestQuote":{"t":"2026-07-09T19:59:59Z","bp":10.1,"ap":10.3,"bs":20,"as":15},
        "latestTrade":{"t":"2026-07-09T19:58:00Z","p":10.2,"s":2},
        "impliedVolatility":0.22,
        "greeks":{"delta":0.51,"gamma":0.02,"theta":-0.04,"vega":0.12,"rho":0.03}
      }}
    })json", "2026-07-09T20:01:00Z");
    REQUIRE(page.snapshots.size() == 1);
    REQUIRE(page.snapshots[0].implied_volatility.value() == 0.22);
    REQUIRE(page.snapshots[0].delta.value() == 0.51);
    REQUIRE(page.snapshots[0].latest_quote->ask_price == 10.3);
}

