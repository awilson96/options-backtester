#include "test.hpp"
#include "options/analysis/intraday_distribution.hpp"

#include <QDate>
#include <QDateTime>
#include <QTimeZone>

#include <cmath>
#include <vector>

namespace {

options::data::Bar minute_bar(const QDate& date,int minute,double price) {
    const auto eastern=QDateTime(date,QTime(9,30).addSecs(minute*60),
        QTimeZone("America/New_York"));
    return {"TEST",eastern.toUTC().toString(Qt::ISODate).toStdString(),price,price+0.5,
        price-0.5,price,100,10,price};
}

void add_session(std::vector<options::data::Bar>& bars,const QDate& date,int minutes=390,
                 double base_price=100.0) {
    for(int minute=0;minute<minutes;++minute)
        bars.push_back(minute_bar(date,minute,base_price+minute));
}

}  // namespace

TEST_CASE("intraday distribution creates per-session candle-count quantile memberships") {
    std::vector<options::data::Bar> bars;
    add_session(bars,QDate(2025,1,6));

    const auto result=options::analysis::analyze_intraday_distribution(bars);
    REQUIRE(result.candidate_sessions==1);
    REQUIRE(result.sessions.size()==1);
    const auto& session=result.sessions.front();
    REQUIRE(session.minutes.size()==390);
    REQUIRE(session.low_minutes.size()==1);
    REQUIRE(session.low_minutes.front()==0);
    REQUIRE(session.high_minutes.size()==1);
    REQUIRE(session.high_minutes.front()==389);
    REQUIRE(session.downside[0].percentage==50);
    REQUIRE(session.downside[0].minutes.size()==195);
    REQUIRE(session.downside[0].ranges.size()==1);
    REQUIRE(session.downside[0].ranges.front().first_minute==0);
    REQUIRE(session.downside[0].ranges.front().last_minute==194);
    REQUIRE(session.upside[0].minutes.size()==195);
    REQUIRE(session.upside[0].ranges.front().first_minute==195);
    REQUIRE(session.upside[0].ranges.front().last_minute==389);
    REQUIRE(session.downside[4].percentage==10);
    REQUIRE(session.downside[4].minutes.size()==39);
    REQUIRE(session.upside[4].minutes.size()==39);
    REQUIRE(std::abs(session.downside[4].price_threshold-138.5)<1e-9);
    REQUIRE(std::abs(session.upside[4].price_threshold-450.5)<1e-9);
}

TEST_CASE("intraday distribution includes quantile boundary ties") {
    std::vector<options::data::Bar> bars;
    add_session(bars,QDate(2025,1,6));
    bars[39]=minute_bar(QDate(2025,1,6),39,138.0);

    const auto result=options::analysis::analyze_intraday_distribution(bars);
    const auto& ten_percent=result.sessions.front().downside[4];
    REQUIRE(ten_percent.minutes.size()==40);
    REQUIRE(ten_percent.ranges.size()==1);
    REQUIRE(ten_percent.ranges.front().last_minute==39);
}

TEST_CASE("intraday distribution preserves disconnected minute ranges") {
    std::vector<options::data::Bar> bars;
    const auto date=QDate(2025,1,6);
    for(int minute=0;minute<390;++minute) {
        const auto price=minute%10==0 ? 100.0+minute/10 : 1000.0+minute;
        bars.push_back(minute_bar(date,minute,price));
    }

    const auto result=options::analysis::analyze_intraday_distribution(bars);
    const auto& ten_percent=result.sessions.front().downside[4];
    REQUIRE(ten_percent.minutes.size()==39);
    REQUIRE(ten_percent.ranges.size()==39);
    REQUIRE(ten_percent.ranges.front().first_minute==0);
    REQUIRE(ten_percent.ranges.front().last_minute==0);
    REQUIRE(ten_percent.ranges.back().first_minute==380);
    REQUIRE(ten_percent.ranges.back().last_minute==380);
}

TEST_CASE("intraday distribution records tied extrema in weekday histograms") {
    std::vector<options::data::Bar> bars;
    add_session(bars,QDate(2025,1,6));
    bars[200].low=bars[0].low;
    bars[100].high=bars[389].high;

    const auto result=options::analysis::analyze_intraday_distribution(bars);
    const auto& session=result.sessions.front();
    REQUIRE(session.low_minutes.size()==2);
    REQUIRE(session.low_minutes[0]==0);
    REQUIRE(session.low_minutes[1]==200);
    REQUIRE(session.high_minutes.size()==2);
    REQUIRE(session.high_minutes[0]==100);
    REQUIRE(session.high_minutes[1]==389);

    const auto weekdays=options::analysis::aggregate_intraday_weekdays(result.sessions);
    const auto& monday=weekdays[0];
    REQUIRE(monday.sessions==1);
    REQUIRE(monday.lows.total_occurrences==2);
    REQUIRE(monday.lows.counts[0]==1);
    REQUIRE(monday.lows.counts[200]==1);
    REQUIRE(monday.lows.peak_count==1);
    REQUIRE(monday.lows.peak_minutes.size()==2);
    REQUIRE(monday.highs.total_occurrences==2);
    REQUIRE(monday.highs.counts[100]==1);
    REQUIRE(monday.highs.counts[389]==1);
    REQUIRE(monday.downside[0].percentage==50);
    REQUIRE(monday.downside[0].total_occurrences==195);
}

TEST_CASE("intraday distribution excludes incomplete and invalid sessions") {
    std::vector<options::data::Bar> bars;
    add_session(bars,QDate(2025,1,6));
    add_session(bars,QDate(2025,1,13),389);
    add_session(bars,QDate(2025,1,20));
    bars.push_back(minute_bar(QDate(2025,1,20),100,200.0));

    const auto result=options::analysis::analyze_intraday_distribution(bars);
    REQUIRE(result.candidate_sessions==3);
    REQUIRE(result.sessions.size()==1);
    REQUIRE(result.excluded_incomplete_sessions==1);
    REQUIRE(result.excluded_invalid_sessions==1);
}

TEST_CASE("intraday distribution observes Eastern daylight-saving transitions") {
    std::vector<options::data::Bar> bars;
    const auto before_transition=QDate(2025,3,3);
    const auto after_transition=QDate(2025,3,10);
    add_session(bars,before_transition);
    add_session(bars,after_transition);
    const auto before_open=QDateTime::fromString(
        QString::fromStdString(bars.front().timestamp),Qt::ISODate);
    const auto after_open=QDateTime::fromString(
        QString::fromStdString(bars[390].timestamp),Qt::ISODate);
    REQUIRE(before_open.time()==QTime(14,30));
    REQUIRE(after_open.time()==QTime(13,30));

    const auto result=options::analysis::analyze_intraday_distribution(bars);
    REQUIRE(result.sessions.size()==2);
    REQUIRE(result.sessions[0].date=="2025-03-03");
    REQUIRE(result.sessions[1].date=="2025-03-10");
    const auto weekdays=options::analysis::aggregate_intraday_weekdays(result.sessions);
    REQUIRE(weekdays[0].sessions==2);
    REQUIRE(weekdays[0].lows.counts[0]==2);
    REQUIRE(weekdays[0].highs.counts[389]==2);
    REQUIRE(weekdays[0].downside[4].total_occurrences==78);
    REQUIRE(weekdays[0].upside[4].total_occurrences==78);
}

TEST_CASE("intraday distribution summarizes complete and partial trading-week rankings") {
    std::vector<options::data::Bar> bars;
    const auto monday=QDate(2025,1,6);
    for(int weekday=0;weekday<5;++weekday)
        add_session(bars,monday.addDays(weekday),390,100.0+weekday*1000.0);
    add_session(bars,monday.addDays(7));
    add_session(bars,monday.addDays(15));

    const auto result=options::analysis::analyze_intraday_distribution(bars);
    const auto summary=options::analysis::summarize_intraday_weeks(result.sessions);
    REQUIRE(summary.weeks_analyzed==3);
    REQUIRE(summary.partial_weeks==2);
    REQUIRE(summary.study_minimum==99.5);
    REQUIRE(summary.study_maximum==4489.5);
    REQUIRE(summary.weekdays[0].average_low_rank==1.0);
    REQUIRE(summary.weekdays[0].average_high_rank==3.0);
    REQUIRE(summary.weekdays[0].weeks_participated==2);
    REQUIRE(summary.weekdays[0].weekly_low_wins==2);
    REQUIRE(summary.weekdays[0].weekly_low_win_percent==100.0);
    REQUIRE(summary.weekdays[1].weeks_participated==2);
    REQUIRE(summary.weekdays[1].average_low_rank==1.5);
    REQUIRE(summary.weekdays[1].average_high_rank==2.5);
    REQUIRE(summary.weekdays[4].average_low_rank==5.0);
    REQUIRE(summary.weekdays[4].average_high_rank==1.0);
    REQUIRE(summary.weekdays[4].weeks_participated==1);
    REQUIRE(summary.weekdays[4].weekly_high_wins==1);
    REQUIRE(summary.weekdays[4].weekly_high_win_percent==100.0);
}
