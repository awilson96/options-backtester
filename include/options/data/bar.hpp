#pragma once

#include <cstdint>
#include <string>

namespace options::data {

inline constexpr char stock_bar_timeframe[] = "1Min";

struct Bar {
    std::string symbol;
    std::string timestamp;
    double open{};
    double high{};
    double low{};
    double close{};
    std::int64_t volume{};
    std::int64_t trade_count{};
    double vwap{};
};

bool operator==(const Bar& lhs, const Bar& rhs);

}  // namespace options::data
