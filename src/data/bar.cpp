#include "options/data/bar.hpp"

namespace options::data {

bool operator==(const Bar& lhs, const Bar& rhs) {
    return lhs.symbol == rhs.symbol && lhs.timestamp == rhs.timestamp && lhs.open == rhs.open &&
           lhs.high == rhs.high && lhs.low == rhs.low && lhs.close == rhs.close &&
           lhs.volume == rhs.volume && lhs.trade_count == rhs.trade_count && lhs.vwap == rhs.vwap;
}

}  // namespace options::data

