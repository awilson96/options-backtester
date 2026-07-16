#include "options/backtest/long_call_backtest.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>

namespace options::backtest {
namespace {

long long days_from_civil(int year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(year - era * 400);
    const unsigned doy = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097LL + static_cast<long long>(doe);
}

long long date_number(const std::string& value) {
    int year=0,month=0,day=0;
    if (value.size()<10 || std::sscanf(value.substr(0,10).c_str(),"%d-%d-%d",&year,&month,&day)!=3)
        throw std::invalid_argument("invalid observation date: " + value);
    return days_from_civil(year,static_cast<unsigned>(month),static_cast<unsigned>(day));
}

bool usable_quote(const data::OptionObservation& value, const LongCallConfig& config) {
    if (!value.snapshot.latest_quote) return false;
    const auto& quote=*value.snapshot.latest_quote;
    if (!std::isfinite(quote.bid_price) || !std::isfinite(quote.ask_price) ||
        quote.bid_price < 0 || quote.ask_price <= 0 || quote.ask_price < quote.bid_price) return false;
    const double midpoint=(quote.bid_price+quote.ask_price)/2.0;
    if (midpoint<=0 || (quote.ask_price-quote.bid_price)/midpoint>config.maximum_spread_fraction) return false;
    return true;
}

bool usable_market(const data::OptionObservation& value, const LongCallConfig& config) {
    return usable_quote(value,config) && value.snapshot.delta &&
           value.open_interest.value_or(0)>=config.minimum_open_interest;
}

PerformanceMetrics calculate_metrics(double initial, const std::vector<EquityPoint>& points,
                                     std::size_t orders) {
    PerformanceMetrics result; result.initial_equity=initial; result.final_equity=points.back().value;
    result.total_return=result.final_equity/initial-1.0;
    if(points.size()>1 && result.final_equity>0)
        result.annualized_return=std::pow(result.final_equity/initial,252.0/(points.size()-1))-1.0;
    double peak=initial;
    for(const auto& point:points) { peak=std::max(peak,point.value); result.max_drawdown=std::max(result.max_drawdown,1.0-point.value/peak); }
    result.orders=orders; return result;
}

}  // namespace

LongCallResult run_long_call_strategy(std::span<const data::OptionObservation> observations,
                                      std::span<const data::Bar> underlying_bars,
                                      const LongCallConfig& config) {
    if(config.initial_cash<=0 || config.minimum_dte<0 || config.maximum_dte<config.minimum_dte ||
       config.hold_sessions==0 || config.maximum_spread_fraction<0)
        throw std::invalid_argument("invalid long-call configuration");
    if(observations.empty()) throw std::invalid_argument("no option snapshot history available");
    if(underlying_bars.empty()) throw std::invalid_argument("no underlying bars available");

    std::map<std::string,std::vector<const data::OptionObservation*>> by_date;
    for(const auto& observation:observations)
        by_date[observation.observation_date.empty()
            ? observation.snapshot.observed_at.substr(0,10) : observation.observation_date].push_back(&observation);
    if(by_date.size()<2) throw std::invalid_argument("at least two daily option snapshots are required");

    struct Position { std::string symbol,entry_date; double entry{},last_bid{}; std::int64_t multiplier{}; std::size_t held{}; };
    std::optional<Position> position;
    double cash=config.initial_cash;
    LongCallResult result; result.underlying=observations.front().underlying_symbol; result.config=config;

    for(const auto& [date,values]:by_date) {
        if(position) {
            const auto found=std::find_if(values.begin(),values.end(),[&](const auto* value){return value->snapshot.symbol==position->symbol && value->snapshot.latest_quote;});
            if(found!=values.end()) {
                const auto bid=(*found)->snapshot.latest_quote->bid_price;
                if(std::isfinite(bid) && bid>=0) position->last_bid=bid;
            }
            if(position->held>=config.hold_sessions && found!=values.end() && usable_quote(**found,config)) {
                const double exit=(*found)->snapshot.latest_quote->bid_price;
                cash+=exit*position->multiplier;
                result.trades.push_back({position->symbol,position->entry_date,date,position->entry,exit,
                                         position->multiplier,(exit-position->entry)*position->multiplier});
                position.reset();
            }
        }

        if(!position) {
            const data::OptionObservation* selected=nullptr;
            double best=std::numeric_limits<double>::infinity();
            for(const auto* value:values) {
                const auto dte=date_number(value->expiration_date)-date_number(date);
                if(value->type!="call" || dte<config.minimum_dte || dte>config.maximum_dte) continue;
                if(!usable_market(*value,config)) { ++result.rejected_markets; continue; }
                const double cost=value->snapshot.latest_quote->ask_price*value->multiplier;
                if(cost>cash) continue;
                const double distance=std::abs(*value->snapshot.delta-config.target_delta);
                if(distance<best) { best=distance; selected=value; }
            }
            if(selected) {
                const auto ask=selected->snapshot.latest_quote->ask_price;
                cash-=ask*selected->multiplier;
                position=Position{selected->snapshot.symbol,date,ask,
                                  selected->snapshot.latest_quote->bid_price,selected->multiplier,0};
            }
        }
        result.strategy_equity.push_back({date,cash+(position ? position->last_bid*position->multiplier : 0.0)});
        if(position) ++position->held;
    }

    struct SessionPrices {
        std::string first_timestamp;
        std::string last_timestamp;
        double open{};
        double close{};
    };
    std::map<std::string,SessionPrices> bars_by_date;
    for(const auto& bar:underlying_bars) {
        const auto date=bar.timestamp.substr(0,10);
        auto [found,inserted]=bars_by_date.try_emplace(
            date,SessionPrices{bar.timestamp,bar.timestamp,bar.open,bar.close});
        if(!inserted && bar.timestamp<found->second.first_timestamp) {
            found->second.first_timestamp=bar.timestamp;
            found->second.open=bar.open;
        }
        if(!inserted && bar.timestamp>found->second.last_timestamp) {
            found->second.last_timestamp=bar.timestamp;
            found->second.close=bar.close;
        }
    }
    const auto first_bar=bars_by_date.lower_bound(by_date.begin()->first);
    if(first_bar==bars_by_date.end()) throw std::invalid_argument("underlying data does not overlap snapshots");
    const auto shares=static_cast<std::int64_t>(std::floor(config.initial_cash/first_bar->second.open));
    const double benchmark_cash=config.initial_cash-shares*first_bar->second.open;
    for(const auto& [date,unused]:by_date) {
        (void)unused;
        const auto bar=bars_by_date.find(date);
        if(bar!=bars_by_date.end())
            result.buy_hold_equity.push_back({date,benchmark_cash+shares*bar->second.close});
    }
    if(result.buy_hold_equity.empty()) throw std::invalid_argument("no underlying closes align with snapshots");
    result.strategy=calculate_metrics(config.initial_cash,result.strategy_equity,result.trades.size()*2+(position?1:0));
    result.buy_and_hold=calculate_metrics(config.initial_cash,result.buy_hold_equity,shares>0?1:0);
    return result;
}

}  // namespace options::backtest
