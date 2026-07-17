cmake_minimum_required(VERSION 3.22)
project(options_backtester VERSION 0.1.0 LANGUAGES CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(OPTIONS_BACKTESTER_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

find_package(CURL REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Core Widgets Charts Concurrent)

add_library(options_data
    src/data/bar.cpp
    src/data/bar_validation.cpp
    src/data/sqlite_bar_store.cpp
    src/data/sqlite_option_store.cpp
    src/providers/alpaca/alpaca_client.cpp
    src/providers/alpaca/alpaca_json.cpp
    src/providers/alpaca/option_json.cpp
)
target_include_directories(options_data PUBLIC include)
target_link_libraries(options_data PUBLIC CURL::libcurl SQLite::SQLite3 Qt6::Core)
target_compile_options(options_data PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>
    $<$<BOOL:${OPTIONS_BACKTESTER_WARNINGS_AS_ERRORS}>:-Werror>
)

add_library(options_backtest
    src/backtest/equity_backtest.cpp
    src/backtest/long_call_backtest.cpp
)
target_include_directories(options_backtest PUBLIC include)
target_link_libraries(options_backtest PUBLIC options_data)
target_compile_options(options_backtest PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>
    $<$<BOOL:${OPTIONS_BACKTESTER_WARNINGS_AS_ERRORS}>:-Werror>
)

add_library(options_analysis
    src/analysis/intraday_distribution.cpp
    src/analysis/momentum.cpp
)
target_include_directories(options_analysis PUBLIC include)
target_link_libraries(options_analysis PUBLIC options_data)
target_compile_options(options_analysis PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>
    $<$<BOOL:${OPTIONS_BACKTESTER_WARNINGS_AS_ERRORS}>:-Werror>
)

add_executable(options-backtester apps/cli/main.cpp)
target_link_libraries(options-backtester PRIVATE options_data options_backtest)

add_executable(options-viewer apps/desktop/main.cpp)
target_link_libraries(options-viewer PRIVATE options_data options_analysis Qt6::Widgets Qt6::Charts Qt6::Concurrent)

if(BUILD_TESTING)
    add_executable(options_data_tests
        tests/test_main.cpp
        tests/alpaca_json_test.cpp
        tests/bar_validation_test.cpp
        tests/equity_backtest_test.cpp
        tests/intraday_distribution_test.cpp
        tests/long_call_backtest_test.cpp
        tests/momentum_test.cpp
        tests/option_json_test.cpp
        tests/sqlite_option_store_test.cpp
        tests/sqlite_bar_store_test.cpp
    )
    target_link_libraries(options_data_tests PRIVATE options_data options_backtest options_analysis)
    add_test(NAME options_data_tests COMMAND options_data_tests)
endif()
