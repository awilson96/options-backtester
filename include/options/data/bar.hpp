#pragma once

#include <cstdint>
#include <string>

namespace options::data {

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

