#pragma once

#include "options/data/bar.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace options::data {

struct OptionContract {
    std::string id;
    std::string symbol;
    std::string name;
    std::string underlying_symbol;
    std::string root_symbol;
    std::string expiration_date;
    std::string type;
    std::string style;
    std::string status;
    double strike_price{};
    std::int64_t multiplier{};
    bool tradable{};
    std::optional<std::int64_t> open_interest;
    std::string open_interest_date;
    std::optional<double> close_price;
    std::string close_price_date;
};

struct OptionQuote {
    std::string symbol;
    std::string timestamp;
    double bid_price{};
    double ask_price{};
    std::int64_t bid_size{};
    std::int64_t ask_size{};
    std::string bid_exchange;
    std::string ask_exchange;
};

struct OptionSnapshot {
    std::string symbol;
    std::string observed_at;
    std::optional<OptionQuote> latest_quote;
    std::string trade_timestamp;
    std::optional<double> trade_price;
    std::optional<std::int64_t> trade_size;
    std::optional<double> implied_volatility;
    std::optional<double> delta;
    std::optional<double> gamma;
    std::optional<double> theta;
    std::optional<double> vega;
    std::optional<double> rho;
};

struct OptionObservation {
    OptionSnapshot snapshot;
    std::string observation_date;
    std::string underlying_symbol;
    std::string expiration_date;
    std::string type;
    double strike_price{};
    std::int64_t multiplier{100};
    std::optional<std::int64_t> open_interest;
};

}  // namespace options::data
