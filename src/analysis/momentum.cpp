#include "options/analysis/momentum.hpp"

#include <QDate>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <iterator>
#include <map>
#include <stdexcept>
#include <vector>

namespace options::analysis {
namespace {

struct Observation {
    QDate date;
    std::string symbol;
    double price{};
};

enum class Outcome { win, loss, tie };

struct ScheduledTrade {
    std::size_t entry{};
    std::size_t exit{};
    Outcome outcome{};
    double profit{};
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

bool should_drop(const Observation& observation,double drop_rate_percent,std::uint64_t drop_seed) {
    if(drop_rate_percent<=0) return false;
    if(drop_rate_percent>=100) return true;
    std::uint64_t hash=1469598103934665603ULL^drop_seed;
    const auto key=observation.symbol+observation.date.toString(Qt::ISODate).toStdString();
    for(const auto character:key) {
        hash^=static_cast<unsigned char>(character);
        hash*=1099511628211ULL;
    }
    const auto percentile=static_cast<double>(hash%1000000ULL)/10000.0;
    return percentile<drop_rate_percent;
}

}  // namespace

MomentumResult analyze_momentum(
    std::span<const data::Bar> bars, std::size_t window_days, std::size_t skip_days,
    std::optional<StrikeAdjustment> strike_adjustment,
    std::optional<SimulatedPricing> simulated_pricing,double drop_rate_percent,
    std::uint64_t drop_seed) {
    if(window_days==0) throw std::invalid_argument("momentum window must be at least one day");
    if(skip_days==0) throw std::invalid_argument("momentum skip window must be at least one day");
    if(!std::isfinite(drop_rate_percent) || drop_rate_percent<0 || drop_rate_percent>100)
        throw std::invalid_argument("drop rate must be between 0 and 100 percent");
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
        const auto price=std::isfinite(bar.vwap) && bar.vwap>0 ? bar.vwap : bar.close;
        if(date.isValid() && std::isfinite(price) && price>0)
            observations.push_back({date,bar.symbol,price});
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
        std::vector<ScheduledTrade> scheduled;
        auto next_eligible_date=observations.front().date.addDays(static_cast<qint64>(phase));
        for(std::size_t left=0;left<observations.size();++left) {
            if(observations[left].date<next_eligible_date) continue;
            if(right_indices[left]==observations.size()) break;
            const auto& right=observations[right_indices[left]];
            const auto threshold=comparison_price(observations[left].price,strike_adjustment);
            if(should_drop(observations[left],drop_rate_percent,drop_seed)) {
                ++result.dropped_comparisons;
                next_eligible_date=observations[left].date.addDays(static_cast<qint64>(skip_days));
                continue;
            }
            ScheduledTrade trade{left,right_indices[left],Outcome::tie,0};
            if(right.price>threshold) {
                trade.outcome=Outcome::win;
                if(simulated_pricing) trade.profit=simulated_pricing->max_profit-friction;
            } else if(right.price<threshold) {
                trade.outcome=Outcome::loss;
                if(simulated_pricing) trade.profit=-(simulated_pricing->max_loss+friction);
            }
            scheduled.push_back(trade);
            next_eligible_date=observations[left].date.addDays(static_cast<qint64>(skip_days));
        }

        MomentumResult phase_result;
        const auto record_trade=[&phase_result](const ScheduledTrade& trade) {
            ++phase_result.comparisons;
            if(trade.outcome==Outcome::win) ++phase_result.wins;
            else if(trade.outcome==Outcome::loss) ++phase_result.losses;
            else ++phase_result.ties;
        };
        if(!simulated_pricing) {
            for(const auto& trade:scheduled) record_trade(trade);
        } else {
            const auto risk=simulated_pricing->max_loss+friction;

            double realized_profit=0;
            double reserved_capital=0;
            double phase_required_capital=0;
            std::deque<const ScheduledTrade*> required_open;
            for(const auto& trade:scheduled) {
                while(!required_open.empty() && required_open.front()->exit<=trade.entry) {
                    realized_profit+=required_open.front()->profit;
                    reserved_capital-=risk;
                    required_open.pop_front();
                }
                phase_required_capital=std::max(
                    phase_required_capital,reserved_capital+risk-realized_profit);
                reserved_capital+=risk;
                required_open.push_back(&trade);
            }
            result.required_capital=std::max(result.required_capital,phase_required_capital);

            double balance=simulated_pricing->allocation;
            reserved_capital=0;
            std::deque<const ScheduledTrade*> funded_open;
            const auto close_position=[&](const ScheduledTrade& trade) {
                reserved_capital-=risk;
                balance+=trade.profit;
                profit_changes[observations[trade.exit].date]+=trade.profit;
            };
            for(const auto& trade:scheduled) {
                while(!funded_open.empty() && funded_open.front()->exit<=trade.entry) {
                    close_position(*funded_open.front());
                    funded_open.pop_front();
                }
                if(balance-reserved_capital+1e-9<risk) {
                    ++phase_result.skipped_comparisons;
                    continue;
                }
                reserved_capital+=risk;
                funded_open.push_back(&trade);
                record_trade(trade);
            }
            while(!funded_open.empty()) {
                close_position(*funded_open.front());
                funded_open.pop_front();
            }
        }
        if(phase_result.comparisons!=0)
            phase_result.win_percentage=100.0*phase_result.wins/phase_result.comparisons;
        result.comparisons+=phase_result.comparisons;
        result.skipped_comparisons+=phase_result.skipped_comparisons;
        result.wins+=phase_result.wins;
        result.losses+=phase_result.losses;
        result.ties+=phase_result.ties;
        result.win_percentage+=phase_result.win_percentage;
    }
    const auto phases=static_cast<double>(skip_days);
    result.comparisons/=phases;
    result.skipped_comparisons/=phases;
    result.dropped_comparisons/=phases;
    result.wins/=phases;
    result.losses/=phases;
    result.ties/=phases;
    result.win_percentage/=phases;
    if(simulated_pricing) {
        double cumulative=0;
        result.profit_curve.push_back(
            {observations.front().date.toString(Qt::ISODate).toStdString(),0});
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

MomentumResult analyze_momentum_drop_scenarios(
    std::span<const data::Bar> bars, std::size_t window_days, std::size_t skip_days,
    std::optional<StrikeAdjustment> strike_adjustment,
    std::optional<SimulatedPricing> simulated_pricing,
    double drop_rate_percent, std::size_t scenario_count) {
    if(scenario_count<5) throw std::invalid_argument("drop analysis requires at least five scenarios");
    if(drop_rate_percent<=0)
        return analyze_momentum(bars,window_days,skip_days,strike_adjustment,simulated_pricing,0);

    std::vector<MomentumResult> scenarios;
    scenarios.reserve(scenario_count);
    for(std::size_t index=0;index<scenario_count;++index)
        scenarios.push_back(analyze_momentum(
            bars,window_days,skip_days,strike_adjustment,simulated_pricing,
            drop_rate_percent,static_cast<std::uint64_t>(index+1)));
    std::ranges::sort(scenarios,[simulated_pricing](const auto& left,const auto& right) {
        const auto left_score=simulated_pricing ? left.total_profit : left.win_percentage;
        const auto right_score=simulated_pricing ? right.total_profit : right.win_percentage;
        if(left_score!=right_score) return left_score<right_score;
        return left.comparisons<right.comparisons;
    });

    auto result=scenarios[scenario_count/2];
    result.low_profit_curve=scenarios.front().profit_curve;
    result.high_profit_curve=scenarios.back().profit_curve;
    const auto no_drop=analyze_momentum(
        bars,window_days,skip_days,strike_adjustment,simulated_pricing,0);
    result.no_drop_profit_curve=no_drop.profit_curve;
    result.drop_scenario_count=scenario_count;
    return result;
}

}  // namespace options::analysis
