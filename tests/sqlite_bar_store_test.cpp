#include "test.hpp"
#include "options/data/sqlite_bar_store.hpp"

#include <filesystem>

TEST_CASE("SQLite bar writes are idempotent") {
    const auto path = std::filesystem::temp_directory_path() / "options-backtester-test.db";
    std::filesystem::remove(path);
    {
        options::data::SqliteBarStore store(path);
        options::data::Bar bar{"SPY", "2024-01-03T05:00:00Z", 470.43, 471.19, 468.17,
                               468.79, 103585900, 519999, 469.51};
        store.upsert(std::span(&bar, 1), "alpaca", "iex", "1Day", "all");
        bar.close = 469.0;
        store.upsert(std::span(&bar, 1), "alpaca", "iex", "1Day", "all");
        REQUIRE(store.count("SPY") == 1);
        const auto loaded = store.load("SPY");
        REQUIRE(loaded.size() == 1);
        REQUIRE(loaded[0].close == 469.0);
        const auto range=store.date_range({"SPY","alpaca","iex","1Day","all","",""});
        REQUIRE(range.start == "2024-01-03");
        REQUIRE(range.end == "2024-01-03");
        const auto symbols=store.available_symbols("alpaca","iex","1Day","all");
        REQUIRE(symbols.size() == 1);
        REQUIRE(symbols[0] == "SPY");
    }
    std::filesystem::remove(path);
    std::filesystem::remove(path.string() + "-shm");
    std::filesystem::remove(path.string() + "-wal");
}

TEST_CASE("SQLite coverage returns only missing date ranges") {
    const auto path = std::filesystem::temp_directory_path() / "options-backtester-coverage-test.db";
    std::filesystem::remove(path);
    {
        options::data::SqliteBarStore store(path);
        const auto missing = [&store] {
            return store.missing_coverage("SPY", "alpaca", "iex", "1Day", "all",
                                          {"2024-01-01", "2024-01-31"});
        };
        REQUIRE(missing().size() == 1);
        store.record_coverage("SPY", "alpaca", "iex", "1Day", "all",
                              {"2024-01-05", "2024-01-10"});
        store.record_coverage("SPY", "alpaca", "iex", "1Day", "all",
                              {"2024-01-15", "2024-01-20"});
        const auto partial = missing();
        REQUIRE(partial.size() == 3);
        REQUIRE(partial[0].start == "2024-01-01");
        REQUIRE(partial[0].end == "2024-01-04");
        REQUIRE(partial[1].start == "2024-01-11");
        REQUIRE(partial[1].end == "2024-01-14");
        REQUIRE(partial[2].start == "2024-01-21");
        REQUIRE(partial[2].end == "2024-01-31");
        store.record_coverage("SPY", "alpaca", "iex", "1Day", "all",
                              {"2024-01-01", "2024-01-31"});
        REQUIRE(missing().empty());
    }
    std::filesystem::remove(path);
    std::filesystem::remove(path.string() + "-shm");
    std::filesystem::remove(path.string() + "-wal");
}
