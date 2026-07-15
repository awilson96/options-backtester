#include "options/analysis/momentum.hpp"

#include <QDate>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <map>
#include <stdexcept>
#include <vector>

namespace options::analysis {
namespace {

struct Observation {
    QDate date;
    double close{};
};

double comparison_price(double underlying,const std::optional<StrikeAdjustment>& adjustment) {
    if(!adjustment) return underlying;
    const auto units=underlying/adjustment->width;
    if(adjustment->offset<0)
        return (std::ceil(units)+static_cast<double>(adjustment->offset))*adjustment->width;
    if(adjustment->offset>0)
        return (std::floor(units)+static_cast<double>(adjustment->offset))*adjustment->width;
    return std::round(units)*adjustment->width;
}

double slippage_cost(const SimulatedPricing& pricing) {
    double result=0;
    if(pricing.slippage_mode==SlippageMode::buy ||
       pricing.slippage_mode==SlippageMode::buy_and_sell)
        result+=pricing.buy_slippage_per_share*100.0;
    if(pricing.slippage_mode==SlippageMode::sell ||
       pricing.slippage_mode==SlippageMode::buy_and_sell)
        result+=pricing.sell_slippage_per_share*100.0;
    return result;
}

}  // namespace

MomentumResult analyze_momentum(
    std::span<const data::Bar> bars, std::size_t window_days, std::size_t skip_days,
    std::optional<StrikeAdjustment> strike_adjustment,
    std::optional<SimulatedPricing> simulated_pricing) {
    if(window_days==0) throw std::invalid_argument("momentum window must be at least one day");
    if(skip_days==0) throw std::invalid_argument("momentum skip window must be at least one day");
    if(strike_adjustment && (!std::isfinite(strike_adjustment->width) || strike_adjustment->width<=0))
        throw std::invalid_argument("strike width must be positive and finite");
    if(simulated_pricing && (!std::isfinite(simulated_pricing->max_profit) ||
       !std::isfinite(simulated_pricing->max_loss) || simulated_pricing->max_profit<0 ||
       simulated_pricing->max_loss<=0 || !std::isfinite(simulated_pricing->allocation) ||
       simulated_pricing->allocation<=0 ||
       !std::isfinite(simulated_pricing->buy_slippage_per_share) ||
       !std::isfinite(simulated_pricing->sell_slippage_per_share) ||
       simulated_pricing->buy_slippage_per_share<0 || simulated_pricing->sell_slippage_per_share<0))
        throw std::invalid_argument(
            "simulated max profit must be non-negative; max loss and allocation must be positive");

    std::vector<Observation> observations;
    observations.reserve(bars.size());
    for(const auto& bar:bars) {
        const auto date=QDate::fromString(QString::fromStdString(bar.timestamp.substr(0,10)),Qt::ISODate);
        if(date.isValid()) observations.push_back({date,bar.close});
    }
    std::ranges::sort(observations,{},&Observation::date);
    if(observations.empty()) return {};

    std::vector<std::size_t> right_indices(observations.size(),observations.size());
    for(std::size_t index=0;index<observations.size();++index) {
        const auto target=observations[index].date.addDays(static_cast<qint64>(window_days));
        const auto right=std::lower_bound(std::next(observations.begin(),static_cast<std::ptrdiff_t>(index+1)),
            observations.end(),target,
            [](const Observation& observation,const QDate& date){return observation.date<date;});
        if(right!=observations.end())
            right_indices[index]=static_cast<std::size_t>(std::distance(observations.begin(),right));
    }

    MomentumResult result;
    std::map<QDate,double> profit_changes;
    const auto friction=simulated_pricing ? slippage_cost(*simulated_pricing) : 0.0;
    for(std::size_t phase=0;phase<skip_days;++phase) {
        MomentumResult phase_result;
        auto next_eligible_date=observations.front().date.addDays(static_cast<qint64>(phase));
        for(std::size_t left=0;left<observations.size();++left) {
            if(observations[left].date<next_eligible_date) continue;
            if(right_indices[left]==observations.size()) break;
            const auto& right=observations[right_indices[left]];
            const auto threshold=comparison_price(observations[left].close,strike_adjustment);
            ++phase_result.comparisons;
            double profit=0;
            if(right.close>threshold) {
                ++phase_result.wins;
                if(simulated_pricing) profit=simulated_pricing->max_profit-friction;
            } else if(right.close<threshold) {
                ++phase_result.losses;
                if(simulated_pricing) profit=-(simulated_pricing->max_loss+friction);
            } else {
                ++phase_result.ties;
            }
            if(simulated_pricing) profit_changes[right.date]+=profit;
            next_eligible_date=observations[left].date.addDays(static_cast<qint64>(skip_days));
        }
        if(phase_result.comparisons!=0)
            phase_result.win_percentage=100.0*phase_result.wins/phase_result.comparisons;
        result.comparisons+=phase_result.comparisons;
        result.wins+=phase_result.wins;
        result.losses+=phase_result.losses;
        result.ties+=phase_result.ties;
        result.win_percentage+=phase_result.win_percentage;
    }
    const auto phases=static_cast<double>(skip_days);
    result.comparisons/=phases;
    result.wins/=phases;
    result.losses/=phases;
    result.ties/=phases;
    result.win_percentage/=phases;
    if(simulated_pricing) {
        double cumulative=0;
        for(const auto& [date,change]:profit_changes) {
            cumulative+=change/phases;
            result.profit_curve.push_back({date.toString(Qt::ISODate).toStdString(),cumulative});
        }
        result.total_profit=cumulative;
        result.allocation=simulated_pricing->allocation;
        result.ending_balance=result.allocation+result.total_profit;
        result.profit_percentage=100.0*(result.ending_balance/result.allocation-1.0);
    }
    return result;
}

}  // namespace options::analysis
