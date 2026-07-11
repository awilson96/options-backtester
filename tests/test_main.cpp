#include "test.hpp"

#include <exception>
#include <iostream>

namespace test { std::vector<Case>& cases() { static std::vector<Case> value; return value; } }

int main() {
    int failures = 0;
    for (const auto& test_case : test::cases()) {
        try { test_case.run(); std::cout << "PASS " << test_case.name << '\n'; }
        catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL " << test_case.name << ": " << error.what() << '\n';
        }
    }
    return failures == 0 ? 0 : 1;
}

