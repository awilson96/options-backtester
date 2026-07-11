#pragma once

#include "options/data/bar.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace options::data {

struct ValidationReport {
    std::size_t bar_count{};
    std::string first_timestamp;
    std::string last_timestamp;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    [[nodiscard]] bool valid() const { return errors.empty(); }
};

ValidationReport validate_bars(std::span<const Bar> bars);

}  // namespace options::data

