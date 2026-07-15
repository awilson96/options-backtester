#include "options/analysis/momentum.hpp"

#include <QDate>

#include <algorithm>
#include <stdexcept>
#include <vector>

namespace options::analysis {
namespace {

struct Observation {
    QDate date;
    double close{};
};

}  // namespace

MomentumResult analyze_momentum(std::span<const data::Bar> bars, std::size_t window_days) {
    if(window_days==0) throw std::invalid_argument("momentum window must be at least one day");

    std::vector<Observation> observations;
    observations.reserve(bars.size());
    for(const auto& bar:bars) {
        const auto date=QDate::fromString(QString::fromStdString(bar.timestamp.substr(0,10)),Qt::ISODate);
        if(date.isValid()) observations.push_back({date,bar.close});
    }
    std::ranges::sort(observations,{},&Observation::date);

    MomentumResult result;
    for(auto left=observations.begin();left!=observations.end();++left) {
        const auto target=left->date.addDays(static_cast<qint64>(window_days));
        const auto right=std::lower_bound(std::next(left),observations.end(),target,
            [](const Observation& observation,const QDate& date){return observation.date<date;});
        if(right==observations.end()) continue;
        ++result.comparisons;
        if(right->close>left->close) ++result.wins;
        else if(right->close<left->close) ++result.losses;
        else ++result.ties;
    }
    if(result.comparisons!=0)
        result.win_percentage=100.0*static_cast<double>(result.wins)/
                              static_cast<double>(result.comparisons);
    return result;
}

}  // namespace options::analysis
