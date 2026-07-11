#pragma once

#include "options/data/bar.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace options::providers::alpaca {

struct BarsPage {
    std::vector<data::Bar> bars;
    std::string next_page_token;
};

BarsPage parse_bars_page(std::string_view json);

}  // namespace options::providers::alpaca

