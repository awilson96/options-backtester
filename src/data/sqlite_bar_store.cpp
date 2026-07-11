#include "options/data/sqlite_bar_store.hpp"

#include <sqlite3.h>

#include <QDate>

#include <stdexcept>

namespace options::data {
namespace {

void check(int rc, sqlite3* db, const char* operation) {
    if (rc != SQLITE_OK && rc != SQLITE_DONE && rc != SQLITE_ROW) {
        throw std::runtime_error(std::string(operation) + ": " + sqlite3_errmsg(db));
    }
}

void exec(sqlite3* db, const char* sql) {
    char* error = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &error);
    if (rc != SQLITE_OK) {
        const std::string message = error ? error : "unknown SQLite error";
        sqlite3_free(error);
        throw std::runtime_error(message);
    }
}

QDate parse_date(const std::string& value) {
    const auto date = QDate::fromString(QString::fromStdString(value), Qt::ISODate);
    if (!date.isValid() || value.size() != 10) {
        throw std::invalid_argument("invalid date (expected YYYY-MM-DD): " + value);
    }
    return date;
}

std::string format_date(const QDate& value) {
    return value.toString(Qt::ISODate).toStdString();
}

}  // namespace

SqliteBarStore::SqliteBarStore(const std::filesystem::path& path) {
    check(sqlite3_open(path.string().c_str(), &db_), db_, "open database");
    try {
        exec(db_, "PRAGMA journal_mode=WAL; PRAGMA foreign_keys=ON; PRAGMA busy_timeout=5000;");
        migrate();
    } catch (...) {
        sqlite3_close(db_);
        db_ = nullptr;
        throw;
    }
}

SqliteBarStore::~SqliteBarStore() { sqlite3_close(db_); }

void SqliteBarStore::migrate() {
    exec(db_, R"sql(
        CREATE TABLE IF NOT EXISTS equity_bars (
            symbol TEXT NOT NULL,
            timestamp TEXT NOT NULL,
            open REAL NOT NULL,
            high REAL NOT NULL,
            low REAL NOT NULL,
            close REAL NOT NULL,
            volume INTEGER NOT NULL,
            trade_count INTEGER NOT NULL,
            vwap REAL NOT NULL,
            provider TEXT NOT NULL,
            feed TEXT NOT NULL,
            timeframe TEXT NOT NULL,
            adjustment TEXT NOT NULL,
            retrieved_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
            PRIMARY KEY (symbol, timestamp, provider, feed, timeframe, adjustment)
        );
        CREATE INDEX IF NOT EXISTS equity_bars_symbol_timestamp
            ON equity_bars(symbol, timestamp);
        CREATE TABLE IF NOT EXISTS equity_bar_coverage (
            symbol TEXT NOT NULL,
            provider TEXT NOT NULL,
            feed TEXT NOT NULL,
            timeframe TEXT NOT NULL,
            adjustment TEXT NOT NULL,
            start_date TEXT NOT NULL,
            end_date TEXT NOT NULL,
            completed_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
            PRIMARY KEY (symbol, provider, feed, timeframe, adjustment, start_date, end_date),
            CHECK (start_date <= end_date)
        );
        CREATE INDEX IF NOT EXISTS equity_bar_coverage_lookup
            ON equity_bar_coverage(symbol, provider, feed, timeframe, adjustment,
                                   start_date, end_date);
    )sql");
}

void SqliteBarStore::upsert(std::span<const Bar> bars, const std::string& provider,
                            const std::string& feed, const std::string& timeframe,
                            const std::string& adjustment) {
    static constexpr const char* sql = R"sql(
        INSERT INTO equity_bars
          (symbol,timestamp,open,high,low,close,volume,trade_count,vwap,provider,feed,timeframe,adjustment)
        VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)
        ON CONFLICT(symbol,timestamp,provider,feed,timeframe,adjustment) DO UPDATE SET
          open=excluded.open, high=excluded.high, low=excluded.low, close=excluded.close,
          volume=excluded.volume, trade_count=excluded.trade_count, vwap=excluded.vwap,
          retrieved_at=CURRENT_TIMESTAMP
    )sql";
    sqlite3_stmt* statement = nullptr;
    check(sqlite3_prepare_v2(db_, sql, -1, &statement, nullptr), db_, "prepare upsert");
    exec(db_, "BEGIN IMMEDIATE");
    try {
        for (const auto& bar : bars) {
            sqlite3_bind_text(statement, 1, bar.symbol.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(statement, 2, bar.timestamp.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_double(statement, 3, bar.open);
            sqlite3_bind_double(statement, 4, bar.high);
            sqlite3_bind_double(statement, 5, bar.low);
            sqlite3_bind_double(statement, 6, bar.close);
            sqlite3_bind_int64(statement, 7, bar.volume);
            sqlite3_bind_int64(statement, 8, bar.trade_count);
            sqlite3_bind_double(statement, 9, bar.vwap);
            sqlite3_bind_text(statement, 10, provider.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(statement, 11, feed.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(statement, 12, timeframe.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(statement, 13, adjustment.c_str(), -1, SQLITE_TRANSIENT);
            check(sqlite3_step(statement), db_, "upsert bar");
            sqlite3_reset(statement);
            sqlite3_clear_bindings(statement);
        }
        exec(db_, "COMMIT");
    } catch (...) {
        sqlite3_finalize(statement);
        exec(db_, "ROLLBACK");
        throw;
    }
    sqlite3_finalize(statement);
}

std::vector<Bar> SqliteBarStore::load(const std::string& symbol) const {
    static constexpr const char* sql = R"sql(
        SELECT symbol,timestamp,open,high,low,close,volume,trade_count,vwap
        FROM equity_bars WHERE symbol=? ORDER BY timestamp
    )sql";
    sqlite3_stmt* statement = nullptr;
    check(sqlite3_prepare_v2(db_, sql, -1, &statement, nullptr), db_, "prepare load");
    sqlite3_bind_text(statement, 1, symbol.c_str(), -1, SQLITE_TRANSIENT);
    std::vector<Bar> result;
    while (sqlite3_step(statement) == SQLITE_ROW) {
        result.push_back({
            reinterpret_cast<const char*>(sqlite3_column_text(statement, 0)),
            reinterpret_cast<const char*>(sqlite3_column_text(statement, 1)),
            sqlite3_column_double(statement, 2), sqlite3_column_double(statement, 3),
            sqlite3_column_double(statement, 4), sqlite3_column_double(statement, 5),
            sqlite3_column_int64(statement, 6), sqlite3_column_int64(statement, 7),
            sqlite3_column_double(statement, 8)});
    }
    sqlite3_finalize(statement);
    return result;
}

std::vector<Bar> SqliteBarStore::load(const BarQuery& query) const {
    static constexpr const char* sql = R"sql(
        SELECT symbol,timestamp,open,high,low,close,volume,trade_count,vwap
        FROM equity_bars
        WHERE symbol=? AND provider=? AND feed=? AND timeframe=? AND adjustment=?
          AND (?='' OR substr(timestamp,1,10)>=?)
          AND (?='' OR substr(timestamp,1,10)<=?)
        ORDER BY timestamp
    )sql";
    sqlite3_stmt* statement = nullptr;
    check(sqlite3_prepare_v2(db_, sql, -1, &statement, nullptr), db_, "prepare filtered load");
    const std::string values[]{query.symbol, query.provider, query.feed, query.timeframe,
                               query.adjustment, query.start, query.start, query.end, query.end};
    for (int index = 0; index < 9; ++index) {
        sqlite3_bind_text(statement, index + 1, values[index].c_str(), -1, SQLITE_TRANSIENT);
    }
    std::vector<Bar> result;
    while (sqlite3_step(statement) == SQLITE_ROW) {
        result.push_back({
            reinterpret_cast<const char*>(sqlite3_column_text(statement, 0)),
            reinterpret_cast<const char*>(sqlite3_column_text(statement, 1)),
            sqlite3_column_double(statement, 2), sqlite3_column_double(statement, 3),
            sqlite3_column_double(statement, 4), sqlite3_column_double(statement, 5),
            sqlite3_column_int64(statement, 6), sqlite3_column_int64(statement, 7),
            sqlite3_column_double(statement, 8)});
    }
    sqlite3_finalize(statement);
    return result;
}

std::int64_t SqliteBarStore::count(const std::string& symbol) const {
    sqlite3_stmt* statement = nullptr;
    check(sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM equity_bars WHERE symbol=?", -1,
                             &statement, nullptr), db_, "prepare count");
    sqlite3_bind_text(statement, 1, symbol.c_str(), -1, SQLITE_TRANSIENT);
    check(sqlite3_step(statement), db_, "count bars");
    const auto result = sqlite3_column_int64(statement, 0);
    sqlite3_finalize(statement);
    return result;
}

DateRange SqliteBarStore::date_range(const BarQuery& query) const {
    static constexpr const char* sql=R"sql(
      SELECT MIN(substr(timestamp,1,10)),MAX(substr(timestamp,1,10)) FROM equity_bars
      WHERE symbol=? AND provider=? AND feed=? AND timeframe=? AND adjustment=?
    )sql";
    sqlite3_stmt* statement=nullptr;
    check(sqlite3_prepare_v2(db_,sql,-1,&statement,nullptr),db_,"prepare bar date range");
    const std::string values[]{query.symbol,query.provider,query.feed,query.timeframe,query.adjustment};
    for(int index=0;index<5;++index)
        sqlite3_bind_text(statement,index+1,values[index].c_str(),-1,SQLITE_TRANSIENT);
    check(sqlite3_step(statement),db_,"read bar date range");
    DateRange result;
    if(sqlite3_column_type(statement,0)!=SQLITE_NULL)
        result.start=reinterpret_cast<const char*>(sqlite3_column_text(statement,0));
    if(sqlite3_column_type(statement,1)!=SQLITE_NULL)
        result.end=reinterpret_cast<const char*>(sqlite3_column_text(statement,1));
    sqlite3_finalize(statement); return result;
}

std::vector<std::string> SqliteBarStore::available_symbols(
    const std::string& provider, const std::string& feed,
    const std::string& timeframe, const std::string& adjustment) const {
    static constexpr const char* sql=R"sql(
      SELECT DISTINCT symbol FROM equity_bars
      WHERE provider=? AND feed=? AND timeframe=? AND adjustment=? ORDER BY symbol
    )sql";
    sqlite3_stmt* statement=nullptr;
    check(sqlite3_prepare_v2(db_,sql,-1,&statement,nullptr),db_,"prepare symbol list");
    const std::string values[]{provider,feed,timeframe,adjustment};
    for(int index=0;index<4;++index)
        sqlite3_bind_text(statement,index+1,values[index].c_str(),-1,SQLITE_TRANSIENT);
    std::vector<std::string> result;
    while(sqlite3_step(statement)==SQLITE_ROW)
        result.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(statement,0)));
    sqlite3_finalize(statement); return result;
}

std::vector<DateRange> SqliteBarStore::missing_coverage(
    const std::string& symbol, const std::string& provider, const std::string& feed,
    const std::string& timeframe, const std::string& adjustment,
    const DateRange& requested) const {
    const auto requested_start = parse_date(requested.start);
    const auto requested_end = parse_date(requested.end);
    if (requested_start > requested_end) throw std::invalid_argument("start date is after end date");

    static constexpr const char* sql = R"sql(
        SELECT start_date,end_date FROM equity_bar_coverage
        WHERE symbol=? AND provider=? AND feed=? AND timeframe=? AND adjustment=?
          AND end_date>=? AND start_date<=?
        ORDER BY start_date,end_date
    )sql";
    sqlite3_stmt* statement = nullptr;
    check(sqlite3_prepare_v2(db_, sql, -1, &statement, nullptr), db_, "prepare coverage lookup");
    const std::string values[]{symbol, provider, feed, timeframe, adjustment,
                               requested.start, requested.end};
    for (int index = 0; index < 7; ++index) {
        sqlite3_bind_text(statement, index + 1, values[index].c_str(), -1, SQLITE_TRANSIENT);
    }

    std::vector<DateRange> missing;
    auto cursor = requested_start;
    while (sqlite3_step(statement) == SQLITE_ROW) {
        auto covered_start = parse_date(
            reinterpret_cast<const char*>(sqlite3_column_text(statement, 0)));
        auto covered_end = parse_date(
            reinterpret_cast<const char*>(sqlite3_column_text(statement, 1)));
        if (covered_start < requested_start) covered_start = requested_start;
        if (covered_end > requested_end) covered_end = requested_end;
        if (covered_start > cursor) {
            missing.push_back({format_date(cursor), format_date(covered_start.addDays(-1))});
        }
        if (covered_end >= cursor) cursor = covered_end.addDays(1);
        if (cursor > requested_end) break;
    }
    sqlite3_finalize(statement);
    if (cursor <= requested_end) missing.push_back({format_date(cursor), format_date(requested_end)});
    return missing;
}

void SqliteBarStore::record_coverage(const std::string& symbol, const std::string& provider,
                                     const std::string& feed, const std::string& timeframe,
                                     const std::string& adjustment, const DateRange& covered) {
    const auto start = parse_date(covered.start);
    const auto end = parse_date(covered.end);
    if (start > end) throw std::invalid_argument("coverage start date is after end date");
    static constexpr const char* sql = R"sql(
        INSERT INTO equity_bar_coverage
          (symbol,provider,feed,timeframe,adjustment,start_date,end_date)
        VALUES (?,?,?,?,?,?,?)
        ON CONFLICT(symbol,provider,feed,timeframe,adjustment,start_date,end_date)
        DO UPDATE SET completed_at=CURRENT_TIMESTAMP
    )sql";
    sqlite3_stmt* statement = nullptr;
    check(sqlite3_prepare_v2(db_, sql, -1, &statement, nullptr), db_, "prepare coverage insert");
    const std::string values[]{symbol, provider, feed, timeframe, adjustment,
                               covered.start, covered.end};
    for (int index = 0; index < 7; ++index) {
        sqlite3_bind_text(statement, index + 1, values[index].c_str(), -1, SQLITE_TRANSIENT);
    }
    check(sqlite3_step(statement), db_, "record coverage");
    sqlite3_finalize(statement);
}

}  // namespace options::data
