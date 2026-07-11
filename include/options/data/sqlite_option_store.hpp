#pragma once

#include "options/data/bar.hpp"
#include "options/data/option_data.hpp"

#include <filesystem>
#include <span>
#include <string>
#include <utility>
#include <vector>

struct sqlite3;

namespace options::data {

class SqliteOptionStore {
public:
    explicit SqliteOptionStore(const std::filesystem::path& path);
    ~SqliteOptionStore();
    SqliteOptionStore(const SqliteOptionStore&) = delete;
    SqliteOptionStore& operator=(const SqliteOptionStore&) = delete;

    void upsert_contracts(std::span<const OptionContract> contracts);
    void upsert_bars(std::span<const Bar> bars, const std::string& feed,
                     const std::string& timeframe);
    void upsert_quotes(std::span<const OptionQuote> quotes, const std::string& feed,
                       const std::string& observed_at);
    void upsert_snapshots(std::span<const OptionSnapshot> snapshots, const std::string& feed);
    void insert_snapshot_observations(std::span<const OptionSnapshot> snapshots,
                                      const std::string& feed);

    [[nodiscard]] std::int64_t contract_count(const std::string& underlying) const;
    [[nodiscard]] std::int64_t bar_count(const std::string& contract_symbol) const;
    [[nodiscard]] std::int64_t quote_count(const std::string& contract_symbol) const;
    [[nodiscard]] std::int64_t snapshot_count(const std::string& underlying) const;
    [[nodiscard]] std::int64_t snapshot_observation_count(const std::string& underlying) const;
    [[nodiscard]] std::vector<OptionContract> load_contracts(const std::string& underlying,
                                                              std::int64_t limit = 50) const;
    [[nodiscard]] std::vector<OptionObservation> load_snapshot_history(
        const std::string& underlying, const std::string& feed, const std::string& start,
        const std::string& end) const;
    [[nodiscard]] std::vector<std::string> available_underlyings() const;
    [[nodiscard]] std::vector<std::string> available_snapshot_feeds(
        const std::string& underlying) const;
    [[nodiscard]] std::pair<std::string,std::string> snapshot_date_range(
        const std::string& underlying, const std::string& feed) const;

private:
    sqlite3* db_{};
    void migrate();
};

}  // namespace options::data
