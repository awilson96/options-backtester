#pragma once

#include "options/data/bar.hpp"

#include <array>
#include <span>
#include <string>
#include <vector>

namespace options::analysis {

inline constexpr std::array<int,5> intraday_distribution_percentages{50,40,30,20,10};
inline constexpr int intraday_regular_session_minutes=390;

struct IntradayMinuteRange {
    int first_minute{};
    int last_minute{};
};

struct IntradayMinuteRecord {
    int minute_of_session{};
    double open{};
    double high{};
    double low{};
    double close{};
    double average_price{};
};

struct IntradayQuantileMembership {
    int percentage{};
    double price_threshold{};
    std::vector<int> minutes;
    std::vector<IntradayMinuteRange> ranges;
};

struct IntradaySessionRecord {
    std::string date;
    int weekday{};
    std::vector<IntradayMinuteRecord> minutes;
    double session_low{};
    double session_high{};
    std::vector<int> low_minutes;
    std::vector<int> high_minutes;
    std::array<IntradayQuantileMembership,5> downside;
    std::array<IntradayQuantileMembership,5> upside;
};

struct IntradayDistributionResult {
    std::vector<IntradaySessionRecord> sessions;
    int candidate_sessions{};
    int excluded_incomplete_sessions{};
    int excluded_invalid_sessions{};
};

struct IntradayMinuteHistogram {
    int percentage{};
    std::array<int,intraday_regular_session_minutes> counts{};
    int total_occurrences{};
    int peak_count{};
    std::vector<int> peak_minutes;
};

struct IntradayWeekdayAggregation {
    int weekday{};
    int sessions{};
    IntradayMinuteHistogram lows;
    IntradayMinuteHistogram highs;
    std::array<IntradayMinuteHistogram,5> downside;
    std::array<IntradayMinuteHistogram,5> upside;
};

struct IntradayWeeklyWeekdaySummary {
    int weekday{};
    int weeks_participated{};
    double average_low_rank{};
    double average_high_rank{};
    int weekly_low_wins{};
    int weekly_high_wins{};
    double weekly_low_win_percent{};
    double weekly_high_win_percent{};
};

struct IntradayWeeklySummary {
    int weeks_analyzed{};
    int partial_weeks{};
    double study_minimum{};
    double study_maximum{};
    std::array<IntradayWeeklyWeekdaySummary,5> weekdays;
};

[[nodiscard]] IntradayDistributionResult analyze_intraday_distribution(
    std::span<const data::Bar> bars);

[[nodiscard]] std::array<IntradayWeekdayAggregation,5> aggregate_intraday_weekdays(
    std::span<const IntradaySessionRecord> sessions);

[[nodiscard]] IntradayWeeklySummary summarize_intraday_weeks(
    std::span<const IntradaySessionRecord> sessions);

}  // namespace options::analysis
