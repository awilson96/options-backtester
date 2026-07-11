#pragma once

#include "options/data/option_data.hpp"
#include "options/providers/alpaca/alpaca_json.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace options::providers::alpaca {

struct ContractsPage {
    std::vector<data::OptionContract> contracts;
    std::string next_page_token;
};

struct QuotesPage {
    std::vector<data::OptionQuote> quotes;
    std::string next_page_token;
};

struct SnapshotsPage {
    std::vector<data::OptionSnapshot> snapshots;
    std::string next_page_token;
};

ContractsPage parse_option_contracts_page(std::string_view json);
QuotesPage parse_option_quotes_page(std::string_view json);
SnapshotsPage parse_option_snapshots_page(std::string_view json, const std::string& observed_at);

}  // namespace options::providers::alpaca

