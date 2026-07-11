#pragma once

#include "options/data/bar.hpp"
#include "options/data/option_data.hpp"

#include <string>
#include <vector>

namespace options::providers::alpaca {

struct Credentials {
    std::string api_key;
    std::string api_secret;
};

struct BarsRequest {
    std::vector<std::string> symbols;
    std::string start;
    std::string end;
    std::string timeframe{"1Day"};
    std::string adjustment{"all"};
    std::string feed{"iex"};
};

struct OptionContractsRequest {
    std::vector<std::string> underlying_symbols;
    std::string expiration_date_gte;
    std::string expiration_date_lte;
    std::string status{"active"};
};

struct OptionChainRequest {
    std::string underlying_symbol;
    std::string feed{"indicative"};
    std::string expiration_date_gte;
    std::string expiration_date_lte;
    std::string type;
};

struct MarketClock {
    std::string timestamp;
    bool is_open{};
    std::string next_open;
    std::string next_close;
};

class AlpacaClient {
public:
    explicit AlpacaClient(Credentials credentials,
                          std::string base_url = "https://data.alpaca.markets",
                          std::string trading_base_url = "https://paper-api.alpaca.markets");
    [[nodiscard]] std::vector<data::Bar> fetch_bars(const BarsRequest& request) const;
    [[nodiscard]] std::vector<data::OptionContract> fetch_option_contracts(
        const OptionContractsRequest& request) const;
    [[nodiscard]] std::vector<data::Bar> fetch_option_bars(const BarsRequest& request) const;
    [[nodiscard]] std::vector<data::OptionQuote> fetch_latest_option_quotes(
        const std::vector<std::string>& symbols, const std::string& feed) const;
    [[nodiscard]] std::vector<data::OptionSnapshot> fetch_option_chain(
        const OptionChainRequest& request) const;
    [[nodiscard]] MarketClock fetch_market_clock() const;

private:
    Credentials credentials_;
    std::string base_url_;
    std::string trading_base_url_;
};

}  // namespace options::providers::alpaca
