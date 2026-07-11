#include "options/data/sqlite_option_store.hpp"

#include <sqlite3.h>

#include <optional>
#include <stdexcept>

namespace options::data {
namespace {

void check(int rc, sqlite3* db, const char* operation) {
    if (rc != SQLITE_OK && rc != SQLITE_DONE && rc != SQLITE_ROW)
        throw std::runtime_error(std::string(operation) + ": " + sqlite3_errmsg(db));
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

bool has_column(sqlite3* db, const char* table, const char* column) {
    sqlite3_stmt* statement=nullptr;
    const std::string sql=std::string("PRAGMA table_info(")+table+")";
    check(sqlite3_prepare_v2(db,sql.c_str(),-1,&statement,nullptr),db,"inspect schema");
    bool found=false;
    while(sqlite3_step(statement)==SQLITE_ROW) {
        const auto* name=reinterpret_cast<const char*>(sqlite3_column_text(statement,1));
        if(name && std::string(name)==column) { found=true; break; }
    }
    sqlite3_finalize(statement); return found;
}

void bind_text(sqlite3_stmt* statement, int index, const std::string& value) {
    sqlite3_bind_text(statement, index, value.c_str(), -1, SQLITE_TRANSIENT);
}

template <typename T>
void bind_number(sqlite3_stmt* statement, int index, const std::optional<T>& value) {
    if (!value) sqlite3_bind_null(statement, index);
    else if constexpr (std::is_integral_v<T>) sqlite3_bind_int64(statement, index, *value);
    else sqlite3_bind_double(statement, index, *value);
}

std::int64_t count(sqlite3* db, const char* sql, const std::string& value) {
    sqlite3_stmt* statement = nullptr;
    check(sqlite3_prepare_v2(db, sql, -1, &statement, nullptr), db, "prepare count");
    bind_text(statement, 1, value);
    check(sqlite3_step(statement), db, "count rows");
    const auto result = sqlite3_column_int64(statement, 0);
    sqlite3_finalize(statement);
    return result;
}

}  // namespace

SqliteOptionStore::SqliteOptionStore(const std::filesystem::path& path) {
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

SqliteOptionStore::~SqliteOptionStore() { sqlite3_close(db_); }

void SqliteOptionStore::migrate() {
    exec(db_, R"sql(
      CREATE TABLE IF NOT EXISTS option_contracts (
        symbol TEXT PRIMARY KEY, id TEXT, name TEXT, underlying_symbol TEXT NOT NULL,
        root_symbol TEXT, expiration_date TEXT NOT NULL, type TEXT NOT NULL, style TEXT,
        status TEXT, strike_price REAL NOT NULL, multiplier INTEGER NOT NULL,
        tradable INTEGER NOT NULL, open_interest INTEGER, open_interest_date TEXT,
        close_price REAL, close_price_date TEXT, retrieved_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
      );
      CREATE INDEX IF NOT EXISTS option_contracts_underlying_expiration
        ON option_contracts(underlying_symbol, expiration_date, strike_price);
      CREATE TABLE IF NOT EXISTS option_bars (
        symbol TEXT NOT NULL, timestamp TEXT NOT NULL, open REAL NOT NULL, high REAL NOT NULL,
        low REAL NOT NULL, close REAL NOT NULL, volume INTEGER NOT NULL,
        trade_count INTEGER NOT NULL, vwap REAL NOT NULL, feed TEXT NOT NULL,
        timeframe TEXT NOT NULL, retrieved_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
        PRIMARY KEY(symbol,timestamp,feed,timeframe)
      );
      CREATE TABLE IF NOT EXISTS option_quotes (
        symbol TEXT NOT NULL, quote_timestamp TEXT NOT NULL, bid_price REAL NOT NULL,
        ask_price REAL NOT NULL, bid_size INTEGER NOT NULL, ask_size INTEGER NOT NULL,
        bid_exchange TEXT, ask_exchange TEXT, feed TEXT NOT NULL, observed_at TEXT NOT NULL,
        PRIMARY KEY(symbol,quote_timestamp,feed)
      );
      CREATE TABLE IF NOT EXISTS option_snapshots (
        symbol TEXT NOT NULL, observation_date TEXT NOT NULL, observed_at TEXT NOT NULL,
        feed TEXT NOT NULL, quote_timestamp TEXT, bid_price REAL, ask_price REAL,
        bid_size INTEGER, ask_size INTEGER, trade_timestamp TEXT, trade_price REAL,
        trade_size INTEGER, implied_volatility REAL, delta REAL, gamma REAL, theta REAL,
        vega REAL, rho REAL, PRIMARY KEY(symbol,observation_date,feed)
      );
      CREATE INDEX IF NOT EXISTS option_snapshots_date ON option_snapshots(observation_date);
      CREATE TABLE IF NOT EXISTS option_snapshot_observations (
        symbol TEXT NOT NULL, observed_at TEXT NOT NULL, market_date TEXT NOT NULL,
        feed TEXT NOT NULL, quote_timestamp TEXT, bid_price REAL, ask_price REAL,
        bid_size INTEGER, ask_size INTEGER, trade_timestamp TEXT, trade_price REAL,
        trade_size INTEGER, implied_volatility REAL, delta REAL, gamma REAL, theta REAL,
        vega REAL, rho REAL, open_interest INTEGER, open_interest_date TEXT,
        PRIMARY KEY(symbol,observed_at,feed)
      );
      CREATE INDEX IF NOT EXISTS option_snapshot_observations_date
        ON option_snapshot_observations(market_date,symbol);
    )sql");
    if(!has_column(db_,"option_snapshots","open_interest"))
        exec(db_,"ALTER TABLE option_snapshots ADD COLUMN open_interest INTEGER");
    if(!has_column(db_,"option_snapshots","open_interest_date"))
        exec(db_,"ALTER TABLE option_snapshots ADD COLUMN open_interest_date TEXT");
}

void SqliteOptionStore::upsert_contracts(std::span<const OptionContract> contracts) {
    static constexpr const char* sql = R"sql(
      INSERT INTO option_contracts
       (symbol,id,name,underlying_symbol,root_symbol,expiration_date,type,style,status,
        strike_price,multiplier,tradable,open_interest,open_interest_date,close_price,close_price_date)
      VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
      ON CONFLICT(symbol) DO UPDATE SET id=excluded.id,name=excluded.name,
       underlying_symbol=excluded.underlying_symbol,root_symbol=excluded.root_symbol,
       expiration_date=excluded.expiration_date,type=excluded.type,style=excluded.style,
       status=excluded.status,strike_price=excluded.strike_price,multiplier=excluded.multiplier,
       tradable=excluded.tradable,open_interest=excluded.open_interest,
       open_interest_date=excluded.open_interest_date,close_price=excluded.close_price,
       close_price_date=excluded.close_price_date,retrieved_at=CURRENT_TIMESTAMP
    )sql";
    sqlite3_stmt* statement = nullptr;
    check(sqlite3_prepare_v2(db_, sql, -1, &statement, nullptr), db_, "prepare contract upsert");
    exec(db_, "BEGIN IMMEDIATE");
    try {
        for (const auto& c : contracts) {
            const std::string texts[]{c.symbol,c.id,c.name,c.underlying_symbol,c.root_symbol,
                                      c.expiration_date,c.type,c.style,c.status};
            for (int i=0;i<9;++i) bind_text(statement,i+1,texts[i]);
            sqlite3_bind_double(statement,10,c.strike_price);
            sqlite3_bind_int64(statement,11,c.multiplier);
            sqlite3_bind_int(statement,12,c.tradable ? 1 : 0);
            bind_number(statement,13,c.open_interest);
            bind_text(statement,14,c.open_interest_date);
            bind_number(statement,15,c.close_price);
            bind_text(statement,16,c.close_price_date);
            check(sqlite3_step(statement),db_,"upsert contract");
            sqlite3_reset(statement); sqlite3_clear_bindings(statement);
        }
        exec(db_,"COMMIT");
    } catch (...) { sqlite3_finalize(statement); exec(db_,"ROLLBACK"); throw; }
    sqlite3_finalize(statement);
}

void SqliteOptionStore::upsert_bars(std::span<const Bar> bars, const std::string& feed,
                                    const std::string& timeframe) {
    static constexpr const char* sql = R"sql(
      INSERT INTO option_bars(symbol,timestamp,open,high,low,close,volume,trade_count,vwap,feed,timeframe)
      VALUES(?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(symbol,timestamp,feed,timeframe) DO UPDATE SET
       open=excluded.open,high=excluded.high,low=excluded.low,close=excluded.close,
       volume=excluded.volume,trade_count=excluded.trade_count,vwap=excluded.vwap,
       retrieved_at=CURRENT_TIMESTAMP
    )sql";
    sqlite3_stmt* s=nullptr; check(sqlite3_prepare_v2(db_,sql,-1,&s,nullptr),db_,"prepare option bars");
    exec(db_,"BEGIN IMMEDIATE");
    try { for (const auto& b:bars) {
        bind_text(s,1,b.symbol); bind_text(s,2,b.timestamp); sqlite3_bind_double(s,3,b.open);
        sqlite3_bind_double(s,4,b.high); sqlite3_bind_double(s,5,b.low); sqlite3_bind_double(s,6,b.close);
        sqlite3_bind_int64(s,7,b.volume); sqlite3_bind_int64(s,8,b.trade_count); sqlite3_bind_double(s,9,b.vwap);
        bind_text(s,10,feed); bind_text(s,11,timeframe); check(sqlite3_step(s),db_,"upsert option bar");
        sqlite3_reset(s); sqlite3_clear_bindings(s);
    } exec(db_,"COMMIT"); } catch (...) { sqlite3_finalize(s); exec(db_,"ROLLBACK"); throw; }
    sqlite3_finalize(s);
}

void SqliteOptionStore::upsert_quotes(std::span<const OptionQuote> quotes, const std::string& feed,
                                      const std::string& observed_at) {
    static constexpr const char* sql = R"sql(
      INSERT INTO option_quotes(symbol,quote_timestamp,bid_price,ask_price,bid_size,ask_size,
       bid_exchange,ask_exchange,feed,observed_at) VALUES(?,?,?,?,?,?,?,?,?,?)
      ON CONFLICT(symbol,quote_timestamp,feed) DO UPDATE SET bid_price=excluded.bid_price,
       ask_price=excluded.ask_price,bid_size=excluded.bid_size,ask_size=excluded.ask_size,
       bid_exchange=excluded.bid_exchange,ask_exchange=excluded.ask_exchange,observed_at=excluded.observed_at
    )sql";
    sqlite3_stmt* s=nullptr; check(sqlite3_prepare_v2(db_,sql,-1,&s,nullptr),db_,"prepare option quotes");
    exec(db_,"BEGIN IMMEDIATE");
    try { for (const auto& q:quotes) {
        bind_text(s,1,q.symbol); bind_text(s,2,q.timestamp); sqlite3_bind_double(s,3,q.bid_price);
        sqlite3_bind_double(s,4,q.ask_price); sqlite3_bind_int64(s,5,q.bid_size); sqlite3_bind_int64(s,6,q.ask_size);
        bind_text(s,7,q.bid_exchange); bind_text(s,8,q.ask_exchange); bind_text(s,9,feed); bind_text(s,10,observed_at);
        check(sqlite3_step(s),db_,"upsert option quote"); sqlite3_reset(s); sqlite3_clear_bindings(s);
    } exec(db_,"COMMIT"); } catch (...) { sqlite3_finalize(s); exec(db_,"ROLLBACK"); throw; }
    sqlite3_finalize(s);
}

void SqliteOptionStore::upsert_snapshots(std::span<const OptionSnapshot> snapshots,
                                         const std::string& feed) {
    static constexpr const char* sql = R"sql(
      INSERT INTO option_snapshots(symbol,observation_date,observed_at,feed,quote_timestamp,
       bid_price,ask_price,bid_size,ask_size,trade_timestamp,trade_price,trade_size,
       implied_volatility,delta,gamma,theta,vega,rho,open_interest,open_interest_date)
       VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,
        (SELECT open_interest FROM option_contracts WHERE symbol=?),
        (SELECT open_interest_date FROM option_contracts WHERE symbol=?))
      ON CONFLICT(symbol,observation_date,feed) DO UPDATE SET observed_at=excluded.observed_at,
       quote_timestamp=excluded.quote_timestamp,bid_price=excluded.bid_price,ask_price=excluded.ask_price,
       bid_size=excluded.bid_size,ask_size=excluded.ask_size,trade_timestamp=excluded.trade_timestamp,
       trade_price=excluded.trade_price,trade_size=excluded.trade_size,
       implied_volatility=excluded.implied_volatility,delta=excluded.delta,gamma=excluded.gamma,
       theta=excluded.theta,vega=excluded.vega,rho=excluded.rho,
       open_interest=excluded.open_interest,open_interest_date=excluded.open_interest_date
    )sql";
    sqlite3_stmt* s=nullptr; check(sqlite3_prepare_v2(db_,sql,-1,&s,nullptr),db_,"prepare snapshots");
    exec(db_,"BEGIN IMMEDIATE");
    try { for (const auto& x:snapshots) {
        const std::string observation_date=x.latest_quote && x.latest_quote->timestamp.size()>=10
            ? x.latest_quote->timestamp.substr(0,10)
            : (x.trade_timestamp.size()>=10 ? x.trade_timestamp.substr(0,10) : x.observed_at.substr(0,10));
        bind_text(s,1,x.symbol); bind_text(s,2,observation_date); bind_text(s,3,x.observed_at); bind_text(s,4,feed);
        if (x.latest_quote) { bind_text(s,5,x.latest_quote->timestamp); sqlite3_bind_double(s,6,x.latest_quote->bid_price);
            sqlite3_bind_double(s,7,x.latest_quote->ask_price); sqlite3_bind_int64(s,8,x.latest_quote->bid_size);
            sqlite3_bind_int64(s,9,x.latest_quote->ask_size); }
        else { for(int i=5;i<=9;++i) sqlite3_bind_null(s,i); }
        bind_text(s,10,x.trade_timestamp); bind_number(s,11,x.trade_price); bind_number(s,12,x.trade_size);
        bind_number(s,13,x.implied_volatility); bind_number(s,14,x.delta); bind_number(s,15,x.gamma);
        bind_number(s,16,x.theta); bind_number(s,17,x.vega); bind_number(s,18,x.rho);
        bind_text(s,19,x.symbol); bind_text(s,20,x.symbol);
        check(sqlite3_step(s),db_,"upsert snapshot"); sqlite3_reset(s); sqlite3_clear_bindings(s);
    } exec(db_,"COMMIT"); } catch (...) { sqlite3_finalize(s); exec(db_,"ROLLBACK"); throw; }
    sqlite3_finalize(s);
}

void SqliteOptionStore::insert_snapshot_observations(
    std::span<const OptionSnapshot> snapshots,const std::string& feed) {
    static constexpr const char* sql=R"sql(
      INSERT INTO option_snapshot_observations(symbol,observed_at,market_date,feed,
       quote_timestamp,bid_price,ask_price,bid_size,ask_size,trade_timestamp,trade_price,
       trade_size,implied_volatility,delta,gamma,theta,vega,rho,open_interest,open_interest_date)
      VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,
       (SELECT open_interest FROM option_contracts WHERE symbol=?),
       (SELECT open_interest_date FROM option_contracts WHERE symbol=?))
      ON CONFLICT(symbol,observed_at,feed) DO NOTHING
    )sql";
    sqlite3_stmt* s=nullptr;
    check(sqlite3_prepare_v2(db_,sql,-1,&s,nullptr),db_,"prepare snapshot observations");
    exec(db_,"BEGIN IMMEDIATE");
    try { for(const auto& x:snapshots) {
        const std::string market_date=x.latest_quote && x.latest_quote->timestamp.size()>=10
            ? x.latest_quote->timestamp.substr(0,10)
            : (x.trade_timestamp.size()>=10 ? x.trade_timestamp.substr(0,10) : x.observed_at.substr(0,10));
        bind_text(s,1,x.symbol); bind_text(s,2,x.observed_at); bind_text(s,3,market_date); bind_text(s,4,feed);
        if(x.latest_quote) { bind_text(s,5,x.latest_quote->timestamp); sqlite3_bind_double(s,6,x.latest_quote->bid_price);
            sqlite3_bind_double(s,7,x.latest_quote->ask_price); sqlite3_bind_int64(s,8,x.latest_quote->bid_size);
            sqlite3_bind_int64(s,9,x.latest_quote->ask_size); }
        else { for(int i=5;i<=9;++i) sqlite3_bind_null(s,i); }
        bind_text(s,10,x.trade_timestamp); bind_number(s,11,x.trade_price); bind_number(s,12,x.trade_size);
        bind_number(s,13,x.implied_volatility); bind_number(s,14,x.delta); bind_number(s,15,x.gamma);
        bind_number(s,16,x.theta); bind_number(s,17,x.vega); bind_number(s,18,x.rho);
        bind_text(s,19,x.symbol); bind_text(s,20,x.symbol);
        check(sqlite3_step(s),db_,"insert snapshot observation");
        sqlite3_reset(s); sqlite3_clear_bindings(s);
    } exec(db_,"COMMIT"); }
    catch(...) { sqlite3_finalize(s); exec(db_,"ROLLBACK"); throw; }
    sqlite3_finalize(s);
}

std::int64_t SqliteOptionStore::contract_count(const std::string& underlying) const {
    return count(db_,"SELECT COUNT(*) FROM option_contracts WHERE underlying_symbol=?",underlying);
}
std::int64_t SqliteOptionStore::bar_count(const std::string& symbol) const {
    return count(db_,"SELECT COUNT(*) FROM option_bars WHERE symbol=?",symbol);
}
std::int64_t SqliteOptionStore::quote_count(const std::string& symbol) const {
    return count(db_,"SELECT COUNT(*) FROM option_quotes WHERE symbol=?",symbol);
}
std::int64_t SqliteOptionStore::snapshot_count(const std::string& underlying) const {
    return count(db_,R"sql(SELECT COUNT(*) FROM option_snapshots s JOIN option_contracts c
      ON c.symbol=s.symbol WHERE c.underlying_symbol=?)sql",underlying);
}
std::int64_t SqliteOptionStore::snapshot_observation_count(const std::string& underlying) const {
    return count(db_,R"sql(SELECT COUNT(*) FROM option_snapshot_observations s
      JOIN option_contracts c ON c.symbol=s.symbol WHERE c.underlying_symbol=?)sql",underlying);
}

std::vector<OptionContract> SqliteOptionStore::load_contracts(const std::string& underlying,
                                                               std::int64_t limit) const {
    static constexpr const char* sql = R"sql(
      SELECT symbol,expiration_date,type,strike_price,open_interest,open_interest_date,
             close_price,close_price_date,status,tradable,multiplier
      FROM option_contracts WHERE underlying_symbol=?
      ORDER BY expiration_date,strike_price,type LIMIT ?
    )sql";
    sqlite3_stmt* s=nullptr;
    check(sqlite3_prepare_v2(db_,sql,-1,&s,nullptr),db_,"prepare contract list");
    bind_text(s,1,underlying); sqlite3_bind_int64(s,2,limit);
    std::vector<OptionContract> result;
    while (sqlite3_step(s)==SQLITE_ROW) {
        OptionContract c; c.symbol=reinterpret_cast<const char*>(sqlite3_column_text(s,0));
        c.underlying_symbol=underlying;
        c.expiration_date=reinterpret_cast<const char*>(sqlite3_column_text(s,1));
        c.type=reinterpret_cast<const char*>(sqlite3_column_text(s,2));
        c.strike_price=sqlite3_column_double(s,3);
        if (sqlite3_column_type(s,4)!=SQLITE_NULL) c.open_interest=sqlite3_column_int64(s,4);
        if (sqlite3_column_type(s,5)!=SQLITE_NULL) c.open_interest_date=reinterpret_cast<const char*>(sqlite3_column_text(s,5));
        if (sqlite3_column_type(s,6)!=SQLITE_NULL) c.close_price=sqlite3_column_double(s,6);
        if (sqlite3_column_type(s,7)!=SQLITE_NULL) c.close_price_date=reinterpret_cast<const char*>(sqlite3_column_text(s,7));
        if (sqlite3_column_type(s,8)!=SQLITE_NULL) c.status=reinterpret_cast<const char*>(sqlite3_column_text(s,8));
        c.tradable=sqlite3_column_int(s,9)!=0; c.multiplier=sqlite3_column_int64(s,10);
        result.push_back(std::move(c));
    }
    sqlite3_finalize(s); return result;
}

std::vector<OptionObservation> SqliteOptionStore::load_snapshot_history(
    const std::string& underlying, const std::string& feed, const std::string& start,
    const std::string& end) const {
    static constexpr const char* sql = R"sql(
      SELECT s.symbol,s.observed_at,
       substr(COALESCE(NULLIF(s.quote_timestamp,''),NULLIF(s.trade_timestamp,''),s.observed_at),1,10),
       s.quote_timestamp,s.bid_price,s.ask_price,s.bid_size,
       s.ask_size,s.trade_timestamp,s.trade_price,s.trade_size,s.implied_volatility,
       s.delta,s.gamma,s.theta,s.vega,s.rho,c.expiration_date,c.type,c.strike_price,
       c.multiplier,s.open_interest
      FROM option_snapshots s JOIN option_contracts c ON c.symbol=s.symbol
      WHERE c.underlying_symbol=? AND s.feed=?
       AND substr(COALESCE(NULLIF(s.quote_timestamp,''),NULLIF(s.trade_timestamp,''),s.observed_at),1,10)>=?
       AND substr(COALESCE(NULLIF(s.quote_timestamp,''),NULLIF(s.trade_timestamp,''),s.observed_at),1,10)<=?
      ORDER BY 3,s.symbol
    )sql";
    sqlite3_stmt* s=nullptr; check(sqlite3_prepare_v2(db_,sql,-1,&s,nullptr),db_,"prepare snapshot history");
    bind_text(s,1,underlying); bind_text(s,2,feed); bind_text(s,3,start); bind_text(s,4,end);
    std::vector<OptionObservation> result;
    while (sqlite3_step(s)==SQLITE_ROW) {
        OptionObservation o; o.underlying_symbol=underlying;
        o.snapshot.symbol=reinterpret_cast<const char*>(sqlite3_column_text(s,0));
        o.snapshot.observed_at=reinterpret_cast<const char*>(sqlite3_column_text(s,1));
        o.observation_date=reinterpret_cast<const char*>(sqlite3_column_text(s,2));
        if (sqlite3_column_type(s,3)!=SQLITE_NULL) {
            OptionQuote q; q.symbol=o.snapshot.symbol;
            q.timestamp=reinterpret_cast<const char*>(sqlite3_column_text(s,3));
            q.bid_price=sqlite3_column_double(s,4); q.ask_price=sqlite3_column_double(s,5);
            q.bid_size=sqlite3_column_int64(s,6); q.ask_size=sqlite3_column_int64(s,7);
            o.snapshot.latest_quote=std::move(q);
        }
        if (sqlite3_column_type(s,8)!=SQLITE_NULL) o.snapshot.trade_timestamp=reinterpret_cast<const char*>(sqlite3_column_text(s,8));
        if (sqlite3_column_type(s,9)!=SQLITE_NULL) o.snapshot.trade_price=sqlite3_column_double(s,9);
        if (sqlite3_column_type(s,10)!=SQLITE_NULL) o.snapshot.trade_size=sqlite3_column_int64(s,10);
        auto set_double=[s](int column,std::optional<double>& target) { if(sqlite3_column_type(s,column)!=SQLITE_NULL) target=sqlite3_column_double(s,column); };
        set_double(11,o.snapshot.implied_volatility); set_double(12,o.snapshot.delta);
        set_double(13,o.snapshot.gamma); set_double(14,o.snapshot.theta);
        set_double(15,o.snapshot.vega); set_double(16,o.snapshot.rho);
        o.expiration_date=reinterpret_cast<const char*>(sqlite3_column_text(s,17));
        o.type=reinterpret_cast<const char*>(sqlite3_column_text(s,18));
        o.strike_price=sqlite3_column_double(s,19); o.multiplier=sqlite3_column_int64(s,20);
        if(sqlite3_column_type(s,21)!=SQLITE_NULL) o.open_interest=sqlite3_column_int64(s,21);
        result.push_back(std::move(o));
    }
    sqlite3_finalize(s); return result;
}

std::vector<std::string> SqliteOptionStore::available_underlyings() const {
    static constexpr const char* sql=R"sql(
      SELECT DISTINCT c.underlying_symbol FROM option_snapshots s
      JOIN option_contracts c ON c.symbol=s.symbol ORDER BY c.underlying_symbol
    )sql";
    sqlite3_stmt* s=nullptr; check(sqlite3_prepare_v2(db_,sql,-1,&s,nullptr),db_,"list underlyings");
    std::vector<std::string> result;
    while(sqlite3_step(s)==SQLITE_ROW)
        result.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(s,0)));
    sqlite3_finalize(s); return result;
}

std::vector<std::string> SqliteOptionStore::available_snapshot_feeds(
    const std::string& underlying) const {
    static constexpr const char* sql=R"sql(
      SELECT DISTINCT s.feed FROM option_snapshots s JOIN option_contracts c ON c.symbol=s.symbol
      WHERE c.underlying_symbol=? ORDER BY s.feed
    )sql";
    sqlite3_stmt* s=nullptr; check(sqlite3_prepare_v2(db_,sql,-1,&s,nullptr),db_,"list snapshot feeds");
    bind_text(s,1,underlying); std::vector<std::string> result;
    while(sqlite3_step(s)==SQLITE_ROW)
        result.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(s,0)));
    sqlite3_finalize(s); return result;
}

std::pair<std::string,std::string> SqliteOptionStore::snapshot_date_range(
    const std::string& underlying, const std::string& feed) const {
    static constexpr const char* sql=R"sql(
      SELECT MIN(substr(COALESCE(NULLIF(s.quote_timestamp,''),NULLIF(s.trade_timestamp,''),s.observed_at),1,10)),
             MAX(substr(COALESCE(NULLIF(s.quote_timestamp,''),NULLIF(s.trade_timestamp,''),s.observed_at),1,10))
      FROM option_snapshots s JOIN option_contracts c ON c.symbol=s.symbol
      WHERE c.underlying_symbol=? AND s.feed=?
    )sql";
    sqlite3_stmt* s=nullptr; check(sqlite3_prepare_v2(db_,sql,-1,&s,nullptr),db_,"snapshot date range");
    bind_text(s,1,underlying); bind_text(s,2,feed); check(sqlite3_step(s),db_,"read snapshot date range");
    std::pair<std::string,std::string> result;
    if(sqlite3_column_type(s,0)!=SQLITE_NULL) result.first=reinterpret_cast<const char*>(sqlite3_column_text(s,0));
    if(sqlite3_column_type(s,1)!=SQLITE_NULL) result.second=reinterpret_cast<const char*>(sqlite3_column_text(s,1));
    sqlite3_finalize(s); return result;
}

}  // namespace options::data
