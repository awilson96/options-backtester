#include "options/analysis/intraday_distribution.hpp"

#include <QDateTime>
#include <QTimeZone>

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

namespace options::analysis {
namespace {

struct PendingSession {
    int weekday{};
    bool invalid{};
    std::map<int,IntradayMinuteRecord> minutes;
};

const QTimeZone& eastern_time_zone() {
    static const QTimeZone zone("America/New_York");
    return zone;
}

bool valid_bar(const data::Bar& bar) {
    return std::isfinite(bar.open) && std::isfinite(bar.high) &&
        std::isfinite(bar.low) && std::isfinite(bar.close) &&
        bar.open>0.0 && bar.high>0.0 && bar.low>0.0 && bar.close>0.0 &&
        bar.low<=std::min(bar.open,bar.close) && bar.high>=std::max(bar.open,bar.close);
}

bool approximately_equal(double left,double right) {
    const auto tolerance=1e-9*std::max({1.0,std::abs(left),std::abs(right)});
    return std::abs(left-right)<=tolerance;
}

std::vector<IntradayMinuteRange> piecewise_ranges(const std::vector<int>& minutes) {
    std::vector<IntradayMinuteRange> ranges;
    for(const auto minute:minutes) {
        if(ranges.empty() || minute>ranges.back().last_minute+1)
            ranges.push_back({minute,minute});
        else
            ranges.back().last_minute=minute;
    }
    return ranges;
}

double boundary_threshold(const std::vector<double>& sorted,std::size_t included,bool downside) {
    if(downside) {
        if(included>=sorted.size()) return sorted.back();
        return (sorted[included-1]+sorted[included])/2.0;
    }
    const auto boundary=sorted.size()-included;
    if(boundary==0) return sorted.front();
    return (sorted[boundary-1]+sorted[boundary])/2.0;
}

IntradayQuantileMembership membership_for(
    const std::vector<IntradayMinuteRecord>& minutes,const std::vector<double>& sorted,
    int percentage,bool downside) {
    const auto included=static_cast<std::size_t>(std::ceil(
        static_cast<double>(percentage)*static_cast<double>(sorted.size())/100.0));
    IntradayQuantileMembership membership;
    membership.percentage=percentage;
    membership.price_threshold=boundary_threshold(sorted,included,downside);
    for(const auto& minute:minutes) {
        const auto qualifies=downside
            ? minute.average_price<=membership.price_threshold
            : minute.average_price>=membership.price_threshold;
        if(qualifies) membership.minutes.push_back(minute.minute_of_session);
    }
    membership.ranges=piecewise_ranges(membership.minutes);
    return membership;
}

}  // namespace

IntradayDistributionResult analyze_intraday_distribution(std::span<const data::Bar> bars) {
    const QTime market_open(9,30);
    std::map<QDate,PendingSession> pending;
    for(const auto& bar:bars) {
        const auto timestamp=QDateTime::fromString(
            QString::fromStdString(bar.timestamp),Qt::ISODate).toTimeZone(eastern_time_zone());
        if(!timestamp.isValid() || timestamp.date().dayOfWeek()>Qt::Friday ||
           timestamp.time()<market_open || timestamp.time()>=QTime(16,0)) continue;
        auto& session=pending[timestamp.date()];
        session.weekday=timestamp.date().dayOfWeek();
        const auto minute=market_open.secsTo(timestamp.time())/60;
        if(!valid_bar(bar) || minute<0 || minute>=intraday_regular_session_minutes) {
            session.invalid=true;
            continue;
        }
        const IntradayMinuteRecord record{
            minute,bar.open,bar.high,bar.low,bar.close,(bar.open+bar.close)/2.0};
        if(!session.minutes.emplace(minute,record).second) session.invalid=true;
    }

    IntradayDistributionResult result;
    result.candidate_sessions=static_cast<int>(pending.size());
    for(auto& [date,pending_session]:pending) {
        if(pending_session.invalid) {
            ++result.excluded_invalid_sessions;
            continue;
        }
        if(pending_session.minutes.size()!=intraday_regular_session_minutes) {
            ++result.excluded_incomplete_sessions;
            continue;
        }
        IntradaySessionRecord session;
        session.date=date.toString(Qt::ISODate).toStdString();
        session.weekday=pending_session.weekday;
        for(auto& [minute,record]:pending_session.minutes)
            session.minutes.push_back(std::move(record));
        session.session_low=std::ranges::min_element(
            session.minutes,{},&IntradayMinuteRecord::low)->low;
        session.session_high=std::ranges::max_element(
            session.minutes,{},&IntradayMinuteRecord::high)->high;
        std::vector<double> sorted_prices;
        sorted_prices.reserve(session.minutes.size());
        for(const auto& minute:session.minutes) {
            sorted_prices.push_back(minute.average_price);
            if(approximately_equal(minute.low,session.session_low))
                session.low_minutes.push_back(minute.minute_of_session);
            if(approximately_equal(minute.high,session.session_high))
                session.high_minutes.push_back(minute.minute_of_session);
        }
        std::ranges::sort(sorted_prices);
        for(std::size_t index=0;index<intraday_distribution_percentages.size();++index) {
            const auto percentage=intraday_distribution_percentages[index];
            session.downside[index]=membership_for(
                session.minutes,sorted_prices,percentage,true);
            session.upside[index]=membership_for(
                session.minutes,sorted_prices,percentage,false);
        }
        result.sessions.push_back(std::move(session));
    }
    return result;
}

std::array<IntradayWeekdayAggregation,5> aggregate_intraday_weekdays(
    std::span<const IntradaySessionRecord> sessions) {
    std::array<IntradayWeekdayAggregation,5> weekdays;
    for(std::size_t index=0;index<weekdays.size();++index) {
        auto& weekday=weekdays[index];
        weekday.weekday=static_cast<int>(index+1);
        for(std::size_t level=0;level<intraday_distribution_percentages.size();++level) {
            weekday.downside[level].percentage=intraday_distribution_percentages[level];
            weekday.upside[level].percentage=intraday_distribution_percentages[level];
        }
    }

    const auto add_minutes=[](IntradayMinuteHistogram& histogram,
                              const std::vector<int>& minutes) {
        for(const auto minute:minutes) {
            if(minute<0 || minute>=intraday_regular_session_minutes) continue;
            ++histogram.counts[static_cast<std::size_t>(minute)];
            ++histogram.total_occurrences;
        }
    };
    for(const auto& session:sessions) {
        if(session.weekday<1 || session.weekday>5) continue;
        auto& weekday=weekdays[static_cast<std::size_t>(session.weekday-1)];
        ++weekday.sessions;
        add_minutes(weekday.lows,session.low_minutes);
        add_minutes(weekday.highs,session.high_minutes);
        for(std::size_t level=0;level<intraday_distribution_percentages.size();++level) {
            add_minutes(weekday.downside[level],session.downside[level].minutes);
            add_minutes(weekday.upside[level],session.upside[level].minutes);
        }
    }

    const auto finish_histogram=[](IntradayMinuteHistogram& histogram) {
        histogram.peak_count=*std::ranges::max_element(histogram.counts);
        if(histogram.peak_count==0) return;
        for(std::size_t minute=0;minute<histogram.counts.size();++minute)
            if(histogram.counts[minute]==histogram.peak_count)
                histogram.peak_minutes.push_back(static_cast<int>(minute));
    };
    for(auto& weekday:weekdays) {
        finish_histogram(weekday.lows);
        finish_histogram(weekday.highs);
        for(auto& histogram:weekday.downside) finish_histogram(histogram);
        for(auto& histogram:weekday.upside) finish_histogram(histogram);
    }
    return weekdays;
}

}  // namespace options::analysis
