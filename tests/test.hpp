#pragma once

#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace test {
struct Case { std::string name; std::function<void()> run; };
std::vector<Case>& cases();
struct Register {
    Register(std::string name, std::function<void()> run) { cases().push_back({std::move(name), std::move(run)}); }
};
}

#define TEST_CONCAT_INNER(a, b) a##b
#define TEST_CONCAT(a, b) TEST_CONCAT_INNER(a, b)
#define TEST_CASE(name) \
    static void TEST_CONCAT(test_function_, __LINE__)(); \
    static test::Register TEST_CONCAT(test_registration_, __LINE__)(name, TEST_CONCAT(test_function_, __LINE__)); \
    static void TEST_CONCAT(test_function_, __LINE__)()
#define REQUIRE(expression) \
    do { if (!(expression)) throw std::runtime_error("requirement failed: " #expression); } while (false)

