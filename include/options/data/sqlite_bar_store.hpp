#pragma once

#include "options/data/bar.hpp"

#include <filesystem>
#include <span>
#include <string>
#include <vector>

struct sqlite3;

namespace options::data {

struct DateRange {
    std::string start;
    std::string end;
};

struct BarQuery {
    std::string symbol;
    std::string provider{"alpaca"};
    std::string feed{"iex"};
    std::string timeframe{stock_bar_timeframe};
    std::string adjustment{"all"};
    std::string start;
    std::string end;
};

class SqliteBarStore {
public:
    explicit SqliteBarStore(const std::filesystem::path& path);
    ~SqliteBarStore();
    SqliteBarStore(const SqliteBarStore&) = delete;
    SqliteBarStore& operator=(const SqliteBarStore&) = delete;

    void upsert(std::span<const Bar> bars, const std::string& provider, const std::string& feed,
                const std::string& timeframe, const std::string& adjustment);
    [[nodiscard]] std::vector<Bar> load(const std::string& symbol) const;
    [[nodiscard]] std::vector<Bar> load(const BarQuery& query) const;
    [[nodiscard]] std::int64_t count(const std::string& symbol) const;
    [[nodiscard]] DateRange date_range(const BarQuery& query) const;
    [[nodiscard]] std::vector<std::string> available_symbols(
        const std::string& provider, const std::string& feed,
        const std::string& timeframe, const std::string& adjustment) const;
    [[nodiscard]] std::vector<DateRange> missing_coverage(
        const std::string& symbol, const std::string& provider, const std::string& feed,
        const std::string& timeframe, const std::string& adjustment,
        const DateRange& requested) const;
    void record_coverage(const std::string& symbol, const std::string& provider,
                         const std::string& feed, const std::string& timeframe,
                         const std::string& adjustment, const DateRange& covered);

private:
    sqlite3* db_{};
    void migrate();
};

}  // namespace options::data
