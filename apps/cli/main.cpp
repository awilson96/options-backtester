#include "options/backtest/equity_backtest.hpp"
#include "options/backtest/long_call_backtest.hpp"
#include "options/data/bar_validation.hpp"
#include "options/data/sqlite_bar_store.hpp"
#include "options/data/sqlite_option_store.hpp"
#include "options/providers/alpaca/alpaca_client.hpp"

#include <QDateTime>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cmath>
#include <exception>
#include <iostream>
#include <map>
#include <iomanip>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>

namespace {

std::atomic_bool stop_requested{false};

void request_stop(int) { stop_requested.store(true); }

std::string environment(const char* name, bool required = true) {
    const char* value = std::getenv(name);
    if (value && *value) return value;
    if (required) throw std::runtime_error(std::string("missing environment variable: ") + name);
    return {};
}

std::unordered_map<std::string, std::string> arguments(int argc, char** argv, int start) {
    std::unordered_map<std::string, std::string> result;
    for (int i = start; i < argc; ++i) {
        const std::string key = argv[i];
        if (!key.starts_with("--")) throw std::runtime_error("expected --name value");
        if (i + 1 >= argc || std::string(argv[i + 1]).starts_with("--")) {
            result[key.substr(2)] = "true";
        } else {
            result[key.substr(2)] = argv[++i];
        }
    }
    return result;
}

std::string required(const std::unordered_map<std::string, std::string>& args,
                     const std::string& key) {
    const auto found = args.find(key);
    if (found == args.end() || found->second.empty()) throw std::runtime_error("missing --" + key);
    return found->second;
}

std::string optional(const std::unordered_map<std::string, std::string>& args,
                     const std::string& key, std::string fallback = {}) {
    const auto found = args.find(key);
    return found == args.end() ? std::move(fallback) : found->second;
}

double number(const std::unordered_map<std::string, std::string>& args,
              const std::string& key, double fallback) {
    const auto value = optional(args, key);
    if (value.empty()) return fallback;
    std::size_t consumed = 0;
    const double result = std::stod(value, &consumed);
    if (consumed != value.size()) throw std::runtime_error("invalid --" + key);
    return result;
}

std::size_t positive_integer(const std::unordered_map<std::string, std::string>& args,
                             const std::string& key, std::size_t fallback) {
    const double value = number(args, key, static_cast<double>(fallback));
    if (value <= 0 || value != std::floor(value))
        throw std::runtime_error("--" + key + " must be a positive integer");
    return static_cast<std::size_t>(value);
}

options::data::BarQuery bar_query(const std::unordered_map<std::string, std::string>& args) {
    const char* feed_value = std::getenv("ALPACA_DATA_FEED");
    return {required(args, "symbol"), "alpaca",
            optional(args, "feed", feed_value && *feed_value ? feed_value : "iex"),
            optional(args, "timeframe", "1Day"), optional(args, "adjustment", "all"),
            optional(args, "start"), optional(args, "end")};
}

void print_metrics(const std::string& label,
                   const options::backtest::PerformanceMetrics& metrics) {
    std::cout << label << "\n"
              << "  Final equity:      $" << std::fixed << std::setprecision(2)
              << metrics.final_equity << "\n"
              << "  Total return:      " << std::setprecision(2) << metrics.total_return * 100.0
              << "%\n"
              << "  Annualized return: " << metrics.annualized_return * 100.0 << "%\n"
              << "  Maximum drawdown:  " << metrics.max_drawdown * 100.0 << "%\n"
              << "  Orders:            " << metrics.orders << "\n";
}

std::vector<std::string> split_symbols(const std::string& value) {
    std::vector<std::string> result;
    std::size_t start = 0;
    while (start < value.size()) {
        const auto comma = value.find(',', start);
        result.push_back(value.substr(start, comma - start));
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return result;
}

void usage() {
    std::cout << "Usage:\n"
              << "  options-backtester download-bars --symbols SPY,AAPL --start YYYY-MM-DD "
                 "--end YYYY-MM-DD [--timeframe 1Day] [--db market-data.db]\n"
              << "    Add --refresh to bypass cached download coverage.\n"
              << "  options-backtester count-bars --symbol SPY [--db market-data.db]\n"
              << "  options-backtester validate-bars --symbol SPY [--start YYYY-MM-DD] "
                 "[--end YYYY-MM-DD]\n"
              << "  options-backtester backtest-sma --symbol SPY --start YYYY-MM-DD "
                 "--end YYYY-MM-DD [--short-window 20] [--long-window 50] "
                 "[--cash 10000]\n"
              << "  options-backtester download-option-contracts --underlying SPY "
                 "--expiration-from YYYY-MM-DD --expiration-to YYYY-MM-DD\n"
              << "  options-backtester download-option-bars --symbols OCC1,OCC2 "
                 "--start YYYY-MM-DD --end YYYY-MM-DD [--timeframe 1Day]\n"
              << "  options-backtester collect-option-quotes --symbols OCC1,OCC2\n"
              << "  options-backtester collect-option-snapshots --underlying SPY "
                 "[--expiration-from YYYY-MM-DD] [--expiration-to YYYY-MM-DD]\n"
              << "  options-backtester list-option-contracts --underlying SPY [--limit 50]\n"
              << "  options-backtester backtest-long-call --underlying SPY --start YYYY-MM-DD "
                 "--end YYYY-MM-DD [--target-delta 0.50] [--min-dte 30] [--max-dte 45] "
                 "[--hold-sessions 5] [--max-spread 0.25] [--cash 10000]\n"
              << "  options-backtester serve-option-snapshots --underlyings SPY,AAPL "
                 "--expiration-from YYYY-MM-DD --expiration-to YYYY-MM-DD "
                 "[--interval-seconds 300] [--feed indicative] [--db market-data.db]\n";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2) { usage(); return 1; }
        const std::string command = argv[1];
        const auto args = arguments(argc, argv, 2);
        const auto db_it = args.find("db");
        const std::string db_path = db_it == args.end() ? "market-data.db" : db_it->second;
        options::data::SqliteBarStore store(db_path);

        const auto option_feed = [&args] {
            const char* value = std::getenv("ALPACA_OPTION_FEED");
            return optional(args, "feed", value && *value ? value : "indicative");
        };

        if(command=="serve-option-snapshots") {
            const auto underlyings=split_symbols(required(args,"underlyings"));
            if(underlyings.empty()) throw std::runtime_error("at least one underlying is required");
            const auto expiration_from=required(args,"expiration-from");
            const auto expiration_to=required(args,"expiration-to");
            const auto interval=positive_integer(args,"interval-seconds",300);
            const auto feed=option_feed();
            options::data::SqliteOptionStore option_store(db_path);
            options::providers::alpaca::AlpacaClient client(
                {environment("ALPACA_API_KEY"),environment("ALPACA_API_SECRET")});
            const auto clock=client.fetch_market_clock();
            if(!clock.is_open) {
                std::cout << "US market is closed; snapshot server will not start.\n"
                          << "Next open: " << clock.next_open << "\n";
                return 0;
            }
            const auto market_close=QDateTime::fromString(
                QString::fromStdString(clock.next_close),Qt::ISODate);
            if(!market_close.isValid()) throw std::runtime_error("Alpaca returned an invalid next_close");

            options::providers::alpaca::OptionContractsRequest contracts_request;
            contracts_request.underlying_symbols=underlyings;
            contracts_request.expiration_date_gte=expiration_from;
            contracts_request.expiration_date_lte=expiration_to;
            const auto contracts=client.fetch_option_contracts(contracts_request);
            option_store.upsert_contracts(contracts);
            std::cout << "Snapshot server started for ";
            for(std::size_t i=0;i<underlyings.size();++i) {
                if(i) std::cout << ',';
                std::cout << underlyings[i];
            }
            std::cout << " at " << interval << " second intervals; market close: "
                      << clock.next_close << "\nLoaded " << contracts.size()
                      << " contract records. Press Ctrl+C to stop.\n";
            std::signal(SIGINT,request_stop);
            std::signal(SIGTERM,request_stop);
            std::size_t cycle=0;
            while(!stop_requested.load() && QDateTime::currentDateTimeUtc()<market_close) {
                std::size_t collected=0;
                for(const auto& underlying:underlyings) {
                    if(stop_requested.load() || QDateTime::currentDateTimeUtc()>=market_close) break;
                    try {
                        options::providers::alpaca::OptionChainRequest request;
                        request.underlying_symbol=underlying;
                        request.feed=feed;
                        request.expiration_date_gte=expiration_from;
                        request.expiration_date_lte=expiration_to;
                        const auto snapshots=client.fetch_option_chain(request);
                        option_store.upsert_snapshots(snapshots,feed);
                        option_store.insert_snapshot_observations(snapshots,feed);
                        collected+=snapshots.size();
                    } catch(const std::exception& error) {
                        std::cerr << "collection failed for " << underlying << ": "
                                  << error.what() << "\n";
                    }
                }
                ++cycle;
                std::cout << QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs).toStdString()
                          << " cycle " << cycle << ": stored " << collected
                          << " snapshot observations\n" << std::flush;
                for(std::size_t elapsed=0;elapsed<interval && !stop_requested.load();++elapsed) {
                    if(QDateTime::currentDateTimeUtc()>=market_close) break;
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                }
            }
            std::cout << (stop_requested.load() ? "Snapshot server stopped by signal.\n"
                                                : "Market closed; snapshot server stopped.\n");
            return 0;
        }

        if (command == "list-option-contracts") {
            options::data::SqliteOptionStore option_store(db_path);
            const auto contracts = option_store.load_contracts(
                required(args,"underlying"),
                static_cast<std::int64_t>(positive_integer(args,"limit",50)));
            std::cout << "SYMBOL\tEXPIRATION\tTYPE\tSTRIKE\tOPEN_INTEREST\tOI_DATE\n";
            for (const auto& contract:contracts) {
                std::cout << contract.symbol << '\t' << contract.expiration_date << '\t'
                          << contract.type << '\t' << contract.strike_price << '\t';
                if (contract.open_interest) std::cout << *contract.open_interest;
                else std::cout << "N/A";
                std::cout << '\t' << contract.open_interest_date << '\n';
            }
            std::cout << contracts.size() << " contracts shown\n";
            return 0;
        }

        if (command == "download-option-contracts" || command == "download-option-bars" ||
            command == "collect-option-quotes" || command == "collect-option-snapshots") {
            options::data::SqliteOptionStore option_store(db_path);
            options::providers::alpaca::AlpacaClient client(
                {environment("ALPACA_API_KEY"), environment("ALPACA_API_SECRET")});
            if (command == "download-option-contracts") {
                options::providers::alpaca::OptionContractsRequest request;
                request.underlying_symbols = split_symbols(required(args, "underlying"));
                request.expiration_date_gte = required(args, "expiration-from");
                request.expiration_date_lte = required(args, "expiration-to");
                request.status = optional(args, "status", "active");
                const auto contracts = client.fetch_option_contracts(request);
                option_store.upsert_contracts(contracts);
                std::cout << "Downloaded and stored " << contracts.size() << " option contracts\n";
                return 0;
            }
            if (command == "download-option-bars") {
                options::providers::alpaca::BarsRequest request;
                request.symbols = split_symbols(required(args, "symbols"));
                request.start = required(args, "start");
                request.end = required(args, "end");
                request.timeframe = optional(args, "timeframe", "1Day");
                request.feed = option_feed();
                const auto bars = client.fetch_option_bars(request);
                option_store.upsert_bars(bars, request.feed, request.timeframe);
                std::cout << "Downloaded and stored " << bars.size() << " option bars\n";
                return 0;
            }
            if (command == "collect-option-quotes") {
                const auto symbols = split_symbols(required(args, "symbols"));
                const auto feed = option_feed();
                const auto quotes = client.fetch_latest_option_quotes(symbols, feed);
                const auto observed = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs).toStdString();
                option_store.upsert_quotes(quotes, feed, observed);
                std::cout << "Collected and stored " << quotes.size() << " latest option quotes\n";
                return 0;
            }
            options::providers::alpaca::OptionChainRequest request;
            request.underlying_symbol = required(args, "underlying");
            request.feed = option_feed();
            request.expiration_date_gte = optional(args, "expiration-from");
            request.expiration_date_lte = optional(args, "expiration-to");
            request.type = optional(args, "type");
            const auto snapshots = client.fetch_option_chain(request);
            option_store.upsert_snapshots(snapshots, request.feed);
            std::cout << "Collected and stored " << snapshots.size() << " daily option snapshots\n";
            return 0;
        }

        if (command == "count-bars") {
            const auto symbol = required(args, "symbol");
            std::cout << symbol << ": " << store.count(symbol) << " bars\n";
            return 0;
        }
        if (command == "validate-bars") {
            const auto query = bar_query(args);
            if (query.start.empty() != query.end.empty())
                throw std::runtime_error("validate-bars requires both --start and --end together");
            const auto bars = store.load(query);
            const auto report = options::data::validate_bars(bars);
            std::vector<options::data::DateRange> missing;
            if (!query.start.empty()) {
                missing = store.missing_coverage(query.symbol, query.provider, query.feed,
                                                 query.timeframe, query.adjustment,
                                                 {query.start, query.end});
            }
            std::cout << query.symbol << ": " << report.bar_count << " bars";
            if (!bars.empty()) std::cout << " from " << report.first_timestamp << " through "
                                         << report.last_timestamp;
            std::cout << "\n";
            for (const auto& warning : report.warnings) std::cout << "WARNING: " << warning << "\n";
            for (const auto& error : report.errors) std::cout << "ERROR: " << error << "\n";
            for (const auto& range : missing)
                std::cout << "ERROR: download coverage missing from " << range.start << " through "
                          << range.end << "\n";
            const bool valid = report.valid() && missing.empty();
            std::cout << (valid ? "Validation passed\n" : "Validation failed\n");
            return valid ? 0 : 3;
        }
        if (command == "backtest-long-call") {
            const auto underlying=required(args,"underlying");
            const auto start=required(args,"start");
            const auto end=required(args,"end");
            options::data::SqliteOptionStore option_store(db_path);
            const auto observations=option_store.load_snapshot_history(
                underlying,option_feed(),start,end);
            const char* equity_feed_value=std::getenv("ALPACA_DATA_FEED");
            const std::string equity_feed=optional(
                args,"equity-feed",equity_feed_value && *equity_feed_value ? equity_feed_value : "iex");
            const auto bars=store.load({underlying,"alpaca",equity_feed,"1Day","all",start,end});
            options::backtest::LongCallConfig config;
            config.initial_cash=number(args,"cash",10000.0);
            config.target_delta=number(args,"target-delta",0.50);
            config.minimum_dte=static_cast<int>(number(args,"min-dte",30));
            config.maximum_dte=static_cast<int>(number(args,"max-dte",45));
            config.hold_sessions=positive_integer(args,"hold-sessions",5);
            config.maximum_spread_fraction=number(args,"max-spread",0.25);
            config.minimum_open_interest=static_cast<std::int64_t>(number(args,"min-open-interest",0));
            const auto result=options::backtest::run_long_call_strategy(observations,bars,config);
            std::cout << "Long-call backtest: " << result.underlying << "\n"
                      << "Selection: target delta " << config.target_delta << ", DTE "
                      << config.minimum_dte << '-' << config.maximum_dte << ", hold "
                      << config.hold_sessions << " snapshot sessions\n"
                      << "Execution: buy at ask, sell/mark at bid; one contract; no commissions\n";
            print_metrics("Long-call strategy",result.strategy);
            print_metrics("Underlying buy and hold",result.buy_and_hold);
            std::cout << "Completed trades: " << result.trades.size()
                      << "\nRejected candidate markets: " << result.rejected_markets << "\n";
            for(const auto& trade:result.trades)
                std::cout << "  " << trade.symbol << ' ' << trade.entry_date << " @ ask "
                          << trade.entry_price << " -> " << trade.exit_date << " @ bid "
                          << trade.exit_price << ", P&L $" << trade.pnl << "\n";
            return 0;
        }
        if (command == "backtest-sma") {
            auto query = bar_query(args);
            query.start = required(args, "start");
            query.end = required(args, "end");
            if (query.timeframe != "1Day")
                throw std::runtime_error("backtest-sma currently requires --timeframe 1Day");
            const auto bars = store.load(query);
            const auto report = options::data::validate_bars(bars);
            if (!report.valid()) throw std::runtime_error("stored bars failed validation");
            const auto missing = store.missing_coverage(
                query.symbol, query.provider, query.feed, query.timeframe, query.adjustment,
                {query.start, query.end});
            if (!missing.empty())
                throw std::runtime_error("requested backtest range is not fully downloaded; run download-bars first");
            const auto comparison = options::backtest::run_sma_comparison(
                bars, number(args, "cash", 10000.0), positive_integer(args, "short-window", 20),
                positive_integer(args, "long-window", 50));
            std::cout << "SMA backtest: " << comparison.symbol << "\n"
                      << "Period: " << comparison.start_timestamp << " through "
                      << comparison.end_timestamp << " (" << comparison.bars << " bars)\n"
                      << "Execution: signal at close, trade at next open; whole shares; no fees\n";
            print_metrics("SMA strategy (" + std::to_string(comparison.short_window) + "/" +
                              std::to_string(comparison.long_window) + ")", comparison.strategy);
            print_metrics("Buy and hold", comparison.buy_and_hold);
            std::cout << "Excess total return: " << std::fixed << std::setprecision(2)
                      << (comparison.strategy.total_return - comparison.buy_and_hold.total_return) * 100.0
                      << "%\n";
            return 0;
        }
        if (command != "download-bars") { usage(); return 1; }

        const char* feed_value = std::getenv("ALPACA_DATA_FEED");
        options::providers::alpaca::BarsRequest request;
        request.symbols = split_symbols(required(args, "symbols"));
        request.start = required(args, "start");
        request.end = required(args, "end");
        if (const auto it = args.find("timeframe"); it != args.end()) request.timeframe = it->second;
        request.feed = feed_value && *feed_value ? feed_value : "iex";

        const bool refresh = args.contains("refresh");
        std::map<std::pair<std::string, std::string>, std::vector<std::string>> work;
        for (const auto& symbol : request.symbols) {
            const options::data::DateRange requested{request.start, request.end};
            const auto ranges = refresh
                ? std::vector<options::data::DateRange>{requested}
                : store.missing_coverage(symbol, "alpaca", request.feed, request.timeframe,
                                         request.adjustment, requested);
            if (ranges.empty()) {
                std::cout << symbol << ": already cached, skipped\n";
                continue;
            }
            for (const auto& range : ranges) {
                work[{range.start, range.end}].push_back(symbol);
            }
        }
        std::size_t downloaded = 0;
        if (!work.empty()) {
            options::providers::alpaca::AlpacaClient client(
                {environment("ALPACA_API_KEY"), environment("ALPACA_API_SECRET")});
            for (const auto& [range, symbols] : work) {
                auto range_request = request;
                range_request.symbols = symbols;
                range_request.start = range.first;
                range_request.end = range.second;
                std::cout << "Downloading " << range.first << " through " << range.second
                          << " for ";
                for (std::size_t index = 0; index < symbols.size(); ++index) {
                    if (index) std::cout << ',';
                    std::cout << symbols[index];
                }
                std::cout << "\n";
                const auto bars = client.fetch_bars(range_request);
                store.upsert(bars, "alpaca", request.feed, request.timeframe, request.adjustment);
                for (const auto& symbol : symbols) {
                    store.record_coverage(symbol, "alpaca", request.feed, request.timeframe,
                                          request.adjustment, {range.first, range.second});
                }
                downloaded += bars.size();
            }
        }
        std::cout << "Downloaded and stored " << downloaded << " bars in " << db_path << "\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 2;
    }
}
