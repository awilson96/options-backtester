#include "test.hpp"
#include "options/data/sqlite_option_store.hpp"

#include <filesystem>

TEST_CASE("SQLite option datasets are idempotent") {
    const auto path = std::filesystem::temp_directory_path() / "options-backtester-options-test.db";
    std::filesystem::remove(path);
    {
        options::data::SqliteOptionStore store(path);
        options::data::OptionContract contract;
        contract.id="id"; contract.symbol="SPY260116C00600000"; contract.underlying_symbol="SPY";
        contract.expiration_date="2026-01-16"; contract.type="call"; contract.style="american";
        contract.status="active"; contract.strike_price=600; contract.multiplier=100; contract.tradable=true;
        store.upsert_contracts(std::span(&contract,1));
        store.upsert_contracts(std::span(&contract,1));
        REQUIRE(store.contract_count("SPY") == 1);

        options::data::Bar bar{"SPY260116C00600000","2025-07-09T04:00:00Z",10,11,9,10.5,100,20,10.4};
        store.upsert_bars(std::span(&bar,1),"indicative","1Day");
        store.upsert_bars(std::span(&bar,1),"indicative","1Day");
        REQUIRE(store.bar_count(contract.symbol) == 1);

        options::data::OptionQuote quote{contract.symbol,"2025-07-09T20:00:00Z",10,10.2,2,3,"C","P"};
        store.upsert_quotes(std::span(&quote,1),"indicative","2025-07-09T20:01:00Z");
        REQUIRE(store.quote_count(contract.symbol) == 1);

        options::data::OptionSnapshot snapshot;
        snapshot.symbol=contract.symbol; snapshot.observed_at="2025-07-09T20:01:00Z";
        snapshot.latest_quote=quote; snapshot.implied_volatility=0.2; snapshot.delta=0.5;
        store.upsert_snapshots(std::span(&snapshot,1),"indicative");
        REQUIRE(store.snapshot_count("SPY") == 1);
        store.insert_snapshot_observations(std::span(&snapshot,1),"indicative");
        snapshot.observed_at="2025-07-09T20:06:00Z";
        store.insert_snapshot_observations(std::span(&snapshot,1),"indicative");
        REQUIRE(store.snapshot_observation_count("SPY") == 2);
        REQUIRE(store.available_underlyings().size() == 1);
        REQUIRE(store.available_underlyings()[0] == "SPY");
        REQUIRE(store.available_snapshot_feeds("SPY")[0] == "indicative");
        const auto range=store.snapshot_date_range("SPY","indicative");
        REQUIRE(range.first == "2025-07-09");
        REQUIRE(range.second == "2025-07-09");
    }
    std::filesystem::remove(path);
    std::filesystem::remove(path.string()+"-shm");
    std::filesystem::remove(path.string()+"-wal");
}
