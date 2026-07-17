#include "options/analysis/momentum.hpp"
#include "options/analysis/intraday_distribution.hpp"
#include "options/data/sqlite_bar_store.hpp"
#include "options/providers/alpaca/alpaca_client.hpp"

#include <QApplication>
#include <QAbstractTableModel>
#include <QBrush>
#include <QChart>
#include <QChartView>
#include <QCandlestickSeries>
#include <QCandlestickSet>
#include <QBarCategoryAxis>
#include <QCategoryAxis>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDateEdit>
#include <QDateTimeAxis>
#include <QDataStream>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QEnterEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QDir>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QLineSeries>
#include <QLegend>
#include <QMainWindow>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QProgressBar>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSaveFile>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableView>
#include <QThread>
#include <QTimer>
#include <QTimeZone>
#include <QStandardPaths>
#include <QValueAxis>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QUuid>
#include <QtConcurrentRun>

#include <cstdlib>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <map>
#include <mutex>
#include <numeric>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace {

struct LoadResult {
    QString symbol;
    QString database_path;
    std::size_t downloaded{};
    bool cached{};
    QString error;
};

struct CachedIntradayDistributionStudy {
    options::analysis::IntradayDistributionResult result;
    std::array<options::analysis::IntradayWeekdayAggregation,5> weekdays;
    options::analysis::IntradayWeeklySummary weekly_summary;
};

struct IntradayDistributionOutput {
    std::shared_ptr<CachedIntradayDistributionStudy> study;
    QString cache_key;
    bool cached{};
    QString error;
};

struct MomentumStudyRow {
    int window_days{};
    int skip_days{};
    int strike_offset{};
    options::analysis::MomentumResult result;
    std::optional<options::analysis::StrikeAdjustment> strike_adjustment;
    std::optional<options::analysis::SimulatedPricing> simulated_pricing;
    double drop_rate{};
};

struct MomentumStudyRequest {
    int window_days{};
    int skip_days{};
    int strike_offset{};
    std::optional<options::analysis::StrikeAdjustment> strike_adjustment;
    std::optional<options::analysis::SimulatedPricing> simulated_pricing;
};

struct MomentumAnalysisOutput {
    QString symbol;
    bool parametric{};
    bool strike_enabled{};
    bool simulated_pricing{};
    int window_days{};
    int skip_days{};
    int strike_offset{};
    double strike_width{};
    double strike_resolution{};
    std::vector<MomentumStudyRow> study_rows;
    std::optional<options::analysis::MomentumResult> single_result;
    QString error;
    QString saved_result_key;
    QString settings_signature;
    bool loaded_saved_result{};
    bool open_profit_chart{};
    bool saved_result_missing{};
};

struct MomentumLedgerOutput {
    std::size_t row_index{};
    QString title;
    options::analysis::MomentumResult result;
    std::vector<options::data::Bar> bars;
    QString error;
};

struct AnalysisProgress {
    std::atomic<std::size_t> completed{};
    std::atomic<int> phase{}; // 0 opening, 1 calculation, 2 saving, 3 parallel load
    std::atomic<std::size_t> total{};
};

struct MomentumStudyPreset {
    QString id;
    QString name;
    QString symbol;
    bool parametric{};
    int window_days{30};
    int skip_days{1};
    int bar_minutes{1};
    QString price_field{"vwap"};
    double drop_rate{};
    bool strike_enabled{};
    double strike_width{5.0};
    double strike_resolution{1.0};
    int strike_offset{};
    bool simulated_pricing{};
    double allocation{10000.0};
    int slippage_mode{};
    double buy_slippage{0.04};
    double sell_slippage{0.04};
    QDate analysis_start;
    QDate analysis_end;
    int window_minimum{1};
    int window_maximum{30};
    int skip_minimum{1};
    int skip_maximum{30};
    int strike_minimum{-1};
    int strike_maximum{1};
    std::map<int,std::pair<double,double>> pricing;
    QString saved_result_key;
};

std::vector<MomentumStudyPreset> load_study_presets() {
    QSettings settings("OptionsBacktester","OptionsBacktester");
    settings.beginGroup("saved_studies");
    std::vector<MomentumStudyPreset> presets;
    for(const auto& id:settings.childGroups()) {
        settings.beginGroup(id);
        if(settings.value("strategy","momentum").toString()=="momentum") {
            MomentumStudyPreset value;
            value.id=id;
            value.name=settings.value("name","Unnamed study").toString();
            value.symbol=settings.value("symbol").toString();
            value.parametric=settings.value("parametric",false).toBool();
            value.window_days=settings.value("window_days",30).toInt();
            value.skip_days=settings.value("skip_days",1).toInt();
            value.bar_minutes=settings.value("bar_minutes",1).toInt();
            value.price_field=settings.value("price_field","vwap").toString();
            value.drop_rate=settings.value("drop_rate",0.0).toDouble();
            value.strike_enabled=settings.value("strike_enabled",false).toBool();
            value.strike_width=settings.value("strike_width",5.0).toDouble();
            value.strike_resolution=settings.value(
                "strike_resolution",value.strike_width).toDouble();
            value.strike_offset=settings.value("strike_offset",0).toInt();
            value.simulated_pricing=settings.value("simulated_pricing",false).toBool();
            value.allocation=settings.value("allocation",10000.0).toDouble();
            value.slippage_mode=settings.value("slippage_mode",0).toInt();
            value.buy_slippage=settings.value("buy_slippage",0.04).toDouble();
            value.sell_slippage=settings.value("sell_slippage",0.04).toDouble();
            value.analysis_start=QDate::fromString(settings.value("analysis_start").toString(),Qt::ISODate);
            value.analysis_end=QDate::fromString(settings.value("analysis_end").toString(),Qt::ISODate);
            value.window_minimum=settings.value("window_minimum",1).toInt();
            value.window_maximum=settings.value("window_maximum",30).toInt();
            value.skip_minimum=settings.value("skip_minimum",1).toInt();
            value.skip_maximum=settings.value("skip_maximum",30).toInt();
            value.strike_minimum=settings.value("strike_minimum",-1).toInt();
            value.strike_maximum=settings.value("strike_maximum",1).toInt();
            value.saved_result_key=settings.value("saved_result_key",
                settings.value("result_cache_key")).toString();
            const auto pricing_count=settings.beginReadArray("pricing");
            for(int index=0;index<pricing_count;++index) {
                settings.setArrayIndex(index);
                value.pricing[settings.value("offset").toInt()]={
                    settings.value("max_profit",100.0).toDouble(),
                    settings.value("max_loss",100.0).toDouble()};
            }
            settings.endArray();
            presets.push_back(std::move(value));
        }
        settings.endGroup();
    }
    std::ranges::sort(presets,[](const auto& left,const auto& right) {
        return left.name.compare(right.name,Qt::CaseInsensitive)<0;
    });
    return presets;
}

void save_study_preset(const MomentumStudyPreset& value) {
    QSettings settings("OptionsBacktester","OptionsBacktester");
    settings.beginGroup("saved_studies");
    settings.beginGroup(value.id);
    settings.remove("");
    settings.setValue("schema_version",5);
    settings.setValue("strategy","momentum");
    settings.setValue("name",value.name);
    settings.setValue("symbol",value.symbol);
    settings.setValue("parametric",value.parametric);
    settings.setValue("window_days",value.window_days);
    settings.setValue("skip_days",value.skip_days);
    settings.setValue("bar_minutes",value.bar_minutes);
    settings.setValue("price_field",value.price_field);
    settings.setValue("drop_rate",value.drop_rate);
    settings.setValue("strike_enabled",value.strike_enabled);
    settings.setValue("strike_width",value.strike_width);
    settings.setValue("strike_resolution",value.strike_resolution);
    settings.setValue("strike_offset",value.strike_offset);
    settings.setValue("simulated_pricing",value.simulated_pricing);
    settings.setValue("allocation",value.allocation);
    settings.setValue("slippage_mode",value.slippage_mode);
    settings.setValue("buy_slippage",value.buy_slippage);
    settings.setValue("sell_slippage",value.sell_slippage);
    settings.setValue("analysis_start",value.analysis_start.toString(Qt::ISODate));
    settings.setValue("analysis_end",value.analysis_end.toString(Qt::ISODate));
    settings.setValue("window_minimum",value.window_minimum);
    settings.setValue("window_maximum",value.window_maximum);
    settings.setValue("skip_minimum",value.skip_minimum);
    settings.setValue("skip_maximum",value.skip_maximum);
    settings.setValue("strike_minimum",value.strike_minimum);
    settings.setValue("strike_maximum",value.strike_maximum);
    settings.setValue("saved_result_key",value.saved_result_key);
    settings.beginWriteArray("pricing",static_cast<int>(value.pricing.size()));
    int index=0;
    for(const auto& [offset,pricing]:value.pricing) {
        settings.setArrayIndex(index++);
        settings.setValue("offset",offset);
        settings.setValue("max_profit",pricing.first);
        settings.setValue("max_loss",pricing.second);
    }
    settings.endArray();
}

void delete_study_preset(const QString& id) {
    QSettings settings("OptionsBacktester","OptionsBacktester");
    settings.beginGroup("saved_studies");
    settings.remove(id);
}

constexpr quint32 saved_result_magic=0x4d4f4d43;
constexpr quint32 legacy_saved_result_version=7;
constexpr quint32 eager_curve_result_version=8;
constexpr quint32 saved_result_version=9;

void write_profit_curve(QDataStream& stream,
    const std::vector<options::analysis::MomentumResult::ProfitPoint>& curve) {
    QByteArray encoded;
    encoded.reserve(static_cast<qsizetype>(curve.size()*12));
    QDataStream curve_stream(&encoded,QIODevice::WriteOnly);
    curve_stream.setVersion(QDataStream::Qt_6_0);
    stream<<static_cast<quint64>(curve.size());
    for(const auto& point:curve) {
        const auto date=QDate::fromString(QString::fromStdString(point.date),Qt::ISODate);
        curve_stream<<static_cast<qint32>(date.toJulianDay())<<point.cumulative_profit;
    }
    stream<<encoded;
}

bool read_profit_curve(QDataStream& stream,
    std::vector<options::analysis::MomentumResult::ProfitPoint>& curve,bool retain=true) {
    quint64 count{};
    quint32 encoded_size{};
    stream>>count>>encoded_size;
    if(count>10000000) return false;
    if(encoded_size==std::numeric_limits<quint32>::max()) {
        curve.clear();
        return count==0 && stream.status()==QDataStream::Ok;
    }
    if(encoded_size!=count*12) return false;
    if(!retain) {
        curve.clear();
        return stream.skipRawData(static_cast<int>(encoded_size))==static_cast<int>(encoded_size) &&
            stream.status()==QDataStream::Ok;
    }
    QByteArray encoded(static_cast<qsizetype>(encoded_size),Qt::Uninitialized);
    if(stream.readRawData(encoded.data(),static_cast<int>(encoded_size))!=
       static_cast<int>(encoded_size)) return false;
    QDataStream curve_stream(encoded);
    curve_stream.setVersion(QDataStream::Qt_6_0);
    curve.clear();
    curve.reserve(static_cast<std::size_t>(count));
    for(quint64 index=0;index<count;++index) {
        qint32 julian_day{};
        double profit{};
        curve_stream>>julian_day>>profit;
        const auto date=QDate::fromJulianDay(julian_day);
        if(!date.isValid()) return false;
        curve.push_back({date.toString(Qt::ISODate).toStdString(),profit});
    }
    return stream.status()==QDataStream::Ok && curve_stream.status()==QDataStream::Ok;
}

void write_momentum_result(QDataStream& stream,const options::analysis::MomentumResult& value,
                           bool include_details=true) {
    stream<<value.comparisons<<value.skipped_comparisons<<value.dropped_comparisons
          <<value.wins<<value.losses<<value.ties<<value.win_percentage<<value.total_profit
          <<value.profit_percentage<<value.allocation<<value.ending_balance<<value.required_capital;
    const std::vector<options::analysis::MomentumResult::ProfitPoint> empty_curve;
    write_profit_curve(stream,include_details ? value.profit_curve : empty_curve);
    write_profit_curve(stream,include_details ? value.high_profit_curve : empty_curve);
    write_profit_curve(stream,include_details ? value.low_profit_curve : empty_curve);
    write_profit_curve(stream,include_details ? value.no_drop_profit_curve : empty_curve);
    stream<<static_cast<quint64>(value.drop_scenario_count)
          <<static_cast<quint64>(include_details ? value.trades.size() : 0);
    if(include_details)
        for(const auto& trade:value.trades)
            stream<<QString::fromStdString(trade.start_date)<<QString::fromStdString(trade.end_date)
                  <<trade.start_price<<trade.end_price<<trade.comparison_price<<trade.profit
                  <<static_cast<quint64>(trade.phase)<<static_cast<qint32>(trade.result);
}

bool read_momentum_result(
    QDataStream& stream,options::analysis::MomentumResult& value,bool retain_details=true) {
    stream>>value.comparisons>>value.skipped_comparisons>>value.dropped_comparisons
          >>value.wins>>value.losses>>value.ties>>value.win_percentage>>value.total_profit
          >>value.profit_percentage>>value.allocation>>value.ending_balance>>value.required_capital;
    if(!read_profit_curve(stream,value.profit_curve,retain_details) ||
       !read_profit_curve(stream,value.high_profit_curve,retain_details) ||
       !read_profit_curve(stream,value.low_profit_curve,retain_details) ||
       !read_profit_curve(stream,value.no_drop_profit_curve,retain_details)) return false;
    quint64 scenarios{};
    stream>>scenarios;
    value.drop_scenario_count=static_cast<std::size_t>(scenarios);
    quint64 trade_count{};
    stream>>trade_count;
    if(trade_count>10000000) return false;
    value.trades.clear();
    if(retain_details) value.trades.reserve(static_cast<std::size_t>(trade_count));
    for(quint64 index=0;index<trade_count;++index) {
        options::analysis::MomentumResult::TradeRecord trade;
        QString start_date,end_date;
        quint64 phase{};
        qint32 result{};
        stream>>start_date>>end_date>>trade.start_price>>trade.end_price
              >>trade.comparison_price>>trade.profit>>phase>>result;
        if(result<static_cast<qint32>(options::analysis::MomentumResult::TradeResult::itm) ||
           result>static_cast<qint32>(options::analysis::MomentumResult::TradeResult::atm))
            return false;
        trade.start_date=start_date.toStdString();
        trade.end_date=end_date.toStdString();
        trade.phase=static_cast<std::size_t>(phase);
        trade.result=static_cast<options::analysis::MomentumResult::TradeResult>(result);
        if(retain_details) value.trades.push_back(std::move(trade));
    }
    return stream.status()==QDataStream::Ok;
}

void write_momentum_row(QDataStream& stream,const MomentumStudyRow& row) {
    stream<<row.window_days<<row.skip_days<<row.strike_offset;
    write_momentum_result(stream,row.result,false);
    stream<<row.strike_adjustment.has_value();
    if(row.strike_adjustment)
        stream<<row.strike_adjustment->width<<row.strike_adjustment->resolution
              <<row.strike_adjustment->offset;
    stream<<row.simulated_pricing.has_value();
    if(row.simulated_pricing)
        stream<<row.simulated_pricing->max_profit<<row.simulated_pricing->max_loss
              <<row.simulated_pricing->allocation<<row.simulated_pricing->buy_slippage_per_share
              <<row.simulated_pricing->sell_slippage_per_share
              <<static_cast<qint32>(row.simulated_pricing->slippage_mode);
    stream<<row.drop_rate;
}

bool read_momentum_row(QDataStream& stream,MomentumStudyRow& row,bool retain_details=false) {
    stream>>row.window_days>>row.skip_days>>row.strike_offset;
    if(!read_momentum_result(stream,row.result,retain_details)) return false;
    bool has_adjustment{};
    stream>>has_adjustment;
    if(has_adjustment) {
        row.strike_adjustment.emplace();
        stream>>row.strike_adjustment->width>>row.strike_adjustment->resolution
              >>row.strike_adjustment->offset;
    }
    bool has_pricing{};
    stream>>has_pricing;
    if(has_pricing) {
        row.simulated_pricing.emplace();
        qint32 slippage_mode{};
        stream>>row.simulated_pricing->max_profit>>row.simulated_pricing->max_loss
              >>row.simulated_pricing->allocation>>row.simulated_pricing->buy_slippage_per_share
              >>row.simulated_pricing->sell_slippage_per_share>>slippage_mode;
        if(slippage_mode<static_cast<qint32>(options::analysis::SlippageMode::none) ||
           slippage_mode>static_cast<qint32>(options::analysis::SlippageMode::buy_and_sell))
            return false;
        row.simulated_pricing->slippage_mode=
            static_cast<options::analysis::SlippageMode>(slippage_mode);
    }
    stream>>row.drop_rate;
    return stream.status()==QDataStream::Ok;
}

int responsive_worker_count() {
    const auto available=std::max(1,QThread::idealThreadCount());
    const auto reserved=available>=4 ? std::max(2,available/4) : available>=2 ? 1 : 0;
    return std::max(1,available-reserved);
}

QByteArray momentum_preset_bytes(const MomentumStudyPreset& value) {
    QByteArray bytes;
    QDataStream stream(&bytes,QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_0);
    stream<<saved_result_version<<value.symbol<<value.parametric<<value.window_days
          <<value.skip_days<<value.bar_minutes<<value.price_field
          <<value.drop_rate<<value.strike_enabled<<value.strike_width
          <<value.strike_resolution
          <<value.strike_offset<<value.simulated_pricing<<value.allocation<<value.slippage_mode
          <<value.buy_slippage<<value.sell_slippage<<value.analysis_start<<value.analysis_end
          <<value.window_minimum<<value.window_maximum<<value.skip_minimum<<value.skip_maximum
          <<value.strike_minimum<<value.strike_maximum<<static_cast<quint64>(value.pricing.size());
    for(const auto& [offset,pricing]:value.pricing)
        stream<<offset<<pricing.first<<pricing.second;
    return bytes;
}

QString momentum_settings_signature(const MomentumStudyPreset& value) {
    return QString::fromLatin1(QCryptographicHash::hash(
        momentum_preset_bytes(value),QCryptographicHash::Sha256).toHex());
}

QString saved_result_path(const QString& key) {
    const auto directory=QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)+
        "/saved-results";
    QDir().mkpath(directory);
    return directory+"/"+key+".bin";
}

QString legacy_cache_path(const QString& key) {
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)+
        "/analysis-cache/"+key+".bin";
}

bool saved_result_exists(const QString& key) {
    return !key.isEmpty() &&
        (QFileInfo::exists(saved_result_path(key)) || QFileInfo::exists(legacy_cache_path(key)));
}

bool saved_result_uses_parallel_format(const QString& key) {
    QFile file(saved_result_path(key));
    if(!file.open(QIODevice::ReadOnly)) return false;
    QDataStream stream(&file);
    stream.setVersion(QDataStream::Qt_6_0);
    quint32 magic{},version{};
    QString stored_key;
    stream>>magic>>version>>stored_key;
    return stream.status()==QDataStream::Ok && magic==saved_result_magic &&
        version==saved_result_version && stored_key==key;
}

bool save_study_results(const QString& key,const MomentumAnalysisOutput& output,
                        std::span<const options::data::Bar> bars,
                        const std::shared_ptr<AnalysisProgress>& progress) {
    if(progress) {
        progress->completed.store(0,std::memory_order_relaxed);
        progress->phase.store(2,std::memory_order_relaxed);
    }
    QSaveFile file(saved_result_path(key));
    if(!file.open(QIODevice::WriteOnly)) return false;
    QDataStream stream(&file);
    stream.setVersion(QDataStream::Qt_6_0);
    stream<<saved_result_magic<<saved_result_version<<key<<output.symbol<<output.parametric
          <<output.strike_enabled<<output.simulated_pricing<<output.window_days<<output.skip_days
          <<output.strike_offset<<static_cast<quint64>(output.study_rows.size());
    std::vector<QByteArray> encoded_rows(output.study_rows.size());
    std::atomic<std::size_t> next_row{};
    std::atomic<bool> encode_failed{};
    const auto worker_count=std::min(
        static_cast<std::size_t>(responsive_worker_count()),output.study_rows.size());
    {
        std::vector<std::jthread> workers;
        workers.reserve(worker_count);
        for(std::size_t worker=0;worker<worker_count;++worker) {
            workers.emplace_back([&] {
                while(!encode_failed.load(std::memory_order_relaxed)) {
                    const auto index=next_row.fetch_add(1,std::memory_order_relaxed);
                    if(index>=output.study_rows.size()) break;
                    try {
                        QDataStream row_stream(&encoded_rows[index],QIODevice::WriteOnly);
                        row_stream.setVersion(QDataStream::Qt_6_0);
                        write_momentum_row(row_stream,output.study_rows[index]);
                        if(row_stream.status()!=QDataStream::Ok) {
                            encode_failed.store(true,std::memory_order_relaxed);
                            break;
                        }
                        if(progress) progress->completed.fetch_add(1,std::memory_order_relaxed);
                    } catch(...) {
                        encode_failed.store(true,std::memory_order_relaxed);
                        break;
                    }
                }
            });
        }
    }
    if(encode_failed.load(std::memory_order_relaxed)) return false;
    for(const auto& encoded:encoded_rows) stream<<encoded;
    stream<<output.single_result.has_value();
    if(output.single_result) {
        write_momentum_result(stream,*output.single_result);
        if(progress) progress->completed.store(
            progress->total.load(std::memory_order_relaxed),std::memory_order_relaxed);
    }
    stream<<static_cast<quint64>(bars.size());
    for(const auto& bar:bars)
        stream<<QString::fromStdString(bar.symbol)<<QString::fromStdString(bar.timestamp)
              <<bar.open<<bar.high<<bar.low<<bar.close<<static_cast<qint64>(bar.volume)
              <<static_cast<qint64>(bar.trade_count)<<bar.vwap;
    if(stream.status()!=QDataStream::Ok) return false;
    return file.commit();
}

struct SavedMomentumAnalysis {
    MomentumAnalysisOutput output;
    std::vector<options::data::Bar> bars;
};

std::optional<SavedMomentumAnalysis> load_study_results(
    const QString& key,const std::shared_ptr<AnalysisProgress>& progress,int requested_workers) {
    QFile file(saved_result_path(key));
    if(!file.exists()) file.setFileName(legacy_cache_path(key));
    if(!file.open(QIODevice::ReadOnly)) return std::nullopt;
    QDataStream stream(&file);
    stream.setVersion(QDataStream::Qt_6_0);
    quint32 magic{},version{};
    QString stored_key;
    SavedMomentumAnalysis saved;
    stream>>magic>>version>>stored_key;
    if(magic!=saved_result_magic ||
       (version!=legacy_saved_result_version && version!=eager_curve_result_version &&
        version!=saved_result_version) || stored_key!=key)
        return std::nullopt;
    stream>>saved.output.symbol>>saved.output.parametric>>saved.output.strike_enabled
          >>saved.output.simulated_pricing>>saved.output.window_days>>saved.output.skip_days
          >>saved.output.strike_offset;
    quint64 row_count{};
    stream>>row_count;
    if(row_count>10000) return std::nullopt;
    if(progress) {
        progress->completed.store(0,std::memory_order_relaxed);
        progress->total=static_cast<std::size_t>(row_count);
        progress->phase.store(3,std::memory_order_relaxed);
    }
    saved.output.study_rows.resize(static_cast<std::size_t>(row_count));
    if(version==legacy_saved_result_version) {
        for(quint64 index=0;index<row_count;++index) {
            if(!read_momentum_row(stream,saved.output.study_rows[static_cast<std::size_t>(index)]))
                return std::nullopt;
            if(progress) progress->completed.fetch_add(1,std::memory_order_relaxed);
        }
    } else {
        std::vector<QByteArray> encoded_rows(static_cast<std::size_t>(row_count));
        for(auto& encoded:encoded_rows) stream>>encoded;
        if(stream.status()!=QDataStream::Ok) return std::nullopt;
        std::atomic<std::size_t> next_row{};
        std::atomic<bool> decode_failed{};
        const auto worker_count=std::min(
            static_cast<std::size_t>(std::max(1,requested_workers)),encoded_rows.size());
        {
            std::vector<std::jthread> workers;
            workers.reserve(worker_count);
            for(std::size_t worker=0;worker<worker_count;++worker) {
                workers.emplace_back([&] {
                    while(!decode_failed.load(std::memory_order_relaxed)) {
                        const auto index=next_row.fetch_add(1,std::memory_order_relaxed);
                        if(index>=encoded_rows.size()) break;
                        try {
                            QDataStream row_stream(encoded_rows[index]);
                            row_stream.setVersion(QDataStream::Qt_6_0);
                            if(!read_momentum_row(row_stream,saved.output.study_rows[index]) ||
                               row_stream.status()!=QDataStream::Ok) {
                                decode_failed.store(true,std::memory_order_relaxed);
                                break;
                            }
                            encoded_rows[index]=QByteArray();
                            if(progress) progress->completed.fetch_add(1,std::memory_order_relaxed);
                        } catch(...) {
                            decode_failed.store(true,std::memory_order_relaxed);
                            break;
                        }
                    }
                });
            }
        }
        if(decode_failed.load(std::memory_order_relaxed)) return std::nullopt;
    }
    bool has_single{};
    stream>>has_single;
    if(has_single) {
        saved.output.single_result.emplace();
        if(!read_momentum_result(stream,*saved.output.single_result)) return std::nullopt;
    }
    quint64 bar_count{};
    stream>>bar_count;
    if(bar_count>10000000) return std::nullopt;
    saved.bars.reserve(static_cast<std::size_t>(bar_count));
    for(quint64 index=0;index<bar_count;++index) {
        options::data::Bar bar;
        QString symbol,timestamp;
        qint64 volume{},trade_count{};
        stream>>symbol>>timestamp>>bar.open>>bar.high>>bar.low>>bar.close>>volume>>trade_count>>bar.vwap;
        bar.symbol=symbol.toStdString();
        bar.timestamp=timestamp.toStdString();
        bar.volume=volume;
        bar.trade_count=trade_count;
        saved.bars.push_back(std::move(bar));
    }
    if(stream.status()!=QDataStream::Ok) return std::nullopt;
    saved.output.saved_result_key=key;
    saved.output.loaded_saved_result=true;
    return saved;
}

class NumericTableWidgetItem final : public QTableWidgetItem {
public:
    NumericTableWidgetItem(const QString& text,double value) : QTableWidgetItem(text),value_(value) {}

    bool operator<(const QTableWidgetItem& other) const override {
        const auto* numeric=dynamic_cast<const NumericTableWidgetItem*>(&other);
        return numeric ? value_<numeric->value_ : QTableWidgetItem::operator<(other);
    }

private:
    double value_{};
};

class PricingEditor final : public QWidget {
public:
    explicit PricingEditor(QWidget* parent=nullptr) : QWidget(parent),layout_(new QFormLayout(this)) {}

    void set_offsets(std::vector<int> offsets) {
        for(const auto& [offset,widgets]:inputs_)
            saved_[offset]={widgets.first->value(),widgets.second->value()};
        while(layout_->rowCount()>0) layout_->removeRow(0);
        inputs_.clear();
        std::ranges::sort(offsets);
        offsets.erase(std::unique(offsets.begin(),offsets.end()),offsets.end());
        const auto has_negative=std::ranges::any_of(offsets,[](int value){return value<0;});
        const auto has_positive=std::ranges::any_of(offsets,[](int value){return value>0;});
        for(const auto offset:offsets) {
            if(offset==0 && has_negative && has_positive) continue;
            auto* row=new QWidget;
            auto* row_layout=new QHBoxLayout(row);
            row_layout->setContentsMargins(0,0,0,0);
            auto* profit=money_box(0.0);
            auto* loss=money_box(0.01);
            const auto saved=saved_.find(offset);
            profit->setValue(saved==saved_.end() ? 100.0 : saved->second.first);
            loss->setValue(saved==saved_.end() ? 100.0 : saved->second.second);
            row_layout->addWidget(new QLabel("Max profit")); row_layout->addWidget(profit);
            row_layout->addWidget(new QLabel("Max loss")); row_layout->addWidget(loss);
            row_layout->addStretch();
            layout_->addRow("Strike offset "+QString::number(offset),row);
            inputs_[offset]={profit,loss};
        }
        if(std::ranges::find(offsets,0)!=offsets.end() && has_negative && has_positive)
            layout_->addRow("Strike offset 0",new QLabel("Midpoint of nearest negative and positive pricing"));
    }

    [[nodiscard]] std::optional<options::analysis::SimulatedPricing> pricing_for(int offset) const {
        const auto direct=inputs_.find(offset);
        if(direct!=inputs_.end()) return pricing(direct->second);
        if(offset!=0) return std::nullopt;
        auto negative=inputs_.end();
        auto positive=inputs_.end();
        for(auto current=inputs_.begin();current!=inputs_.end();++current) {
            if(current->first<0 && (negative==inputs_.end() || current->first>negative->first)) negative=current;
            if(current->first>0 && (positive==inputs_.end() || current->first<positive->first)) positive=current;
        }
        if(negative==inputs_.end() || positive==inputs_.end()) return std::nullopt;
        const auto below=pricing(negative->second);
        const auto above=pricing(positive->second);
        return options::analysis::SimulatedPricing{
            (below.max_profit+above.max_profit)/2.0,(below.max_loss+above.max_loss)/2.0,0};
    }

    void set_pricing(std::map<int,std::pair<double,double>> pricing) {
        saved_=std::move(pricing);
        for(const auto& [offset,value]:saved_) {
            const auto input=inputs_.find(offset);
            if(input!=inputs_.end()) {
                input->second.first->setValue(value.first);
                input->second.second->setValue(value.second);
            }
        }
    }

    [[nodiscard]] std::map<int,std::pair<double,double>> all_pricing() const {
        auto values=saved_;
        for(const auto& [offset,widgets]:inputs_)
            values[offset]={widgets.first->value(),widgets.second->value()};
        return values;
    }

private:
    static QDoubleSpinBox* money_box(double minimum) {
        auto* box=new QDoubleSpinBox;
        box->setRange(minimum,1000000000.0);
        box->setDecimals(2);
        box->setPrefix("$");
        return box;
    }
    static options::analysis::SimulatedPricing pricing(
        const std::pair<QDoubleSpinBox*,QDoubleSpinBox*>& widgets) {
        return {widgets.first->value(),widgets.second->value(),0};
    }

    QFormLayout* layout_{};
    std::map<int,std::pair<QDoubleSpinBox*,QDoubleSpinBox*>> inputs_;
    std::map<int,std::pair<double,double>> saved_;
};

QString averaged_count(double value);

class LedgerTableWidget final : public QTableWidget {
public:
    using QTableWidget::QTableWidget;

    void set_row_hover_callback(std::function<void(std::optional<int>)> callback) {
        callback_=std::move(callback);
        setMouseTracking(true);
        viewport()->setMouseTracking(true);
    }

protected:
    void mouseMoveEvent(QMouseEvent* event) override {
        QTableWidget::mouseMoveEvent(event);
        const auto row=indexAt(event->position().toPoint()).row();
        if(row==hovered_row_) return;
        hovered_row_=row;
        if(callback_) callback_(row>=0 ? std::optional<int>(row) : std::nullopt);
    }

    void leaveEvent(QEvent* event) override {
        hovered_row_=-1;
        if(callback_) callback_(std::nullopt);
        QTableWidget::leaveEvent(event);
    }

private:
    std::function<void(std::optional<int>)> callback_;
    int hovered_row_{-1};
};

class ProfitChartView final : public QChartView {
public:
    explicit ProfitChartView(QChart* chart,QWidget* parent=nullptr) : QChartView(chart,parent) {
        setMouseTracking(true);
        viewport()->setMouseTracking(true);
    }

    void set_hover_series(QLineSeries* no_drops,QLineSeries* high,QLineSeries* low,
                          QLineSeries* median) {
        hover_series_={{"No drops",no_drops},{"High",high},{"Low",low},{"Median",median}};
        original_profit_pens_.clear();
        for(const auto& [name,series]:hover_series_) {
            Q_UNUSED(name);
            original_profit_pens_.push_back(series ? series->pen() : QPen());
        }
    }

    void set_underlying_series(QLineSeries* underlying) {
        underlying_series_=underlying;
        if(underlying_series_) original_underlying_pen_=underlying_series_->pen();
    }

    void highlight_trade(
        std::optional<options::analysis::MomentumResult::TradeRecord> trade) {
        if(trade && trade->result==options::analysis::MomentumResult::TradeResult::atm)
            trade.reset();
        highlighted_trade_=std::move(trade);
        const QColor muted_colors[]{QColor("#d9e5f2"),QColor("#d8eed9"),
                                    QColor("#f3dada"),QColor("#dddddd")};
        for(std::size_t index=0;index<hover_series_.size();++index) {
            auto* series=hover_series_[index].second;
            if(!series) continue;
            auto pen=index<original_profit_pens_.size() ? original_profit_pens_[index] : series->pen();
            if(highlighted_trade_) pen.setColor(muted_colors[index]);
            series->setPen(pen);
        }
        if(underlying_series_) {
            auto pen=original_underlying_pen_;
            if(highlighted_trade_) {
                pen.setColor(Qt::black);
                pen.setWidthF(2.0);
            }
            underlying_series_->setPen(pen);
        }
        viewport()->update();
    }

protected:
    void mouseMoveEvent(QMouseEvent* event) override {
        QChartView::mouseMoveEvent(event);
        const auto plot=chart()->plotArea();
        if(plot.contains(event->position())) {
            hover_position_=event->position();
            hovering_=true;
        } else {
            hovering_=false;
        }
        viewport()->update();
    }

    void leaveEvent(QEvent* event) override {
        hovering_=false;
        viewport()->update();
        QChartView::leaveEvent(event);
    }

    void paintEvent(QPaintEvent* event) override {
        QChartView::paintEvent(event);
        QPainter painter(viewport());
        painter.setRenderHint(QPainter::Antialiasing);
        const auto plot=chart()->plotArea();
        if(highlighted_trade_ && !hover_series_.empty() && hover_series_.front().second &&
           underlying_series_)
            paint_trade_highlight(painter,plot,*highlighted_trade_,
                hover_series_.front().second,underlying_series_);
        if(!hovering_ || hover_series_.empty() || !hover_series_.front().second) return;

        const auto x=qBound(plot.left(),hover_position_.x(),plot.right());
        const auto chart_value=chart()->mapToValue(QPointF(x,plot.center().y()),hover_series_.front().second);
        const auto milliseconds=static_cast<qint64>(std::llround(chart_value.x()));
        QStringList lines{
            "Time: "+QDateTime::fromMSecsSinceEpoch(milliseconds,Qt::UTC)
                .toString(Qt::ISODate)};
        for(const auto& [name,series]:hover_series_) {
            const auto value=value_at(series,chart_value.x());
            lines.push_back(name+": "+(value ? "$"+QString::number(*value,'f',2) : QString("—")));
        }

        painter.setPen(QPen(QColor("#777777"),1,Qt::DashLine));
        painter.drawLine(QPointF(x,plot.top()),QPointF(x,plot.bottom()));

        const QFontMetrics metrics(painter.font());
        int text_width=0;
        for(const auto& line:lines) text_width=qMax(text_width,metrics.horizontalAdvance(line));
        constexpr int horizontal_padding=12;
        constexpr int vertical_padding=9;
        const auto box_width=text_width+horizontal_padding*2;
        const auto box_height=metrics.height()*lines.size()+vertical_padding*2;
        const auto box_x=qMax(8.0,x-box_width-12.0);
        const auto box_y=qBound(8.0,plot.top()+12.0,
            qMax(8.0,static_cast<double>(viewport()->height()-box_height-8)));
        const QRectF box(box_x,box_y,box_width,box_height);
        painter.setPen(QPen(QColor("#a8a8a8")));
        painter.setBrush(QColor("#e8e8e8"));
        painter.drawRoundedRect(box,5,5);
        painter.setPen(QColor("#202020"));
        auto baseline=box.top()+vertical_padding+metrics.ascent();
        for(const auto& line:lines) {
            painter.drawText(QPointF(box.left()+horizontal_padding,baseline),line);
            baseline+=metrics.height();
        }
    }

private:
    static void paint_trade_highlight(QPainter& painter,const QRectF& plot,
        const options::analysis::MomentumResult::TradeRecord& trade,QLineSeries* date_reference,
        QLineSeries* price_reference) {
        const auto start_timestamp=QDateTime::fromString(
            QString::fromStdString(trade.start_date),Qt::ISODate);
        const auto end_timestamp=QDateTime::fromString(
            QString::fromStdString(trade.end_date),Qt::ISODate);
        const auto points=date_reference->points();
        if(!start_timestamp.isValid() || !end_timestamp.isValid() || points.empty()) return;
        const auto y=points.front().y();
        const auto start_value=start_timestamp.toMSecsSinceEpoch();
        const auto end_value=end_timestamp.toMSecsSinceEpoch();
        const auto start_x=qBound(plot.left(),date_reference->chart()->mapToPosition(
            QPointF(start_value,y),date_reference).x(),plot.right());
        const auto end_x=qBound(plot.left(),date_reference->chart()->mapToPosition(
            QPointF(end_value,y),date_reference).x(),plot.right());
        const auto left=std::min(start_x,end_x);
        const auto right=std::max(start_x,end_x);
        const auto strike_y=qBound(plot.top(),price_reference->chart()->mapToPosition(
            QPointF(start_value,trade.comparison_price),price_reference).y(),plot.bottom());
        auto itm_fill=QColor("#66bb6a");
        auto otm_fill=QColor("#ef5350");
        itm_fill.setAlpha(48);
        otm_fill.setAlpha(48);
        painter.fillRect(QRectF(left,plot.top(),right-left,strike_y-plot.top()),itm_fill);
        painter.fillRect(QRectF(left,strike_y,right-left,plot.bottom()-strike_y),otm_fill);
        painter.setPen(QPen(QColor("#444444"),1.25));
        painter.drawLine(QPointF(left,strike_y),QPointF(right,strike_y));
        painter.setPen(QPen(QColor("#555555"),1.25,Qt::DashLine));
        painter.drawLine(QPointF(start_x,plot.top()),QPointF(start_x,plot.bottom()));
        painter.drawLine(QPointF(end_x,plot.top()),QPointF(end_x,plot.bottom()));
    }

    static std::optional<double> value_at(const QLineSeries* series,double x) {
        if(!series) return std::nullopt;
        const auto points=series->points();
        if(points.empty() || x<points.front().x() || x>points.back().x()) return std::nullopt;
        const auto after=std::lower_bound(points.begin(),points.end(),x,
            [](const QPointF& point,double value) { return point.x()<value; });
        if(after==points.begin()) return after->y();
        if(after==points.end()) return points.back().y();
        if(std::abs(after->x()-x)<0.5) return after->y();
        const auto before=std::prev(after);
        const auto span=after->x()-before->x();
        if(span==0.0) return after->y();
        const auto fraction=(x-before->x())/span;
        return before->y()+fraction*(after->y()-before->y());
    }

    std::vector<std::pair<QString,QLineSeries*>> hover_series_;
    std::vector<QPen> original_profit_pens_;
    QLineSeries* underlying_series_{};
    QPen original_underlying_pen_;
    std::optional<options::analysis::MomentumResult::TradeRecord> highlighted_trade_;
    QPointF hover_position_;
    bool hovering_{};
};

void show_profit_chart(QWidget* parent,const QString& title,
                       const options::analysis::MomentumResult& result,
                       std::span<const options::data::Bar> underlying_bars) {
    QDialog dialog(parent);
    dialog.setWindowTitle("Simulated Profit — "+title);
    dialog.setWindowFlag(Qt::WindowMaximizeButtonHint,true);
    dialog.setSizeGripEnabled(true);
    dialog.resize(1200,850);
    auto* layout=new QVBoxLayout(&dialog);
    auto* chart=new QChart;
    chart->setTitle("Simulated account value — one contract, averaged across start phases");
    std::vector<QLineSeries*> series;
    const auto add_curve=[&](const QString& name,
                             const std::vector<options::analysis::MomentumResult::ProfitPoint>& curve,
                             const QColor& color,Qt::PenStyle style=Qt::SolidLine) {
        auto* line=new QLineSeries;
        line->setName(name);
        line->setPen(QPen(color,2,style));
        for(const auto& point:curve) {
            const auto date=QDate::fromString(QString::fromStdString(point.date),Qt::ISODate);
            line->append(QDateTime(date,QTime(12,0),Qt::UTC).toMSecsSinceEpoch(),
                         result.allocation+point.cumulative_profit);
        }
        chart->addSeries(line);
        series.push_back(line);
        return line;
    };
    QLineSeries *no_drops_line{},*high_line{},*low_line{},*median_line{};
    if(result.drop_scenario_count>=5) {
        no_drops_line=add_curve("No drops",result.no_drop_profit_curve,QColor("#1565c0"),Qt::DashLine);
        high_line=add_curve("High",result.high_profit_curve,QColor("#2e7d32"));
        low_line=add_curve("Low",result.low_profit_curve,QColor("#c62828"));
        median_line=add_curve("Median",result.profit_curve,QColor("#000000"));
    } else {
        median_line=add_curve("Account value",result.profit_curve,QColor("#000000"));
        no_drops_line=high_line=low_line=median_line;
    }
    auto* underlying_line=new QLineSeries;
    underlying_line->setName("Underlying");
    underlying_line->setPen(QPen(QColor("#d8d8d8"),1.5));
    double underlying_min=std::numeric_limits<double>::max();
    double underlying_max=std::numeric_limits<double>::lowest();
    for(const auto& bar:underlying_bars) {
        const auto timestamp=QDateTime::fromString(
            QString::fromStdString(bar.timestamp),Qt::ISODate);
        const auto value=bar.vwap>0.0 && std::isfinite(bar.vwap) ? bar.vwap : bar.close;
        if(!timestamp.isValid() || !std::isfinite(value)) continue;
        underlying_line->append(timestamp.toMSecsSinceEpoch(),value);
        underlying_min=std::min(underlying_min,value);
        underlying_max=std::max(underlying_max,value);
    }
    chart->addSeries(underlying_line);
    auto* dates=new QDateTimeAxis;
    dates->setFormat("MMM yyyy");
    dates->setTitleText("Exit date");
    auto* profit=new QValueAxis;
    profit->setLabelFormat("$%.2f");
    profit->setTitleText("Account value");
    chart->addAxis(dates,Qt::AlignBottom); chart->addAxis(profit,Qt::AlignLeft);
    for(auto* line:series) {
        line->attachAxis(dates);
        line->attachAxis(profit);
    }
    underlying_line->attachAxis(dates);
    if(!underlying_line->points().empty()) {
        auto* underlying_price=new QValueAxis;
        underlying_price->setLabelFormat("$%.2f");
        underlying_price->setTitleText("Underlying price");
        underlying_price->setLabelsColor(QColor("#999999"));
        underlying_price->setLinePenColor(QColor("#c8c8c8"));
        underlying_price->setTitleBrush(QBrush(QColor("#888888")));
        const auto spread=underlying_max-underlying_min;
        const auto padding=spread>0.0 ? spread*0.05 : qMax(1.0,std::abs(underlying_min)*0.01);
        underlying_price->setRange(underlying_min-padding,underlying_max+padding);
        chart->addAxis(underlying_price,Qt::AlignRight);
        underlying_line->attachAxis(underlying_price);
    }
    auto* view=new ProfitChartView(chart);
    view->set_hover_series(no_drops_line,high_line,low_line,median_line);
    view->set_underlying_series(underlying_line);
    view->setRenderHint(QPainter::Antialiasing);
    auto* content=new QSplitter(Qt::Vertical);
    content->addWidget(view);
    if(!result.trades.empty()) {
        auto* ledger_container=new QWidget;
        auto* ledger_layout=new QVBoxLayout(ledger_container);
        ledger_layout->setContentsMargins(0,0,0,0);
        auto* ledger_label=new QLabel(
            "Median-scenario executed trades — phase identifies the skip-window start schedule");
        ledger_layout->addWidget(ledger_label);
        auto* ledger=new LedgerTableWidget(static_cast<int>(result.trades.size()),9);
        ledger->setHorizontalHeaderLabels({"Start date","End date","Start price","End price",
            "Comparison price","Result","Phase","Contract P/L","Money currently at risk"});
        ledger->setEditTriggers(QAbstractItemView::NoEditTriggers);
        ledger->setSelectionBehavior(QAbstractItemView::SelectRows);
        ledger->setAlternatingRowColors(true);
        ledger->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        ledger->horizontalHeader()->setStretchLastSection(true);
        for(int row=0;row<static_cast<int>(result.trades.size());++row) {
            const auto& trade=result.trades[static_cast<std::size_t>(row)];
            const auto result_text=trade.result==options::analysis::MomentumResult::TradeResult::itm
                ? "ITM" : trade.result==options::analysis::MomentumResult::TradeResult::otm
                    ? "OTM" : "ATM";
            ledger->setItem(row,0,new QTableWidgetItem(QString::fromStdString(trade.start_date)));
            ledger->setItem(row,1,new QTableWidgetItem(QString::fromStdString(trade.end_date)));
            ledger->setItem(row,2,new NumericTableWidgetItem(
                "$"+QString::number(trade.start_price,'f',2),trade.start_price));
            ledger->setItem(row,3,new NumericTableWidgetItem(
                "$"+QString::number(trade.end_price,'f',2),trade.end_price));
            ledger->setItem(row,4,new NumericTableWidgetItem(
                "$"+QString::number(trade.comparison_price,'f',2),trade.comparison_price));
            ledger->setItem(row,5,new QTableWidgetItem(result_text));
            ledger->setItem(row,6,new NumericTableWidgetItem(
                QString::number(trade.phase+1),static_cast<double>(trade.phase+1)));
            ledger->setItem(row,7,new NumericTableWidgetItem(
                "$"+QString::number(trade.profit,'f',2),trade.profit));
            ledger->setItem(row,8,new NumericTableWidgetItem(
                "$"+QString::number(trade.money_at_risk,'f',2),trade.money_at_risk));
        }
        ledger->set_row_hover_callback([view,&result](std::optional<int> row) {
            if(!row || *row<0 || *row>=static_cast<int>(result.trades.size())) {
                view->highlight_trade(std::nullopt);
                return;
            }
            const auto& trade=result.trades[static_cast<std::size_t>(*row)];
            if(trade.result==options::analysis::MomentumResult::TradeResult::atm) {
                view->highlight_trade(std::nullopt);
                return;
            }
            view->highlight_trade(trade);
        });
        ledger_layout->addWidget(ledger,1);
        content->addWidget(ledger_container);
        content->setCollapsible(0,false);
        content->setCollapsible(1,false);
        content->setStretchFactor(0,7);
        content->setStretchFactor(1,3);
        content->setSizes({700,300});
    }
    layout->addWidget(content,1);
    auto* summary=new QLabel("Allocation: $"+QString::number(result.allocation,'f',2)+
        "    Capital needed: $"+QString::number(result.required_capital,'f',2)+
        "    Avg skipped trades: "+averaged_count(result.skipped_comparisons)+
        "    Avg dropped trades: "+averaged_count(result.dropped_comparisons)+
        "    Total profit: $"+QString::number(result.total_profit,'f',2)+
        "    Ending balance: $"+QString::number(result.ending_balance,'f',2)+
        "    Profit: "+QString::number(result.profit_percentage,'f',2)+"%");
    layout->addWidget(summary);
    dialog.exec();
}

std::string environment(const char* name) {
    const auto* value=std::getenv(name);
    if(!value || !*value) throw std::runtime_error(QString("Missing environment variable: "+
        QString::fromUtf8(name)).toStdString());
    return value;
}

QString averaged_count(double value) {
    const auto rounded=std::round(value);
    return std::abs(value-rounded)<1e-9
        ? QString::number(static_cast<qint64>(rounded))
        : QString::number(value,'f',2);
}

class MomentumStudyTableModel final : public QAbstractTableModel {
public:
    explicit MomentumStudyTableModel(QObject* parent=nullptr) : QAbstractTableModel(parent) {}

    int rowCount(const QModelIndex& parent={}) const override {
        return parent.isValid() ? 0 : static_cast<int>(order_.size());
    }

    int columnCount(const QModelIndex& parent={}) const override {
        return parent.isValid() ? 0 : 12;
    }

    QVariant headerData(int section,Qt::Orientation orientation,int role) const override {
        if(role!=Qt::DisplayRole) return {};
        if(orientation==Qt::Vertical) return section+1;
        static const QString headers[]{"DTE","Skip","Strike offset","Win Rate %","Avg wins",
            "Avg losses","Avg comparisons","Avg dropped","Total profit","Profit %",
            "Capital Needed","Order"};
        return section>=0 && section<12 ? QVariant(headers[section]) : QVariant();
    }

    QVariant data(const QModelIndex& index,int role=Qt::DisplayRole) const override {
        if(!index.isValid() || !rows_ || index.row()<0 ||
           static_cast<std::size_t>(index.row())>=order_.size()) return {};
        const auto source=order_[static_cast<std::size_t>(index.row())];
        const auto& value=(*rows_)[source];
        if(role==Qt::UserRole) return static_cast<qulonglong>(source);
        if(role==Qt::BackgroundRole && source<highlighted_.size() && highlighted_[source])
            return QBrush(QColor("#c8e6c9"));
        if(role!=Qt::DisplayRole) return {};
        switch(index.column()) {
        case 0: return QString::number(value.window_days);
        case 1: return QString::number(value.skip_days);
        case 2: return strike_enabled_ ? QString::number(value.strike_offset) : QString("Off");
        case 3: return QString::number(value.result.win_percentage,'f',2)+"%";
        case 4: return averaged_count(value.result.wins);
        case 5: return averaged_count(value.result.losses);
        case 6: return averaged_count(value.result.comparisons);
        case 7: return averaged_count(value.result.dropped_comparisons);
        case 8: return "$"+QString::number(value.result.total_profit,'f',2);
        case 9: return QString::number(value.result.profit_percentage,'f',2)+"%";
        case 10: return "$"+QString::number(value.result.required_capital,'f',2);
        case 11: return QString::number(source);
        default: return {};
        }
    }

    void set_rows(const std::vector<MomentumStudyRow>* rows,bool strike_enabled) {
        beginResetModel();
        rows_=rows;
        strike_enabled_=strike_enabled;
        order_.clear();
        highlighted_.clear();
        if(rows_) {
            order_.resize(rows_->size());
            std::iota(order_.begin(),order_.end(),0);
            highlighted_.assign(rows_->size(),false);
            auto ranked=order_;
            std::ranges::sort(ranked,[this](auto left,auto right) {
                const auto& left_result=(*rows_)[left].result;
                const auto& right_result=(*rows_)[right].result;
                if(left_result.comparisons!=right_result.comparisons)
                    return left_result.comparisons>right_result.comparisons;
                return left_result.win_percentage>right_result.win_percentage;
            });
            ranked.resize(std::min<std::size_t>(10,ranked.size()));
            for(const auto source:ranked) highlighted_[source]=true;
        }
        endResetModel();
    }

    void sort_by(int column,std::optional<Qt::SortOrder> direction) {
        if(!rows_) return;
        beginResetModel();
        std::iota(order_.begin(),order_.end(),0);
        if(direction) {
            std::ranges::stable_sort(order_,[this,column,direction](auto left,auto right) {
                const auto left_value=numeric_value((*rows_)[left],column,left);
                const auto right_value=numeric_value((*rows_)[right],column,right);
                return *direction==Qt::AscendingOrder
                    ? left_value<right_value : left_value>right_value;
            });
        }
        endResetModel();
    }

    std::size_t source_index(int displayed_row) const {
        return displayed_row>=0 && static_cast<std::size_t>(displayed_row)<order_.size()
            ? order_[static_cast<std::size_t>(displayed_row)]
            : std::numeric_limits<std::size_t>::max();
    }

private:
    static double numeric_value(const MomentumStudyRow& value,int column,std::size_t source) {
        switch(column) {
        case 0: return value.window_days;
        case 1: return value.skip_days;
        case 2: return value.strike_offset;
        case 3: return value.result.win_percentage;
        case 4: return value.result.wins;
        case 5: return value.result.losses;
        case 6: return value.result.comparisons;
        case 7: return value.result.dropped_comparisons;
        case 8: return value.result.total_profit;
        case 9: return value.result.profit_percentage;
        case 10: return value.result.required_capital;
        default: return static_cast<double>(source);
        }
    }

    const std::vector<MomentumStudyRow>* rows_{};
    std::vector<std::size_t> order_;
    std::vector<bool> highlighted_;
    bool strike_enabled_{};
};

QString initial_database_path() {
    QSettings settings("OptionsBacktester","OptionsBacktester");
    const auto saved=settings.value("database").toString();
    if(!saved.isEmpty() && QFileInfo::exists(saved)) return saved;
    const auto current=QFileInfo("market-data.db").absoluteFilePath();
    if(QFileInfo::exists(current)) return current;
    const auto from_build=QFileInfo(QCoreApplication::applicationDirPath()+"/../../market-data.db").absoluteFilePath();
    if(QFileInfo::exists(from_build)) return from_build;
    return current;
}

const QTimeZone& eastern_time_zone() {
    static const QTimeZone zone("America/New_York");
    return zone;
}

QDateTime eastern_timestamp(const options::data::Bar& bar) {
    return QDateTime::fromString(QString::fromStdString(bar.timestamp),Qt::ISODate)
        .toTimeZone(eastern_time_zone());
}

bool is_regular_market_bar(const QDateTime& timestamp) {
    const auto time=timestamp.time();
    return timestamp.isValid() && timestamp.date().dayOfWeek()<=Qt::Friday &&
        time>=QTime(9,30) && time<QTime(16,0);
}

struct CandleResolution {
    int minutes{};
    QString label;
};

struct ChartCandle {
    QDateTime timestamp;
    double open{};
    double high{};
    double low{};
    double close{};
    qint64 volume{};
};

std::vector<ChartCandle> aggregate_chart_candles(
    std::span<const options::data::Bar> bars,const CandleResolution& resolution) {
    std::vector<ChartCandle> result;
    const QTime market_open(9,30);
    for(const auto& bar:bars) {
        const auto timestamp=eastern_timestamp(bar);
        if(!is_regular_market_bar(timestamp)) continue;
        const auto bucket_time=resolution.minutes==0
            ? market_open
            : market_open.addSecs(
                (market_open.secsTo(timestamp.time())/60/resolution.minutes)*
                resolution.minutes*60);
        const QDateTime bucket(timestamp.date(),bucket_time,eastern_time_zone());
        if(result.empty() || result.back().timestamp!=bucket) {
            result.push_back({bucket,bar.open,bar.high,bar.low,bar.close,bar.volume});
            continue;
        }
        auto& candle=result.back();
        candle.high=std::max(candle.high,bar.high);
        candle.low=std::min(candle.low,bar.low);
        candle.close=bar.close;
        candle.volume+=bar.volume;
    }
    return result;
}

std::vector<options::data::Bar> aggregate_regular_bars(
    std::span<const options::data::Bar> bars,int minutes) {
    std::vector<options::data::Bar> result;
    std::vector<long double> vwap_numerators;
    std::vector<long double> vwap_weights;
    const QTime market_open(9,30);
    for(const auto& source:bars) {
        const auto timestamp=eastern_timestamp(source);
        if(!is_regular_market_bar(timestamp)) continue;
        const auto bucket_time=minutes==0 ? market_open : market_open.addSecs(
            (market_open.secsTo(timestamp.time())/60/minutes)*minutes*60);
        const QDateTime bucket(timestamp.date(),bucket_time,eastern_time_zone());
        const auto bucket_text=bucket.toUTC().toString(Qt::ISODateWithMs).toStdString();
        const auto weight=static_cast<long double>(source.volume>0 ? source.volume : 1);
        if(result.empty() || result.back().timestamp!=bucket_text) {
            auto bar=source;
            bar.timestamp=bucket_text;
            result.push_back(std::move(bar));
            vwap_numerators.push_back(static_cast<long double>(source.vwap)*weight);
            vwap_weights.push_back(weight);
            continue;
        }
        auto& bar=result.back();
        bar.high=std::max(bar.high,source.high);
        bar.low=std::min(bar.low,source.low);
        bar.close=source.close;
        bar.volume+=source.volume;
        bar.trade_count+=source.trade_count;
        vwap_numerators.back()+=static_cast<long double>(source.vwap)*weight;
        vwap_weights.back()+=weight;
        bar.vwap=static_cast<double>(vwap_numerators.back()/vwap_weights.back());
    }
    return result;
}

double analysis_price(const options::data::Bar& bar,const QString& field) {
    if(field=="open") return bar.open;
    if(field=="high") return bar.high;
    if(field=="low") return bar.low;
    if(field=="close") return bar.close;
    if(field=="average") return (bar.open+bar.close)/2.0;
    return std::isfinite(bar.vwap) && bar.vwap>0.0 ? bar.vwap : bar.close;
}

void select_analysis_price(std::span<options::data::Bar> bars,const QString& field) {
    for(auto& bar:bars) bar.vwap=analysis_price(bar,field);
}

class TimelineChartView final : public QChartView {
public:
    explicit TimelineChartView(std::function<void(int)> shift_window)
        : shift_window_(std::move(shift_window)) {
        setMouseTracking(true);
        viewport()->setMouseTracking(true);
    }

    void set_candles(QAbstractSeries* series,std::span<const ChartCandle> candles) {
        series_=series;
        candles_.assign(candles.begin(),candles.end());
        hovering_=false;
        viewport()->update();
    }

protected:
    void mouseMoveEvent(QMouseEvent* event) override {
        QChartView::mouseMoveEvent(event);
        const auto plot=chart()->plotArea();
        if(series_ && !candles_.empty() && plot.contains(event->position())) {
            hover_position_=event->position();
            hovering_=true;
        } else {
            hovering_=false;
        }
        viewport()->update();
    }

    void leaveEvent(QEvent* event) override {
        hovering_=false;
        viewport()->update();
        QChartView::leaveEvent(event);
    }

    void paintEvent(QPaintEvent* event) override {
        QChartView::paintEvent(event);
        if(!hovering_ || !series_ || candles_.empty()) return;

        const auto plot=chart()->plotArea();
        const auto cursor_x=qBound(plot.left(),hover_position_.x(),plot.right());
        const auto chart_value=chart()->mapToValue(
            QPointF(cursor_x,plot.center().y()),series_);
        const auto candle_index=static_cast<std::size_t>(qBound<qint64>(0,
            static_cast<qint64>(std::llround(chart_value.x())),
            static_cast<qint64>(candles_.size()-1)));
        const auto& candle=candles_[candle_index];
        const auto guide_x=qBound(plot.left(),chart()->mapToPosition(
            QPointF(static_cast<qreal>(candle_index),candle.close),series_).x(),plot.right());

        const QStringList lines{
            "Time (ET): "+candle.timestamp.toString("yyyy-MM-dd HH:mm:ss t"),
            "Open: $"+QString::number(candle.open,'f',2),
            "High: $"+QString::number(candle.high,'f',2),
            "Low: $"+QString::number(candle.low,'f',2),
            "Close: $"+QString::number(candle.close,'f',2),
            "Volume: "+QString::number(candle.volume),
        };

        QPainter painter(viewport());
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(QColor("#777777"),1,Qt::DashLine));
        painter.drawLine(QPointF(guide_x,plot.top()),QPointF(guide_x,plot.bottom()));

        const QFontMetrics metrics(painter.font());
        int text_width=0;
        for(const auto& line:lines) text_width=qMax(text_width,metrics.horizontalAdvance(line));
        constexpr int horizontal_padding=12;
        constexpr int vertical_padding=9;
        const auto box_width=text_width+horizontal_padding*2;
        const auto box_height=metrics.height()*lines.size()+vertical_padding*2;
        const auto box_x=qMax(8.0,guide_x-box_width-12.0);
        const auto box_y=qBound(8.0,plot.top()+12.0,
            qMax(8.0,static_cast<double>(viewport()->height()-box_height-8)));
        const QRectF box(box_x,box_y,box_width,box_height);
        painter.setPen(QPen(QColor("#a8a8a8")));
        painter.setBrush(QColor("#e8e8e8"));
        painter.drawRoundedRect(box,5,5);
        painter.setPen(QColor("#202020"));
        auto baseline=box.top()+vertical_padding+metrics.ascent();
        for(const auto& line:lines) {
            painter.drawText(QPointF(box.left()+horizontal_padding,baseline),line);
            baseline+=metrics.height();
        }
    }

    void wheelEvent(QWheelEvent* event) override {
        if(event->modifiers().testFlag(Qt::ControlModifier)) {
            const auto delta=event->angleDelta().y()!=0 ? event->angleDelta().y()
                                                       : event->angleDelta().x();
            if(delta!=0) {
                const auto direction=delta>0 ? -1 : 1;
                if(direction!=pending_direction_) {
                    pending_direction_=direction;
                    pending_events_=0;
                }
                if(++pending_events_==5) {
                    shift_window_(direction);
                    pending_events_=0;
                }
            }
            event->accept();
            return;
        }
        QChartView::wheelEvent(event);
    }

private:
    std::function<void(int)> shift_window_;
    std::vector<ChartCandle> candles_;
    QAbstractSeries* series_{};
    QPointF hover_position_;
    bool hovering_{};
    int pending_direction_{};
    int pending_events_{};
};

class IntradayHistogramWidget final : public QWidget {
public:
    explicit IntradayHistogramWidget(QWidget* parent=nullptr):QWidget(parent) {
        setMinimumHeight(230);
        setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Expanding);
    }

    void set_histogram(const options::analysis::IntradayMinuteHistogram& histogram,
                       QString title,QColor color) {
        histogram_=histogram;
        title_=std::move(title);
        color_=color;
        highlighted_minutes_.clear();
        update();
    }

    void set_highlighted_minutes(std::vector<int> minutes) {
        highlighted_minutes_=std::move(minutes);
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing,false);
        painter.fillRect(rect(),palette().base());
        constexpr int left=55;
        constexpr int right=18;
        constexpr int top=34;
        constexpr int bottom=34;
        const QRectF plot(left,top,std::max(1,width()-left-right),
            std::max(1,height()-top-bottom));
        painter.setPen(palette().text().color());
        painter.drawText(QRectF(left,5,plot.width(),22),Qt::AlignLeft|Qt::AlignVCenter,
            title_+"  ("+QString::number(histogram_.total_occurrences)+" observations)");
        painter.setPen(QColor(135,135,135));
        painter.drawLine(plot.bottomLeft(),plot.bottomRight());
        painter.drawLine(plot.bottomLeft(),plot.topLeft());
        if(histogram_.peak_count>0) {
            const auto bin_width=plot.width()/options::analysis::intraday_regular_session_minutes;
            painter.setPen(Qt::NoPen);
            painter.setBrush(color_);
            for(int minute=0;minute<options::analysis::intraday_regular_session_minutes;++minute) {
                const auto count=histogram_.counts[static_cast<std::size_t>(minute)];
                if(count==0) continue;
                const auto height=plot.height()*static_cast<double>(count)/histogram_.peak_count;
                painter.drawRect(QRectF(plot.left()+minute*bin_width,plot.bottom()-height,
                    std::max(1.0,bin_width),height));
            }
            painter.setBrush(QColor(255,140,0));
            for(const auto minute:highlighted_minutes_) {
                if(minute<0 || minute>=options::analysis::intraday_regular_session_minutes) continue;
                const auto count=histogram_.counts[static_cast<std::size_t>(minute)];
                const auto height=count==0 ? plot.height()*.04
                    : plot.height()*static_cast<double>(count)/histogram_.peak_count;
                painter.drawRect(QRectF(plot.left()+minute*bin_width,plot.bottom()-height,
                    std::max(2.0,bin_width),height));
            }
        }
        painter.setPen(palette().text().color());
        const std::array<std::pair<int,const char*>,6> ticks{{
            {0,"9:30"},{90,"11:00"},{180,"12:30"},{270,"2:00"},{360,"3:30"},{390,"4:00"}}};
        for(const auto& [minute,label]:ticks) {
            const auto x=plot.left()+plot.width()*minute/
                options::analysis::intraday_regular_session_minutes;
            painter.drawLine(QPointF(x,plot.bottom()),QPointF(x,plot.bottom()+4));
            painter.drawText(QRectF(x-24,plot.bottom()+6,48,18),Qt::AlignHCenter,label);
        }
        painter.drawText(QRectF(2,plot.top()-8,left-8,18),Qt::AlignRight,
            QString::number(histogram_.peak_count));
        painter.drawText(QRectF(2,plot.bottom()-9,left-8,18),Qt::AlignRight,"0");
    }

private:
    options::analysis::IntradayMinuteHistogram histogram_;
    QString title_;
    QColor color_{70,130,180};
    std::vector<int> highlighted_minutes_;
};

const std::array<QColor,5>& intraday_level_colors() {
    static const std::array<QColor,5> colors{
        QColor(235,195,30),QColor(55,165,85),QColor(45,115,210),
        QColor(135,75,190),QColor(225,85,155)};
    return colors;
}

class IntradayMetricHoverLabel final : public QLabel {
public:
    IntradayMetricHoverLabel(QString text,QColor color,QWidget* parent=nullptr)
        :QLabel(std::move(text),parent) {
        setAlignment(Qt::AlignCenter);
        setMinimumWidth(48);
        setContentsMargins(7,4,7,4);
        setStyleSheet("QLabel { background: "+color.name()+
            "; color: black; border: 1px solid rgba(0,0,0,80); border-radius: 3px; }");
    }

    std::function<void(bool)> hovered;

protected:
    void enterEvent(QEnterEvent* event) override {
        QLabel::enterEvent(event);
        if(hovered) hovered(true);
    }

    void leaveEvent(QEvent* event) override {
        QLabel::leaveEvent(event);
        if(hovered) hovered(false);
    }
};

class IntradayDayChartWidget final : public QWidget {
public:
    explicit IntradayDayChartWidget(
        const options::analysis::IntradaySessionRecord& session,QWidget* parent=nullptr)
        :QWidget(parent),session_(session) {
        setMinimumHeight(540);
        setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Expanding);
    }

    void set_hovered_membership(bool downside,int level) {
        hovered_membership_=std::pair{downside,level};
        update();
    }

    void clear_hovered_membership() {
        hovered_membership_.reset();
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing,true);
        painter.fillRect(rect(),palette().base());
        constexpr int left=72;
        constexpr int right=24;
        constexpr int top=24;
        constexpr int bottom=42;
        const QRectF plot(left,top,std::max(1,width()-left-right),
            std::max(1,height()-top-bottom));
        const auto price_span=std::max(1e-9,session_.session_high-session_.session_low);
        const auto price_min=session_.session_low-price_span*.03;
        const auto price_max=session_.session_high+price_span*.03;
        const auto price_y=[&](double price) {
            return plot.bottom()-(price-price_min)/(price_max-price_min)*plot.height();
        };
        const auto bin_width=plot.width()/options::analysis::intraday_regular_session_minutes;

        painter.setPen(QPen(QColor(205,205,205),1,Qt::DashLine));
        for(int tick=0;tick<=4;++tick) {
            const auto price=price_min+(price_max-price_min)*tick/4.0;
            const auto y=price_y(price);
            painter.drawLine(QPointF(plot.left(),y),QPointF(plot.right(),y));
            painter.setPen(palette().text().color());
            painter.drawText(QRectF(2,y-9,left-8,18),Qt::AlignRight,
                QString::number(price,'f',2));
            painter.setPen(QPen(QColor(205,205,205),1,Qt::DashLine));
        }

        if(hovered_membership_) {
            const auto [downside,level]=*hovered_membership_;
            const auto& membership=downside
                ? session_.downside[static_cast<std::size_t>(level)]
                : session_.upside[static_cast<std::size_t>(level)];
            auto shade=intraday_level_colors()[static_cast<std::size_t>(level)];
            shade.setAlpha(72);
            painter.setPen(Qt::NoPen);
            painter.setBrush(shade);
            const auto threshold_y=price_y(membership.price_threshold);
            for(const auto& range:membership.ranges) {
                const auto x=plot.left()+range.first_minute*bin_width;
                const auto range_width=(range.last_minute-range.first_minute+1)*bin_width;
                const QRectF shaded=downside
                    ? QRectF(x,threshold_y,range_width,plot.bottom()-threshold_y)
                    : QRectF(x,plot.top(),range_width,threshold_y-plot.top());
                painter.drawRect(shaded.normalized());
            }
        }

        for(std::size_t level=0;level<intraday_level_colors().size();++level) {
            auto low_pen=QPen(intraday_level_colors()[level],1.2,Qt::SolidLine);
            low_pen.setCosmetic(true);
            painter.setPen(low_pen);
            const auto low_y=price_y(session_.downside[level].price_threshold);
            painter.drawLine(QPointF(plot.left(),low_y),QPointF(plot.right(),low_y));
            auto high_pen=QPen(intraday_level_colors()[level],1.2,Qt::DashLine);
            high_pen.setCosmetic(true);
            painter.setPen(high_pen);
            const auto high_y=price_y(session_.upside[level].price_threshold);
            painter.drawLine(QPointF(plot.left(),high_y),QPointF(plot.right(),high_y));
        }

        painter.setRenderHint(QPainter::Antialiasing,false);
        for(const auto& candle:session_.minutes) {
            const auto x=plot.left()+(candle.minute_of_session+.5)*bin_width;
            const auto rising=candle.close>=candle.open;
            const auto color=rising ? QColor(35,155,85) : QColor(205,65,65);
            painter.setPen(QPen(color,1));
            painter.drawLine(QPointF(x,price_y(candle.high)),QPointF(x,price_y(candle.low)));
            const auto open_y=price_y(candle.open);
            const auto close_y=price_y(candle.close);
            painter.setBrush(color);
            painter.setPen(Qt::NoPen);
            painter.drawRect(QRectF(x-std::max(0.6,bin_width*.32),
                std::min(open_y,close_y),std::max(1.2,bin_width*.64),
                std::max(1.0,std::abs(open_y-close_y))));
        }

        painter.setRenderHint(QPainter::Antialiasing,true);
        painter.setBrush(QColor(20,125,220));
        painter.setPen(QPen(Qt::white,1));
        for(const auto minute:session_.low_minutes) {
            const auto x=plot.left()+(minute+.5)*bin_width;
            painter.drawEllipse(QPointF(x,price_y(session_.session_low)),4,4);
        }
        painter.setBrush(QColor(225,70,125));
        for(const auto minute:session_.high_minutes) {
            const auto x=plot.left()+(minute+.5)*bin_width;
            painter.drawEllipse(QPointF(x,price_y(session_.session_high)),4,4);
        }

        painter.setPen(palette().text().color());
        painter.drawLine(plot.bottomLeft(),plot.bottomRight());
        const std::array<std::pair<int,const char*>,6> ticks{{
            {0,"9:30"},{90,"11:00"},{180,"12:30"},{270,"2:00"},{360,"3:30"},{390,"4:00"}}};
        for(const auto& [minute,label]:ticks) {
            const auto x=plot.left()+plot.width()*minute/
                options::analysis::intraday_regular_session_minutes;
            painter.drawLine(QPointF(x,plot.bottom()),QPointF(x,plot.bottom()+4));
            painter.drawText(QRectF(x-25,plot.bottom()+7,50,18),Qt::AlignHCenter,label);
        }
    }

private:
    options::analysis::IntradaySessionRecord session_;
    std::optional<std::pair<bool,int>> hovered_membership_;
};

class IntradayLedgerTable final : public QTableWidget {
public:
    using QTableWidget::QTableWidget;
    std::function<void()> mouse_left;

protected:
    void leaveEvent(QEvent* event) override {
        QTableWidget::leaveEvent(event);
        clearSelection();
        if(mouse_left) mouse_left();
    }
};

class ResultsWindow final : public QMainWindow {
public:
    ResultsWindow() {
        setWindowTitle("Options Backtester");
        resize(1250,850);
        auto* central=new QWidget;
        auto* page=new QVBoxLayout(central);

        auto* open_button=new QPushButton("Open Database…");
        auto* tabs=new QTabWidget;
        auto* stock_page=new QWidget;
        auto* stock_layout=new QVBoxLayout(stock_page);
        auto* stock_controls=new QHBoxLayout;
        stock_symbol_=new QComboBox;
        strategy_=new QComboBox;
        strategy_->addItems({"Select strategy…","Momentum","Intraday Distribution"});
        candle_interval_=new QComboBox;
        candle_interval_->addItem("1 Day",0);
        candle_interval_->addItem("1 Hour",60);
        candle_interval_->addItem("30 Min",30);
        candle_interval_->addItem("15 Min",15);
        candle_interval_->addItem("5 Min",5);
        candle_interval_->addItem("1 Min",1);
        stock_start_=new QDateEdit; stock_end_=new QDateEdit;
        stock_start_->setCalendarPopup(true); stock_end_->setCalendarPopup(true);
        stock_controls->addWidget(new QLabel("Symbol:")); stock_controls->addWidget(stock_symbol_);
        stock_controls->addWidget(new QLabel("Start:")); stock_controls->addWidget(stock_start_);
        stock_controls->addWidget(new QLabel("End:")); stock_controls->addWidget(stock_end_);
        stock_controls->addWidget(new QLabel("Strategy:")); stock_controls->addWidget(strategy_);
        stock_controls->addStretch();
        stock_controls->addWidget(new QLabel("Candles:"));
        stock_controls->addWidget(candle_interval_);
        stock_controls->addWidget(open_button);
        stock_layout->addLayout(stock_controls);
        auto* saved_studies_row=new QHBoxLayout;
        saved_studies_row->addWidget(new QLabel("Saved studies:"));
        saved_studies_=new QComboBox;
        saved_studies_->setMinimumWidth(260);
        load_study_button_=new QPushButton("Load");
        delete_study_button_=new QPushButton("Delete");
        saved_studies_row->addWidget(saved_studies_);
        saved_studies_row->addWidget(load_study_button_);
        saved_studies_row->addWidget(delete_study_button_);
        saved_studies_row->addStretch();
        stock_layout->addLayout(saved_studies_row);
        stock_chart_view_=new TimelineChartView([this](int direction){ shift_date_window(direction); });
        stock_chart_view_->setRenderHint(QPainter::Antialiasing);
        stock_layout->addWidget(stock_chart_view_,1);
        tabs->addTab(stock_page,"Underlying Price");

        auto* load_page=new QWidget;
        auto* load_layout=new QVBoxLayout(load_page);
        auto* load_controls=new QHBoxLayout;
        load_symbol_=new QLineEdit;
        load_symbol_->setPlaceholderText("e.g. AAPL");
        load_symbol_->setMaximumWidth(160);
        load_start_=new QDateEdit(QDate::currentDate().addYears(-1));
        load_end_=new QDateEdit(QDate::currentDate());
        load_start_->setCalendarPopup(true); load_end_->setCalendarPopup(true);
        load_start_->setDateRange(QDate(1970,1,1),QDate::currentDate());
        load_end_->setDateRange(QDate(1970,1,1),QDate::currentDate());
        load_button_=new QPushButton("Load Data");
        load_controls->addWidget(new QLabel("Symbol:")); load_controls->addWidget(load_symbol_);
        load_controls->addWidget(new QLabel("Start:")); load_controls->addWidget(load_start_);
        load_controls->addWidget(new QLabel("End:")); load_controls->addWidget(load_end_);
        load_controls->addWidget(load_button_); load_controls->addStretch();
        load_layout->addLayout(load_controls);
        load_status_=new QLabel("Enter a symbol to load its available Alpaca IEX one-minute history.");
        load_status_->setWordWrap(true);
        load_layout->addWidget(load_status_);
        load_layout->addStretch();
        tabs->addTab(load_page,"Load Data");

        page->addWidget(tabs,1);
        setCentralWidget(central);

        connect(open_button,&QPushButton::clicked,this,[this]{ choose_database(); });
        connect(stock_symbol_,&QComboBox::currentTextChanged,this,[this]{ update_stock_range(); });
        const auto dates_changed=[this] {
            persist_dates();
            if(!candle_interval_overridden_) select_default_candle_interval();
            plot_underlying();
        };
        connect(stock_start_,&QDateEdit::dateChanged,this,dates_changed);
        connect(stock_end_,&QDateEdit::dateChanged,this,dates_changed);
        connect(candle_interval_,&QComboBox::activated,this,[this](int) {
            candle_interval_overridden_=true;
            plot_underlying();
        });
        connect(strategy_,&QComboBox::activated,this,[this](int index){
            if(index==1) show_momentum_analysis();
            else if(index==2) show_intraday_distribution_analysis();
            strategy_->setCurrentIndex(0);
        });
        connect(load_button_,&QPushButton::clicked,this,[this]{ load_symbol_data(); });
        connect(load_symbol_,&QLineEdit::returnPressed,this,[this]{ load_symbol_data(); });
        connect(load_study_button_,&QPushButton::clicked,this,[this]{ load_selected_study(); });
        connect(delete_study_button_,&QPushButton::clicked,this,[this]{ delete_selected_study(); });
        load_watcher_=new QFutureWatcher<LoadResult>(this);
        connect(load_watcher_,&QFutureWatcher<LoadResult>::finished,this,[this]{ finish_symbol_load(); });
        open_database(initial_database_path());
        refresh_saved_studies();
    }

private:
    void choose_database() {
        const auto selected=QFileDialog::getOpenFileName(this,"Open market data database",
            QFileInfo(database_path_).absolutePath(),"SQLite databases (*.db);;All files (*)");
        if(!selected.isEmpty()) open_database(selected);
    }

    void open_database(const QString& path) {
        try {
            database_path_=QFileInfo(path).absoluteFilePath();
            bar_store_=std::make_unique<options::data::SqliteBarStore>(database_path_.toStdString());
            intraday_study_cache_.clear();
            QSettings("OptionsBacktester","OptionsBacktester").setValue("database",database_path_);
            load_stock_symbols();
        } catch(const std::exception& error) {
            QMessageBox::critical(this,"Open Database",error.what());
        }
    }

    void update_stock_range() {
        if(!bar_store_ || stock_symbol_->currentText().isEmpty()) return;
        const options::data::BarQuery query{stock_symbol_->currentText().toStdString(),"alpaca",
            "iex",options::data::stock_bar_timeframe,"all","",""};
        const auto range=bar_store_->date_range(query);
        const auto first=QDate::fromString(QString::fromStdString(range.start),Qt::ISODate);
        const auto last=QDate::fromString(QString::fromStdString(range.end),Qt::ISODate);
        const bool available=first.isValid() && last.isValid();
        if(!available) {
            return;
        }
        available_start_=first;
        available_end_=last;
        const QSignalBlocker block_start(stock_start_);
        const QSignalBlocker block_end(stock_end_);
        stock_start_->setDateRange(first,last); stock_end_->setDateRange(first,last);
        auto selected_start=first;
        auto selected_end=last;
        QSettings settings("OptionsBacktester","OptionsBacktester");
        settings.beginGroup("underlying_price");
        settings.beginGroup(stock_symbol_->currentText());
        const auto saved_start=QDate::fromString(settings.value("start_date").toString(),Qt::ISODate);
        const auto saved_end=QDate::fromString(settings.value("end_date").toString(),Qt::ISODate);
        if(saved_start.isValid()) selected_start=qBound(first,saved_start,last);
        if(saved_end.isValid()) selected_end=qBound(first,saved_end,last);
        if(selected_start>selected_end) {
            selected_start=first;
            selected_end=last;
        }
        stock_start_->setDate(selected_start); stock_end_->setDate(selected_end);
        candle_interval_overridden_=false;
        select_default_candle_interval();
        plot_underlying();
    }

    void select_default_candle_interval() {
        const auto minutes=stock_start_->date()<stock_end_->date() ? 0 : 1;
        const auto index=candle_interval_->findData(minutes);
        if(index>=0) candle_interval_->setCurrentIndex(index);
    }

    void load_stock_symbols() {
        if(!bar_store_) return;
        const auto previous=stock_symbol_->currentText();
        stock_symbol_->blockSignals(true); stock_symbol_->clear();
        for(const auto& symbol:bar_store_->available_symbols(
                "alpaca","iex",options::data::stock_bar_timeframe,"all"))
            stock_symbol_->addItem(QString::fromStdString(symbol));
        const auto previous_index=stock_symbol_->findText(previous);
        if(previous_index>=0) stock_symbol_->setCurrentIndex(previous_index);
        stock_symbol_->blockSignals(false);
        if(stock_symbol_->count()==0) {
            return;
        }
        update_stock_range();
    }

    void shift_date_window(int direction) {
        const auto start=stock_start_->date();
        const auto end=stock_end_->date();
        if(!available_start_.isValid() || !available_end_.isValid() || start>end) return;

        const auto window_days=start.daysTo(end)+1;
        auto shift=direction*qMax(1,window_days/10);
        if(start.addDays(shift)<available_start_) shift=start.daysTo(available_start_);
        if(end.addDays(shift)>available_end_) shift=end.daysTo(available_end_);
        if(shift==0) return;

        const QSignalBlocker block_start(stock_start_);
        const QSignalBlocker block_end(stock_end_);
        stock_start_->setDate(start.addDays(shift));
        stock_end_->setDate(end.addDays(shift));
        persist_dates();
        plot_underlying();
    }

    void persist_dates() const {
        if(stock_symbol_->currentText().isEmpty()) return;
        QSettings settings("OptionsBacktester","OptionsBacktester");
        settings.beginGroup("underlying_price");
        settings.beginGroup(stock_symbol_->currentText());
        settings.setValue("start_date",stock_start_->date().toString(Qt::ISODate));
        settings.setValue("end_date",stock_end_->date().toString(Qt::ISODate));
    }

    void refresh_saved_studies(const QString& preferred_id={}) {
        const auto previous_id=preferred_id.isEmpty()
            ? saved_studies_->currentData().toString() : preferred_id;
        const QSignalBlocker blocker(saved_studies_);
        saved_studies_->clear();
        const auto presets=load_study_presets();
        if(presets.empty()) {
            saved_studies_->addItem("No saved studies");
            saved_studies_->setEnabled(false);
            load_study_button_->setEnabled(false);
            delete_study_button_->setEnabled(false);
            return;
        }
        saved_studies_->setEnabled(true);
        load_study_button_->setEnabled(true);
        delete_study_button_->setEnabled(true);
        for(const auto& preset:presets) {
            saved_studies_->addItem(preset.name+" — "+preset.symbol,preset.id);
            saved_studies_->setItemData(saved_studies_->count()-1,
                "Momentum study for "+preset.symbol,Qt::ToolTipRole);
        }
        const auto previous_index=saved_studies_->findData(previous_id);
        if(previous_index>=0) saved_studies_->setCurrentIndex(previous_index);
    }

    void load_selected_study() {
        const auto id=saved_studies_->currentData().toString();
        const auto studies=load_study_presets();
        const auto found=std::ranges::find_if(studies,[&id](const auto& value) {
            return value.id==id;
        });
        if(found==studies.end()) {
            QMessageBox::warning(this,"Load Study","This saved study no longer exists.");
            refresh_saved_studies(id);
            return;
        }
        if(!found->symbol.isEmpty() && found->symbol!=stock_symbol_->currentText()) {
            const auto symbol_index=stock_symbol_->findText(found->symbol);
            if(symbol_index<0) {
                QMessageBox::information(this,"Load Study",
                    "The saved ticker "+found->symbol+
                    " has no stored IEX one-minute bars in this database.");
                return;
            }
            stock_symbol_->setCurrentIndex(symbol_index);
        }
        show_momentum_analysis(*found);
    }

    void delete_selected_study() {
        const auto id=saved_studies_->currentData().toString();
        const auto studies=load_study_presets();
        const auto found=std::ranges::find_if(studies,[&id](const auto& value) {
            return value.id==id;
        });
        if(found==studies.end()) {
            refresh_saved_studies();
            return;
        }
        if(QMessageBox::question(this,"Delete Saved Study",
            "Delete the saved study \""+found->name+"\"? This cannot be undone.")!=QMessageBox::Yes)
            return;
        delete_study_preset(id);
        refresh_saved_studies();
    }


    void show_intraday_day_chart(
        const options::analysis::IntradaySessionRecord& session) {
        QDialog dialog(this);
        dialog.setWindowTitle("Intraday Distribution Day — "+
            QString::fromStdString(session.date));
        dialog.setWindowFlag(Qt::WindowMaximizeButtonHint,true);
        dialog.setMinimumSize(1050,700);
        const auto available_size=dialog.screen()->availableGeometry().size();
        dialog.resize(qMin(1500,available_size.width()-40),
            qMin(900,available_size.height()-40));
        auto* layout=new QVBoxLayout(&dialog);
        auto* summary=new QLabel(
            "OHLC  "+QString::number(session.minutes.front().open,'f',2)+" / "+
            QString::number(session.session_high,'f',2)+" / "+
            QString::number(session.session_low,'f',2)+" / "+
            QString::number(session.minutes.back().close,'f',2)+
            "    Blue marker: session low    Pink marker: session high    "
            "Solid line: downside threshold    Dashed line: upside threshold");
        summary->setWordWrap(true);
        layout->addWidget(summary);

        auto* chart=new IntradayDayChartWidget(session);
        auto* hover_controls=new QHBoxLayout;
        hover_controls->addWidget(new QLabel("Hover downside:"));
        for(std::size_t level=0;level<options::analysis::intraday_distribution_percentages.size();++level) {
            const auto percentage=options::analysis::intraday_distribution_percentages[level];
            auto* label=new IntradayMetricHoverLabel(
                QString::number(percentage)+"%",intraday_level_colors()[level]);
            label->setToolTip("Shade the lowest "+QString::number(percentage)+
                "% of one-minute candle averages below $"+
                QString::number(session.downside[level].price_threshold,'f',2));
            label->hovered=[chart,level](bool entered) {
                if(entered) chart->set_hovered_membership(true,static_cast<int>(level));
                else chart->clear_hovered_membership();
            };
            hover_controls->addWidget(label);
        }
        hover_controls->addSpacing(20);
        hover_controls->addWidget(new QLabel("Hover upside:"));
        for(std::size_t level=0;level<options::analysis::intraday_distribution_percentages.size();++level) {
            const auto percentage=options::analysis::intraday_distribution_percentages[level];
            auto* label=new IntradayMetricHoverLabel(
                QString::number(percentage)+"%",intraday_level_colors()[level]);
            label->setToolTip("Shade the highest "+QString::number(percentage)+
                "% of one-minute candle averages above $"+
                QString::number(session.upside[level].price_threshold,'f',2));
            label->hovered=[chart,level](bool entered) {
                if(entered) chart->set_hovered_membership(false,static_cast<int>(level));
                else chart->clear_hovered_membership();
            };
            hover_controls->addWidget(label);
        }
        hover_controls->addStretch();
        layout->addLayout(hover_controls);
        layout->addWidget(chart,1);
        auto* hint=new QLabel(
            "Shading is generated only for the hovered level and only across its qualifying "
            "piecewise one-minute ranges. This day chart is created on demand from the "
            "double-clicked ledger record.");
        hint->setWordWrap(true);
        layout->addWidget(hint);
        dialog.exec();
    }

    void show_intraday_distribution_analysis() {
        const auto symbol=stock_symbol_->currentText();
        if(symbol.isEmpty() || !available_start_.isValid() || !available_end_.isValid()) {
            QMessageBox::information(this,"Intraday Distribution",
                "Select a symbol with stored one-minute bars first.");
            return;
        }

        QDialog dialog(this);
        dialog.setWindowTitle("Intraday Distribution — "+symbol);
        dialog.setWindowFlag(Qt::WindowMaximizeButtonHint,true);
        dialog.setSizeGripEnabled(true);
        dialog.setMinimumSize(1050,720);
        const auto available_size=dialog.screen()->availableGeometry().size();
        dialog.resize(qMin(1450,available_size.width()-40),
            qMin(900,available_size.height()-40));
        auto* layout=new QVBoxLayout(&dialog);
        auto* description=new QLabel(
            "The strategy always analyzes complete 390-candle regular sessions at one-minute "
            "resolution. Its Monday–Friday pages are contained inside this strategy window. "
            "Hover a ledger row to highlight that session's contribution to the selected histogram.");
        description->setWordWrap(true);
        layout->addWidget(description);

        auto* controls=new QHBoxLayout;
        auto* ticker=new QComboBox;
        for(int index=0;index<stock_symbol_->count();++index)
            ticker->addItem(stock_symbol_->itemText(index));
        ticker->setCurrentText(symbol);
        auto* start=new QDateEdit(qMax(available_start_,available_end_.addYears(-1)));
        auto* end=new QDateEdit(available_end_);
        start->setCalendarPopup(true);
        end->setCalendarPopup(true);
        start->setDateRange(available_start_,available_end_);
        end->setDateRange(available_start_,available_end_);
        auto* run=new QPushButton("Run Study");
        controls->addWidget(new QLabel("Ticker:"));
        controls->addWidget(ticker);
        controls->addWidget(new QLabel("Start:"));
        controls->addWidget(start);
        controls->addWidget(new QLabel("End:"));
        controls->addWidget(end);
        controls->addWidget(run);
        controls->addStretch();
        layout->addLayout(controls);

        auto* weekday_tabs=new QTabWidget;
        layout->addWidget(weekday_tabs,1);
        auto* status=new QLabel;
        status->setWordWrap(true);
        layout->addWidget(status);

        auto* watcher=new QFutureWatcher<IntradayDistributionOutput>(&dialog);
        const auto minute_text=[](int minute) {
            return QTime(9,30).addSecs(minute*60).toString("h:mm AP");
        };
        const auto minute_list_text=[minute_text](const std::vector<int>& minutes) {
            QStringList values;
            for(const auto minute:minutes) values.push_back(minute_text(minute));
            return values.join(", ");
        };
        const auto membership_text=[](const options::analysis::IntradayQuantileMembership& value) {
            return QString::number(value.minutes.size())+" min / "+
                QString::number(value.ranges.size())+" segment(s)";
        };
        const auto number_item=[](double value) {
            auto* item=new QTableWidgetItem(QString::number(value,'f',2));
            item->setData(Qt::UserRole,value);
            return item;
        };

        connect(watcher,&QFutureWatcher<IntradayDistributionOutput>::finished,&dialog,[&] {
            run->setEnabled(true);
            ticker->setEnabled(true);
            start->setEnabled(true);
            end->setEnabled(true);
            auto output=watcher->result();
            if(!output.error.isEmpty()) {
                status->setText("Study failed: "+output.error);
                return;
            }
            if(!output.study) {
                status->setText("Study failed: no analysis result was produced.");
                return;
            }
            if(!output.cached) {
                if(intraday_study_cache_.size()>=6)
                    intraday_study_cache_.erase(intraday_study_cache_.begin());
                intraday_study_cache_[output.cache_key]=output.study;
            }
            while(weekday_tabs->count()>0) delete weekday_tabs->widget(0);
            auto study=output.study;
            auto analysis=std::shared_ptr<options::analysis::IntradayDistributionResult>(
                study,&study->result);
            auto weekdays=std::shared_ptr<std::array<
                options::analysis::IntradayWeekdayAggregation,5>>(study,&study->weekdays);
            static const QStringList weekday_names{
                "Monday","Tuesday","Wednesday","Thursday","Friday"};

            auto* summary_page=new QWidget;
            auto* summary_layout=new QVBoxLayout(summary_page);
            const auto& weekly=study->weekly_summary;
            auto* summary_heading=new QLabel(
                "Study range: $"+QString::number(weekly.study_minimum,'f',2)+" minimum to $"+
                QString::number(weekly.study_maximum,'f',2)+" maximum    Weeks analyzed: "+
                QString::number(weekly.weeks_analyzed)+"    Holiday/partial weeks: "+
                QString::number(weekly.partial_weeks));
            summary_heading->setWordWrap(true);
            summary_layout->addWidget(summary_heading);
            auto* rank_explanation=new QLabel(
                "Low rank 1 is the lowest low among the weekdays present that week. High rank 1 "
                "is the highest high among those weekdays. Holiday and partial weeks are included; "
                "their ranks run from 1 through the number of available sessions. Ties share their "
                "average rank and count as wins for every tied day.");
            rank_explanation->setWordWrap(true);
            summary_layout->addWidget(rank_explanation);
            auto* summary_table=new QTableWidget(5,8);
            summary_table->setHorizontalHeaderLabels({"Weekday","Weeks represented","Average low rank",
                "Weekly-low wins","Low win rate","Average high rank","Weekly-high wins",
                "High win rate"});
            summary_table->setAlternatingRowColors(true);
            summary_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
            summary_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
            for(int row=0;row<5;++row) {
                const auto& weekday=weekly.weekdays[static_cast<std::size_t>(row)];
                summary_table->setItem(row,0,new QTableWidgetItem(weekday_names[row]));
                const auto rank_text=[&](double value) {
                    return weekday.weeks_participated==0
                        ? QString("—") : QString::number(value,'f',2);
                };
                summary_table->setItem(row,1,new QTableWidgetItem(
                    QString::number(weekday.weeks_participated)));
                summary_table->setItem(row,2,new QTableWidgetItem(
                    rank_text(weekday.average_low_rank)));
                summary_table->setItem(row,3,new QTableWidgetItem(
                    QString::number(weekday.weekly_low_wins)));
                summary_table->setItem(row,4,new QTableWidgetItem(
                    QString::number(weekday.weekly_low_win_percent,'f',1)+"%"));
                summary_table->setItem(row,5,new QTableWidgetItem(
                    rank_text(weekday.average_high_rank)));
                summary_table->setItem(row,6,new QTableWidgetItem(
                    QString::number(weekday.weekly_high_wins)));
                summary_table->setItem(row,7,new QTableWidgetItem(
                    QString::number(weekday.weekly_high_win_percent,'f',1)+"%"));
            }
            summary_layout->addWidget(summary_table);
            summary_layout->addStretch();
            weekday_tabs->addTab(summary_page,"Summary");

            auto histogram_selectors=std::make_shared<std::vector<QComboBox*>>();
            auto synchronized_histogram_index=std::make_shared<int>(0);
            auto synchronizing_histograms=std::make_shared<bool>(false);

            for(int weekday_index=0;weekday_index<5;++weekday_index) {
                auto* page=new QWidget;
                auto* page_layout=new QVBoxLayout(page);
                auto* histogram_controls=new QHBoxLayout;
                auto* histogram_choice=new QComboBox;
                histogram_choice->addItems({"Actual low occurrences","Actual high occurrences",
                    "Downside 50%","Downside 40%","Downside 30%","Downside 20%",
                    "Downside 10%","Upside 50%","Upside 40%","Upside 30%",
                    "Upside 20%","Upside 10%"});
                histogram_choice->setCurrentIndex(*synchronized_histogram_index);
                histogram_selectors->push_back(histogram_choice);
                histogram_controls->addWidget(new QLabel("Histogram:"));
                histogram_controls->addWidget(histogram_choice);
                histogram_controls->addStretch();
                histogram_controls->addWidget(new QLabel(
                    "Orange bars show the hovered ledger session."));
                page_layout->addLayout(histogram_controls);
                auto* histogram=new IntradayHistogramWidget;
                page_layout->addWidget(histogram,1);

                auto* ledger=new IntradayLedgerTable;
                ledger->setColumnCount(12);
                ledger->setHorizontalHeaderLabels({"Date","Open","High","Low","Close",
                    "Median","Low time(s)","High time(s)","Down 50%","Down 10%",
                    "Up 50%","Up 10%"});
                ledger->setAlternatingRowColors(true);
                ledger->setEditTriggers(QAbstractItemView::NoEditTriggers);
                ledger->setSelectionBehavior(QAbstractItemView::SelectRows);
                ledger->setMouseTracking(true);
                ledger->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
                ledger->horizontalHeader()->setStretchLastSection(true);
                std::vector<std::size_t> session_indices;
                for(std::size_t index=0;index<analysis->sessions.size();++index)
                    if(analysis->sessions[index].weekday==weekday_index+1)
                        session_indices.push_back(index);
                ledger->setRowCount(static_cast<int>(session_indices.size()));
                for(int row=0;row<ledger->rowCount();++row) {
                    const auto& session=analysis->sessions[session_indices[static_cast<std::size_t>(row)]];
                    ledger->setItem(row,0,new QTableWidgetItem(QString::fromStdString(session.date)));
                    ledger->setItem(row,1,number_item(session.minutes.front().open));
                    ledger->setItem(row,2,number_item(session.session_high));
                    ledger->setItem(row,3,number_item(session.session_low));
                    ledger->setItem(row,4,number_item(session.minutes.back().close));
                    ledger->setItem(row,5,number_item(session.downside[0].price_threshold));
                    ledger->setItem(row,6,new QTableWidgetItem(minute_list_text(session.low_minutes)));
                    ledger->setItem(row,7,new QTableWidgetItem(minute_list_text(session.high_minutes)));
                    ledger->setItem(row,8,new QTableWidgetItem(membership_text(session.downside[0])));
                    ledger->setItem(row,9,new QTableWidgetItem(membership_text(session.downside[4])));
                    ledger->setItem(row,10,new QTableWidgetItem(membership_text(session.upside[0])));
                    ledger->setItem(row,11,new QTableWidgetItem(membership_text(session.upside[4])));
                }
                page_layout->addWidget(ledger,2);

                const auto select_histogram=[histogram,histogram_choice,weekdays,weekday_index] {
                    const auto& weekday=(*weekdays)[static_cast<std::size_t>(weekday_index)];
                    const auto selection=histogram_choice->currentIndex();
                    if(selection==0)
                        histogram->set_histogram(weekday.lows,"Actual daily-low occurrences",
                            QColor(50,125,200));
                    else if(selection==1)
                        histogram->set_histogram(weekday.highs,"Actual daily-high occurrences",
                            QColor(185,80,115));
                    else if(selection<=6) {
                        const auto level=static_cast<std::size_t>(selection-2);
                        histogram->set_histogram(weekday.downside[level],
                            "Lowest "+QString::number(weekday.downside[level].percentage)+
                            "% candle membership",QColor(55,145,135));
                    } else {
                        const auto level=static_cast<std::size_t>(selection-7);
                        histogram->set_histogram(weekday.upside[level],
                            "Highest "+QString::number(weekday.upside[level].percentage)+
                            "% candle membership",QColor(145,90,180));
                    }
                };
                connect(histogram_choice,&QComboBox::currentIndexChanged,page,
                    [select_histogram](int){ select_histogram(); });
                connect(histogram_choice,&QComboBox::currentIndexChanged,page,
                    [histogram_selectors,synchronized_histogram_index,
                     synchronizing_histograms](int index) {
                    if(*synchronizing_histograms) return;
                    *synchronizing_histograms=true;
                    *synchronized_histogram_index=index;
                    for(auto* selector:*histogram_selectors)
                        if(selector->currentIndex()!=index) selector->setCurrentIndex(index);
                    *synchronizing_histograms=false;
                });
                select_histogram();

                connect(ledger,&QTableWidget::cellEntered,page,
                    [histogram,histogram_choice,analysis,session_indices,ledger](int row,int) {
                    if(row<0 || static_cast<std::size_t>(row)>=session_indices.size()) return;
                    ledger->selectRow(row);
                    const auto& session=analysis->sessions[
                        session_indices[static_cast<std::size_t>(row)]];
                    const auto selection=histogram_choice->currentIndex();
                    if(selection==0) histogram->set_highlighted_minutes(session.low_minutes);
                    else if(selection==1) histogram->set_highlighted_minutes(session.high_minutes);
                    else if(selection<=6) histogram->set_highlighted_minutes(
                        session.downside[static_cast<std::size_t>(selection-2)].minutes);
                    else histogram->set_highlighted_minutes(
                        session.upside[static_cast<std::size_t>(selection-7)].minutes);
                });
                connect(ledger,&QTableWidget::cellDoubleClicked,page,
                    [this,analysis,session_indices](int row,int) {
                    if(row<0 || static_cast<std::size_t>(row)>=session_indices.size()) return;
                    show_intraday_day_chart(analysis->sessions[
                        session_indices[static_cast<std::size_t>(row)]]);
                });
                ledger->mouse_left=[histogram] { histogram->set_highlighted_minutes({}); };
                weekday_tabs->addTab(page,weekday_names[weekday_index]+" ("+
                    QString::number(session_indices.size())+")");
            }
            status->setText(QString(output.cached ? "Loaded cached analysis. " : "Analysis cached. ")+
                "Accepted "+QString::number(analysis->sessions.size())+" of "+
                QString::number(analysis->candidate_sessions)+" candidate sessions. Excluded "+
                QString::number(analysis->excluded_incomplete_sessions)+
                " incomplete/short and "+QString::number(analysis->excluded_invalid_sessions)+
                " invalid/duplicate sessions.");
        });

        const auto update_range=[&](const QString& selected_symbol) {
            const auto range=bar_store_->date_range({selected_symbol.toStdString(),"alpaca",
                "iex",options::data::stock_bar_timeframe,"all","",""});
            const auto first=QDate::fromString(QString::fromStdString(range.start),Qt::ISODate);
            const auto last=QDate::fromString(QString::fromStdString(range.end),Qt::ISODate);
            if(!first.isValid() || !last.isValid()) return;
            const QSignalBlocker start_blocker(start);
            const QSignalBlocker end_blocker(end);
            start->setDateRange(first,last);
            end->setDateRange(first,last);
            start->setDate(qMax(first,last.addYears(-1)));
            end->setDate(last);
            dialog.setWindowTitle("Intraday Distribution — "+selected_symbol);
        };
        connect(ticker,&QComboBox::currentTextChanged,&dialog,update_range);

        const auto run_study=[&] {
            if(watcher->isRunning()) return;
            if(start->date()>end->date()) {
                status->setText("The start date must not be after the end date.");
                return;
            }
            run->setEnabled(false);
            ticker->setEnabled(false);
            start->setEnabled(false);
            end->setEnabled(false);
            while(weekday_tabs->count()>0) delete weekday_tabs->widget(0);
            status->setText("Building one-minute session records and weekday histograms…");
            const auto database_path=database_path_;
            const auto selected_symbol=ticker->currentText();
            const auto start_text=start->date().toString(Qt::ISODate);
            const auto end_text=end->date().toString(Qt::ISODate);
            const auto cache_key=database_path+"|"+selected_symbol+"|"+start_text+"|"+end_text;
            const auto cached=intraday_study_cache_.find(cache_key);
            if(cached!=intraday_study_cache_.end()) {
                status->setText("Loading cached session records, histograms, and summary…");
                const auto cached_study=cached->second;
                watcher->setFuture(QtConcurrent::run([cached_study,cache_key] {
                    IntradayDistributionOutput output;
                    output.study=cached_study;
                    output.cache_key=cache_key;
                    output.cached=true;
                    return output;
                }));
                return;
            }
            status->setText("Building one-minute session records, histograms, and summary…");
            watcher->setFuture(QtConcurrent::run(
                [database_path,selected_symbol,start_text,end_text,cache_key] {
                    IntradayDistributionOutput output;
                    output.cache_key=cache_key;
                    try {
                        options::data::SqliteBarStore store(database_path.toStdString());
                        const auto bars=store.load({selected_symbol.toStdString(),"alpaca","iex",
                            options::data::stock_bar_timeframe,"all",start_text.toStdString(),
                            end_text.toStdString()});
                        output.study=std::make_shared<CachedIntradayDistributionStudy>();
                        output.study->result=options::analysis::analyze_intraday_distribution(bars);
                        output.study->weekdays=options::analysis::aggregate_intraday_weekdays(
                            output.study->result.sessions);
                        output.study->weekly_summary=options::analysis::summarize_intraday_weeks(
                            output.study->result.sessions);
                    } catch(const std::exception& error) {
                        output.study.reset();
                        output.error=QString::fromUtf8(error.what());
                    }
                    return output;
                }));
        };
        connect(run,&QPushButton::clicked,&dialog,run_study);
        run_study();
        dialog.exec();
    }

    void show_momentum_analysis(std::optional<MomentumStudyPreset> preset=std::nullopt) {
        const auto symbol=stock_symbol_->currentText();
        if(symbol.isEmpty() || !available_start_.isValid() || !available_end_.isValid()) {
            QMessageBox::information(this,"Momentum Analysis",
                "Select a symbol with stored one-minute bars first.");
            return;
        }

        QDialog dialog(this);
        dialog.setWindowTitle("Momentum Analysis — "+symbol);
        dialog.setWindowFlag(Qt::WindowMaximizeButtonHint,true);
        dialog.setSizeGripEnabled(true);
        dialog.setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Expanding);
        dialog.setMaximumSize(QWIDGETSIZE_MAX,QWIDGETSIZE_MAX);
        dialog.setMinimumSize(900,660);
        const auto available_size=dialog.screen()->availableGeometry().size();
        dialog.resize(qMin(qMax(1040,available_size.width()*3/4),available_size.width()-40),
                      qMin(qMax(760,available_size.height()*4/5),available_size.height()-40));
        auto* layout=new QVBoxLayout(&dialog);
        layout->setSizeConstraint(QLayout::SetNoConstraint);
        auto* description=new QLabel(
            "For each eligible aggregated-bar price q, this compares the price at the first stored bar "
            "at or after q's timestamp plus x calendar days. The skip window d controls when the next q "
            "becomes eligible. Results are averaged across all d possible start phases. Optional "
            "strike adjustment compares r with a strike-grid price derived from q instead of q itself. "
            "The data resolution and OHLC/VWAP analysis field are selectable below.");
        description->setWordWrap(true);
        layout->addWidget(description);

        auto* controls_widget=new QWidget;
        auto* controls=new QFormLayout(controls_widget);
        auto* analysis_symbol=new QComboBox;
        for(int index=0;index<stock_symbol_->count();++index)
            analysis_symbol->addItem(stock_symbol_->itemText(index));
        analysis_symbol->setCurrentText(symbol);
        auto* parametric=new QCheckBox("Run a parametric study");
        auto* bar_resolution=new QComboBox;
        bar_resolution->addItem("1 Day",0);
        bar_resolution->addItem("1 Hour",60);
        bar_resolution->addItem("30 Min",30);
        bar_resolution->addItem("15 Min",15);
        bar_resolution->addItem("5 Min",5);
        bar_resolution->addItem("1 Min",1);
        auto* price_field=new QComboBox;
        price_field->addItem("Open","open");
        price_field->addItem("Close","close");
        price_field->addItem("High","high");
        price_field->addItem("Low","low");
        price_field->addItem("Average","average");
        price_field->addItem("VWAP","vwap");
        price_field->setCurrentIndex(price_field->findData("vwap"));
        auto* window_days=new QSpinBox;
        window_days->setRange(1,3650);
        window_days->setValue(30);
        window_days->setSuffix(" day(s)");
        auto* skip_days=new QSpinBox;
        skip_days->setRange(1,3650);
        skip_days->setValue(1);
        skip_days->setSuffix(" day(s)");
        auto* drop_rate=new QDoubleSpinBox;
        drop_rate->setRange(0.0,100.0);
        drop_rate->setDecimals(2);
        drop_rate->setValue(0.0);
        drop_rate->setSuffix("%");
        auto* strike_enabled=new QCheckBox("Compare against an option strike");
        auto* strike_width=new QDoubleSpinBox;
        strike_width->setRange(0.01,10000.0);
        strike_width->setDecimals(2);
        strike_width->setValue(5.0);
        strike_width->setPrefix("$");
        strike_width->setEnabled(false);
        auto* strike_resolution=new QDoubleSpinBox;
        strike_resolution->setRange(0.01,10000.0);
        strike_resolution->setDecimals(2);
        strike_resolution->setValue(1.0);
        strike_resolution->setPrefix("$");
        strike_resolution->setEnabled(false);
        auto* strike_offset=new QSpinBox;
        strike_offset->setRange(-100,100);
        strike_offset->setValue(0);
        strike_offset->setEnabled(false);
        auto* simulated_pricing=new QCheckBox("Use simulated spread pricing");
        simulated_pricing->setEnabled(false);
        auto* allocation=new QDoubleSpinBox;
        allocation->setRange(1.0,1000000000.0);
        allocation->setDecimals(2);
        allocation->setValue(10000.0);
        allocation->setPrefix("$");
        allocation->setEnabled(false);
        auto* slippage_mode=new QComboBox;
        slippage_mode->addItems({"None","Sell only","Buy only","Buy and sell"});
        slippage_mode->setEnabled(false);
        const auto slippage_box=[] {
            auto* box=new QDoubleSpinBox;
            box->setRange(0.0,1000.0);
            box->setDecimals(4);
            box->setSingleStep(0.01);
            box->setValue(0.04);
            box->setPrefix("$");
            box->setSuffix(" per share");
            box->setEnabled(false);
            return box;
        };
        auto* buy_slippage=slippage_box();
        auto* sell_slippage=slippage_box();
        QDate analysis_available_start=available_start_;
        QDate analysis_available_end=available_end_;
        auto* analysis_start=new QDateEdit(qMax(analysis_available_start,analysis_available_end.addYears(-1)));
        auto* analysis_end=new QDateEdit(analysis_available_end);
        analysis_start->setCalendarPopup(true); analysis_end->setCalendarPopup(true);
        analysis_start->setDateRange(analysis_available_start,analysis_available_end);
        analysis_end->setDateRange(analysis_available_start,analysis_available_end);
        auto* window_days_label=new QLabel("Comparison window (x)");
        auto* skip_days_label=new QLabel("Skip window (d)");
        auto* strike_offset_label=new QLabel("Strike offset");
        controls->addRow("Ticker",analysis_symbol);
        controls->addRow("Study mode",parametric);
        controls->addRow("Data resolution",bar_resolution);
        controls->addRow("Analysis price",price_field);
        controls->addRow(window_days_label,window_days);
        controls->addRow(skip_days_label,skip_days);
        controls->addRow("Drop rate",drop_rate);
        controls->addRow("Strike analysis",strike_enabled);
        controls->addRow("Strike width",strike_width);
        controls->addRow("Strike resolution",strike_resolution);
        controls->addRow(strike_offset_label,strike_offset);
        controls->addRow("Pseudo-backtest",simulated_pricing);
        controls->addRow("Trading allocation",allocation);
        controls->addRow("Slippage sides",slippage_mode);
        controls->addRow("Buy slippage",buy_slippage);
        controls->addRow("Sell slippage",sell_slippage);
        controls->addRow("Analysis start",analysis_start);
        controls->addRow("Analysis end",analysis_end);
        layout->addWidget(controls_widget);

        auto* parameter_ranges=new QWidget;
        auto* range_controls=new QFormLayout(parameter_ranges);
        const auto range_row=[](int minimum_value,int maximum_value) {
            auto* row=new QWidget;
            auto* row_layout=new QHBoxLayout(row);
            row_layout->setContentsMargins(0,0,0,0);
            auto* minimum=new QSpinBox;
            auto* maximum=new QSpinBox;
            minimum->setRange(1,3650); maximum->setRange(1,3650);
            minimum->setValue(minimum_value); maximum->setValue(maximum_value);
            minimum->setSuffix(" day(s)"); maximum->setSuffix(" day(s)");
            row_layout->addWidget(new QLabel("From")); row_layout->addWidget(minimum);
            row_layout->addWidget(new QLabel("through")); row_layout->addWidget(maximum);
            row_layout->addStretch();
            return std::tuple{row,minimum,maximum};
        };
        auto [window_range_row,window_minimum,window_maximum]=range_row(1,30);
        auto [skip_range_row,skip_minimum,skip_maximum]=range_row(1,30);
        auto* strike_range_row=new QWidget;
        auto* strike_range_layout=new QHBoxLayout(strike_range_row);
        strike_range_layout->setContentsMargins(0,0,0,0);
        auto* strike_minimum=new QSpinBox;
        auto* strike_maximum=new QSpinBox;
        strike_minimum->setRange(-100,100); strike_maximum->setRange(-100,100);
        strike_minimum->setValue(-1); strike_maximum->setValue(1);
        strike_range_layout->addWidget(new QLabel("From"));
        strike_range_layout->addWidget(strike_minimum);
        strike_range_layout->addWidget(new QLabel("through"));
        strike_range_layout->addWidget(strike_maximum);
        strike_range_layout->addStretch();
        range_controls->addRow("Comparison window (x)",window_range_row);
        range_controls->addRow("Skip window (d)",skip_range_row);
        range_controls->addRow("Strike offset",strike_range_row);
        strike_range_row->setVisible(false);
        parameter_ranges->setVisible(false);
        layout->addWidget(parameter_ranges);

        auto* pricing_editor=new PricingEditor;
        auto* pricing_scroll=new QScrollArea;
        pricing_scroll->setWidget(pricing_editor);
        pricing_scroll->setWidgetResizable(true);
        pricing_scroll->setFrameShape(QFrame::StyledPanel);
        pricing_scroll->setMinimumHeight(180);
        pricing_scroll->setMaximumHeight(320);
        pricing_scroll->setVisible(false);
        layout->addWidget(pricing_scroll);

        auto* single_results=new QWidget;
        auto* results=new QFormLayout(single_results);
        auto* win_percentage=new QLabel("—");
        auto* wins=new QLabel("—");
        auto* losses=new QLabel("—");
        auto* ties=new QLabel("—");
        auto* comparisons=new QLabel("—");
        auto* dropped=new QLabel("—");
        results->addRow("Win Rate %",win_percentage);
        results->addRow("Average wins per start",wins);
        results->addRow("Average losses per start",losses);
        results->addRow("Average ties per start",ties);
        results->addRow("Average comparisons per start",comparisons);
        results->addRow("Average dropped per start",dropped);
        layout->addWidget(single_results);

        std::vector<MomentumStudyRow> study_rows;
        auto* study_table=new QTableView;
        auto* study_model=new MomentumStudyTableModel(study_table);
        study_table->setModel(study_model);
        study_table->horizontalHeader()->setMinimumSectionSize(80);
        study_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
        study_table->horizontalHeader()->setDefaultSectionSize(110);
        study_table->horizontalHeader()->setStretchLastSection(false);
        study_table->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
        study_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        study_table->setSelectionBehavior(QAbstractItemView::SelectRows);
        study_table->horizontalHeader()->setSortIndicatorShown(false);
        study_table->setColumnHidden(8,true);
        study_table->setColumnHidden(9,true);
        study_table->setColumnHidden(10,true);
        study_table->setColumnHidden(11,true);
        study_table->setVisible(false);
        layout->addWidget(study_table,1);

        auto* analysis_status=new QLabel;
        analysis_status->setWordWrap(true);
        layout->addWidget(analysis_status);
        auto* analysis_activity=new QWidget;
        auto* activity_layout=new QHBoxLayout(analysis_activity);
        activity_layout->setContentsMargins(0,0,0,0);
        auto* activity_label=new QLabel("Analyzing…");
        auto* activity_bar=new QProgressBar;
        activity_bar->setRange(0,0);
        activity_bar->setTextVisible(false);
        activity_bar->setMaximumWidth(260);
        activity_layout->addStretch();
        activity_layout->addWidget(activity_label);
        activity_layout->addWidget(activity_bar);
        activity_layout->addStretch();
        analysis_activity->setVisible(false);
        layout->addWidget(analysis_activity);
        auto* dialog_buttons=new QHBoxLayout;
        auto* save_button=new QPushButton("Save Study…");
        auto* analyze_button=new QPushButton("Run Analysis");
        dialog_buttons->addStretch();
        dialog_buttons->addWidget(save_button);
        dialog_buttons->addWidget(analyze_button);
        layout->addLayout(dialog_buttons);

        int active_sort_column=-1;
        int sort_cycle=0;
        std::vector<options::data::Bar> analyzed_bars;
        QString analyzed_symbol=symbol;
        std::shared_ptr<AnalysisProgress> active_analysis_progress;
        auto* progress_timer=new QTimer(&dialog);
        progress_timer->setInterval(100);
        connect(progress_timer,&QTimer::timeout,&dialog,[&] {
            if(!active_analysis_progress) return;
            const auto phase=active_analysis_progress->phase.load(std::memory_order_relaxed);
            if(phase==0) {
                activity_label->setText("Opening saved results…");
                activity_bar->setRange(0,0);
                return;
            }
            const auto completed=active_analysis_progress->completed.load(std::memory_order_relaxed);
            const auto total=active_analysis_progress->total.load(std::memory_order_relaxed);
            const auto remaining=total>completed ? total-completed : 0;
            activity_bar->setRange(0,static_cast<int>(total));
            activity_bar->setValue(static_cast<int>(completed));
            if(phase==2) {
                activity_label->setText(remaining==0
                    ? "Finalizing saved results…"
                    : "Saving results: "+QString::number(completed)+" / "+QString::number(total)+
                        " rows — "+QString::number(remaining)+" remaining");
            } else if(phase==3) {
                activity_label->setText(remaining==0
                    ? "Loaded "+QString::number(total)+" / "+QString::number(total)+" result rows"
                    : "Loading results: "+QString::number(completed)+" / "+QString::number(total)+
                        " rows — "+QString::number(remaining)+" remaining");
            } else {
                activity_label->setText(remaining==0
                    ? QString::number(total)+" / "+QString::number(total)+
                        " calculations complete"
                    : QString::number(completed)+" / "+QString::number(total)+
                        " complete — "+QString::number(remaining)+" remaining");
            }
        });
        connect(study_table->horizontalHeader(),&QHeaderView::sectionClicked,&dialog,
            [=,&active_sort_column,&sort_cycle](int column) {
                if(study_model->rowCount()==0) return;
                if(column!=active_sort_column) {
                    active_sort_column=column;
                    sort_cycle=1;
                } else {
                    sort_cycle=(sort_cycle+1)%3;
                }
                if(sort_cycle==0) {
                    study_model->sort_by(11,std::nullopt);
                    study_table->horizontalHeader()->setSortIndicatorShown(false);
                    active_sort_column=-1;
                    return;
                }
                const auto order=sort_cycle==1 ? Qt::DescendingOrder : Qt::AscendingOrder;
                study_table->horizontalHeader()->setSortIndicator(column,order);
                study_table->horizontalHeader()->setSortIndicatorShown(true);
                study_model->sort_by(column,order);
            });

        auto* ledger_watcher=new QFutureWatcher<MomentumLedgerOutput>(&dialog);
        connect(ledger_watcher,&QFutureWatcher<MomentumLedgerOutput>::finished,&dialog,[&] {
            study_table->setEnabled(true);
            analyze_button->setEnabled(true);
            save_button->setEnabled(true);
            analysis_activity->setVisible(false);
            activity_label->setText("Analyzing…");
            activity_bar->setRange(0,0);
            auto output=ledger_watcher->result();
            if(!output.error.isEmpty()) {
                analysis_status->setText("Ledger generation failed: "+output.error);
                return;
            }
            if(output.row_index>=study_rows.size()) return;
            study_rows[output.row_index].result=output.result;
            analysis_status->setText(
                "Generated "+QString::number(output.result.trades.size())+
                " executed-trade ledger rows for the selected median scenario.");
            show_profit_chart(&dialog,output.title,output.result,output.bars);
        });
        connect(study_table,&QTableView::doubleClicked,&dialog,
            [&](const QModelIndex& model_index) {
                if(ledger_watcher->isRunning()) return;
                const auto index=study_model->source_index(model_index.row());
                if(index>=study_rows.size()) return;
                const auto& value=study_rows[static_cast<std::size_t>(index)];
                if(!value.simulated_pricing) return;
                const auto resolution=value.strike_adjustment
                    ? value.strike_adjustment->resolution : 0.0;
                const auto width=value.strike_adjustment
                    ? value.strike_adjustment->width : 0.0;
                const auto title=analyzed_symbol+"  DTE="+QString::number(value.window_days)+
                    " skip="+QString::number(value.skip_days)+
                    " data="+bar_resolution->currentText()+
                    " price="+price_field->currentText()+
                    " res="+QString::number(resolution,'f',2)+
                    " strike_width="+QString::number(width,'f',2)+
                    " strike_offset="+QString::number(value.strike_offset);
                if(!value.result.trades.empty()) {
                    show_profit_chart(&dialog,title,value.result,analyzed_bars);
                    return;
                }
                study_table->setEnabled(false);
                analyze_button->setEnabled(false);
                save_button->setEnabled(false);
                activity_label->setText("Generating trade ledger…");
                activity_bar->setRange(0,0);
                analysis_activity->setVisible(true);
                analysis_status->setText("Generating the selected row's median-scenario trade ledger…");
                ledger_watcher->setFuture(QtConcurrent::run(
                    [bars=analyzed_bars,value,title,index=static_cast<std::size_t>(index)]() mutable {
                        MomentumLedgerOutput output;
                        output.row_index=index;
                        output.title=title;
                        output.bars=bars;
                        try {
                            output.result=options::analysis::analyze_momentum_drop_scenarios(
                                bars,static_cast<std::size_t>(value.window_days),
                                static_cast<std::size_t>(value.skip_days),value.strike_adjustment,
                                value.simulated_pricing,value.drop_rate,5,true);
                        } catch(const std::exception& error) {
                            output.error=QString::fromUtf8(error.what());
                        }
                        return output;
                    }));
            });

        const auto expand_dialog_for=[&dialog](int minimum_width,int minimum_height) {
            QTimer::singleShot(0,&dialog,[&dialog,minimum_width,minimum_height] {
                const auto available=dialog.screen()->availableGeometry().size();
                const auto hint=dialog.sizeHint();
                const auto target_width=std::min(
                    std::max({dialog.width(),minimum_width,hint.width()+40}),available.width()-40);
                const auto target_height=std::min(
                    std::max({dialog.height(),minimum_height,hint.height()+40}),available.height()-40);
                dialog.resize(target_width,target_height);
            });
        };
        const auto rebuild_pricing=[=,&expand_dialog_for] {
            if(!simulated_pricing->isChecked() || !strike_enabled->isChecked()) {
                pricing_scroll->setVisible(false);
                return;
            }
            std::vector<int> offsets;
            if(parametric->isChecked()) {
                if(strike_minimum->value()<=strike_maximum->value())
                    for(int value=strike_minimum->value();value<=strike_maximum->value();++value)
                        offsets.push_back(value);
            } else {
                offsets.push_back(strike_offset->value());
            }
            pricing_editor->set_offsets(std::move(offsets));
            pricing_scroll->setVisible(true);
            expand_dialog_for(1250,820);
        };
        const auto selected_slippage_mode=[=] {
            switch(slippage_mode->currentIndex()) {
            case 1: return options::analysis::SlippageMode::sell;
            case 2: return options::analysis::SlippageMode::buy;
            case 3: return options::analysis::SlippageMode::buy_and_sell;
            default: return options::analysis::SlippageMode::none;
            }
        };

        bool study_resolution_overridden=false;
        const auto select_default_study_resolution=[=] {
            const auto minutes=analysis_start->date()<analysis_end->date() ? 0 : 1;
            const auto index=bar_resolution->findData(minutes);
            if(index>=0) bar_resolution->setCurrentIndex(index);
        };
        select_default_study_resolution();
        connect(bar_resolution,&QComboBox::activated,&dialog,
            [&study_resolution_overridden](int) { study_resolution_overridden=true; });
        const auto study_dates_changed=[&study_resolution_overridden,
            select_default_study_resolution](QDate) {
            if(!study_resolution_overridden) select_default_study_resolution();
        };
        connect(analysis_start,&QDateEdit::dateChanged,&dialog,study_dates_changed);
        connect(analysis_end,&QDateEdit::dateChanged,&dialog,study_dates_changed);

        connect(parametric,&QCheckBox::toggled,&dialog,[=](bool enabled) {
            window_days_label->setVisible(!enabled);
            window_days->setVisible(!enabled);
            skip_days_label->setVisible(!enabled);
            skip_days->setVisible(!enabled);
            strike_offset_label->setVisible(!enabled);
            strike_offset->setVisible(!enabled);
            strike_offset->setEnabled(strike_enabled->isChecked());
            strike_range_row->setVisible(strike_enabled->isChecked() && enabled);
            parameter_ranges->setVisible(enabled);
            single_results->setVisible(!enabled);
            study_table->setVisible(enabled);
            analyze_button->setText(enabled ? "Run Parametric Study" : "Run Analysis");
            analysis_status->clear();
            rebuild_pricing();
            if(enabled) expand_dialog_for(1400,820);
        });
        connect(strike_enabled,&QCheckBox::toggled,&dialog,[=](bool enabled) {
            strike_width->setEnabled(enabled);
            strike_resolution->setEnabled(enabled);
            strike_offset->setEnabled(enabled && !parametric->isChecked());
            strike_range_row->setVisible(enabled && parametric->isChecked());
            simulated_pricing->setEnabled(enabled);
            if(!enabled) simulated_pricing->setChecked(false);
            analysis_status->clear();
            rebuild_pricing();
        });
        connect(simulated_pricing,&QCheckBox::toggled,&dialog,[=](bool enabled) {
            allocation->setEnabled(enabled);
            slippage_mode->setEnabled(enabled);
            buy_slippage->setEnabled(enabled &&
                (slippage_mode->currentIndex()==2 || slippage_mode->currentIndex()==3));
            sell_slippage->setEnabled(enabled &&
                (slippage_mode->currentIndex()==1 || slippage_mode->currentIndex()==3));
            study_table->setColumnHidden(8,!enabled);
            study_table->setColumnHidden(9,!enabled);
            study_table->setColumnHidden(10,!enabled);
            study_table->setColumnHidden(11,true);
            rebuild_pricing();
        });
        connect(slippage_mode,&QComboBox::currentIndexChanged,&dialog,[=](int index) {
            buy_slippage->setEnabled(simulated_pricing->isChecked() && (index==2 || index==3));
            sell_slippage->setEnabled(simulated_pricing->isChecked() && (index==1 || index==3));
        });
        connect(strike_offset,&QSpinBox::valueChanged,&dialog,[=](int) { rebuild_pricing(); });
        connect(strike_minimum,&QSpinBox::valueChanged,&dialog,[=](int) { rebuild_pricing(); });
        connect(strike_maximum,&QSpinBox::valueChanged,&dialog,[=](int) { rebuild_pricing(); });
        connect(analysis_symbol,&QComboBox::currentTextChanged,&dialog,
            [&,analysis_start,analysis_end](const QString& selected_symbol) {
                const auto range=bar_store_->date_range({selected_symbol.toStdString(),"alpaca",
                    "iex",options::data::stock_bar_timeframe,"all","",""});
                const auto first=QDate::fromString(QString::fromStdString(range.start),Qt::ISODate);
                const auto last=QDate::fromString(QString::fromStdString(range.end),Qt::ISODate);
                if(!first.isValid() || !last.isValid()) return;
                const auto span=qMax(0,analysis_start->date().daysTo(analysis_end->date()));
                auto selected_end=qBound(first,analysis_end->date(),last);
                auto selected_start=selected_end.addDays(-span);
                if(selected_start<first) {
                    selected_start=first;
                    selected_end=qMin(last,first.addDays(span));
                }
                analysis_available_start=first;
                analysis_available_end=last;
                const QSignalBlocker start_blocker(analysis_start);
                const QSignalBlocker end_blocker(analysis_end);
                analysis_start->setDateRange(first,last);
                analysis_end->setDateRange(first,last);
                analysis_start->setDate(selected_start);
                analysis_end->setDate(selected_end);
                analysis_status->setText(
                    "Ticker changed to "+selected_symbol+". Review the parameters, then select Run.");
            });

        QString loaded_study_id=preset ? preset->id : QString();
        QString loaded_study_name=preset ? preset->name : QString();
        QString loaded_study_symbol=preset ? preset->symbol : QString();
        QString last_saved_result_key;
        QString last_result_settings_signature;
        std::optional<MomentumAnalysisOutput> last_analysis_result;
        if(preset) {
            study_resolution_overridden=true;
            const auto bar_index=bar_resolution->findData(preset->bar_minutes);
            if(bar_index>=0) bar_resolution->setCurrentIndex(bar_index);
            const auto price_index=price_field->findData(preset->price_field);
            if(price_index>=0) price_field->setCurrentIndex(price_index);
            window_days->setValue(preset->window_days);
            skip_days->setValue(preset->skip_days);
            drop_rate->setValue(preset->drop_rate);
            strike_width->setValue(preset->strike_width);
            strike_resolution->setValue(preset->strike_resolution);
            strike_offset->setValue(preset->strike_offset);
            allocation->setValue(preset->allocation);
            slippage_mode->setCurrentIndex(qBound(0,preset->slippage_mode,3));
            buy_slippage->setValue(preset->buy_slippage);
            sell_slippage->setValue(preset->sell_slippage);
            window_minimum->setValue(preset->window_minimum);
            window_maximum->setValue(preset->window_maximum);
            skip_minimum->setValue(preset->skip_minimum);
            skip_maximum->setValue(preset->skip_maximum);
            strike_minimum->setValue(preset->strike_minimum);
            strike_maximum->setValue(preset->strike_maximum);
            if(preset->analysis_start.isValid())
                analysis_start->setDate(qBound(analysis_available_start,preset->analysis_start,analysis_available_end));
            if(preset->analysis_end.isValid())
                analysis_end->setDate(qBound(analysis_available_start,preset->analysis_end,analysis_available_end));
            pricing_editor->set_pricing(preset->pricing);
            parametric->setChecked(preset->parametric);
            strike_enabled->setChecked(preset->strike_enabled);
            simulated_pricing->setChecked(preset->strike_enabled && preset->simulated_pricing);
            dialog.setWindowTitle("Momentum Analysis — "+symbol+" — "+preset->name);
            analysis_status->setText("Loaded study \""+preset->name+"\". Adjust any parameters, then select Run.");
        }
        connect(analysis_symbol,&QComboBox::currentTextChanged,&dialog,
            [&,analysis_symbol](const QString& selected_symbol) {
                dialog.setWindowTitle("Momentum Analysis — "+selected_symbol+
                    (loaded_study_name.isEmpty() ? QString() : " — "+loaded_study_name));
            });

        const auto capture_preset=[&](const QString& name,const QString& id) {
            MomentumStudyPreset value;
            value.id=id;
            value.name=name;
            value.symbol=analysis_symbol->currentText();
            value.parametric=parametric->isChecked();
            value.window_days=window_days->value();
            value.skip_days=skip_days->value();
            value.bar_minutes=bar_resolution->currentData().toInt();
            value.price_field=price_field->currentData().toString();
            value.drop_rate=drop_rate->value();
            value.strike_enabled=strike_enabled->isChecked();
            value.strike_width=strike_width->value();
            value.strike_resolution=strike_resolution->value();
            value.strike_offset=strike_offset->value();
            value.simulated_pricing=simulated_pricing->isChecked();
            value.allocation=allocation->value();
            value.slippage_mode=slippage_mode->currentIndex();
            value.buy_slippage=buy_slippage->value();
            value.sell_slippage=sell_slippage->value();
            value.analysis_start=analysis_start->date();
            value.analysis_end=analysis_end->date();
            value.window_minimum=window_minimum->value();
            value.window_maximum=window_maximum->value();
            value.skip_minimum=skip_minimum->value();
            value.skip_maximum=skip_maximum->value();
            value.strike_minimum=strike_minimum->value();
            value.strike_maximum=strike_maximum->value();
            value.pricing=pricing_editor->all_pricing();
            return value;
        };

        connect(save_button,&QPushButton::clicked,&dialog,[&] {
            bool accepted=false;
            const auto proposed=QInputDialog::getText(&dialog,"Save Momentum Study","Study name:",
                QLineEdit::Normal,loaded_study_name,&accepted).trimmed();
            if(!accepted || proposed.isEmpty()) return;

            const auto selected_symbol=analysis_symbol->currentText();
            auto id=!loaded_study_id.isEmpty() &&
                    proposed.compare(loaded_study_name,Qt::CaseInsensitive)==0 &&
                    selected_symbol.compare(loaded_study_symbol,Qt::CaseInsensitive)==0
                ? loaded_study_id : QString();
            const auto studies=load_study_presets();
            const auto same_study=std::ranges::find_if(studies,[&](const auto& value) {
                return value.name.compare(proposed,Qt::CaseInsensitive)==0 &&
                    value.symbol.compare(selected_symbol,Qt::CaseInsensitive)==0;
            });
            if(same_study!=studies.end() && same_study->id!=id) {
                if(QMessageBox::question(&dialog,"Replace Saved Study",
                    "A study named \""+same_study->name+"\" already exists for "+
                        selected_symbol+". Replace it?")!=QMessageBox::Yes)
                    return;
                id=same_study->id;
            }
            if(id.isEmpty()) id=QUuid::createUuid().toString(QUuid::WithoutBraces);

            auto value=capture_preset(proposed,id);
            const auto has_matching_result=last_analysis_result &&
                momentum_settings_signature(value)==last_result_settings_signature &&
                !last_saved_result_key.isEmpty();
            value.saved_result_key.clear();
            if(has_matching_result && saved_result_exists(last_saved_result_key))
                value.saved_result_key=last_saved_result_key;
            save_study_preset(value);
            loaded_study_id=id;
            loaded_study_name=proposed;
            loaded_study_symbol=selected_symbol;
            dialog.setWindowTitle("Momentum Analysis — "+value.symbol+" — "+proposed);
            refresh_saved_studies(id);
            if(!has_matching_result) {
                analysis_status->setText(
                    "Saved study \""+proposed+"\" without results because its current settings have not been run.");
                return;
            }
            if(!value.saved_result_key.isEmpty() &&
               saved_result_uses_parallel_format(value.saved_result_key)) {
                analysis_status->setText("Saved study \""+proposed+"\" with its existing results.");
                return;
            }

            auto saved_output=std::make_shared<MomentumAnalysisOutput>(*last_analysis_result);
            if(saved_output->parametric) saved_output->study_rows=study_rows;
            active_analysis_progress=std::make_shared<AnalysisProgress>();
            active_analysis_progress->total=saved_output->parametric
                ? saved_output->study_rows.size() : 1;
            active_analysis_progress->phase.store(2,std::memory_order_relaxed);
            const auto save_progress=active_analysis_progress;
            const auto result_key=last_saved_result_key;
            const auto saved_bars=analyzed_bars;
            controls_widget->setEnabled(false);
            parameter_ranges->setEnabled(false);
            pricing_editor->setEnabled(false);
            study_table->setEnabled(false);
            save_button->setEnabled(false);
            analyze_button->setEnabled(false);
            analysis_activity->setVisible(true);
            analysis_status->setText("Saving study \""+proposed+"\" and its analysis results…");
            progress_timer->start();

            auto* save_watcher=new QFutureWatcher<bool>(&dialog);
            connect(save_watcher,&QFutureWatcher<bool>::finished,&dialog,
                [&,save_watcher,saved_output,value,result_key]() mutable {
                    const auto saved=save_watcher->result();
                    progress_timer->stop();
                    active_analysis_progress.reset();
                    controls_widget->setEnabled(true);
                    parameter_ranges->setEnabled(true);
                    pricing_editor->setEnabled(true);
                    study_table->setEnabled(true);
                    save_button->setEnabled(true);
                    analyze_button->setEnabled(true);
                    analysis_activity->setVisible(false);
                    activity_label->setText("Analyzing…");
                    activity_bar->setRange(0,0);
                    if(saved) {
                        auto saved_value=value;
                        saved_value.saved_result_key=result_key;
                        save_study_preset(saved_value);
                        refresh_saved_studies(saved_value.id);
                        analysis_status->setText(
                            "Saved study \""+saved_value.name+"\" with its analysis results.");
                    } else {
                        analysis_status->setText(
                            "Saved the study parameters, but its analysis results could not be written.");
                    }
                    save_watcher->deleteLater();
                });
            save_watcher->setFuture(QtConcurrent::run(
                [result_key,saved_output,saved_bars,save_progress] {
                    return save_study_results(result_key,*saved_output,saved_bars,save_progress);
                }));
        });

        auto* analysis_watcher=new QFutureWatcher<MomentumAnalysisOutput>(&dialog);
        const auto set_busy=[=,&active_analysis_progress](bool busy) {
            controls_widget->setEnabled(!busy);
            parameter_ranges->setEnabled(!busy);
            pricing_editor->setEnabled(!busy);
            study_table->setEnabled(!busy);
            save_button->setEnabled(!busy);
            analyze_button->setEnabled(!busy);
            analysis_activity->setVisible(busy);
            if(busy) {
                progress_timer->start();
            } else {
                progress_timer->stop();
                active_analysis_progress.reset();
                activity_label->setText("Analyzing…");
                activity_bar->setRange(0,0);
            }
        };
        connect(analysis_watcher,&QFutureWatcher<MomentumAnalysisOutput>::finished,&dialog,
            [&,set_busy,analysis_watcher] {
                auto output=analysis_watcher->future().takeResult();
                if(!output.error.isEmpty()) {
                    set_busy(false);
                    analysis_status->setText("Analysis failed: "+output.error);
                    return;
                }
                if(output.saved_result_missing) {
                    set_busy(false);
                    analysis_status->setText(
                        "No saved result is available for this study. Select Run to analyze it.");
                    return;
                }
                last_saved_result_key=output.saved_result_key;
                last_result_settings_signature=output.settings_signature;
                if(output.parametric) {
                    progress_timer->stop();
                    activity_label->setText("Preparing result table…");
                    activity_bar->setRange(0,0);
                    study_rows=std::move(output.study_rows);
                    active_sort_column=-1;
                    sort_cycle=0;
                    study_table->horizontalHeader()->setSortIndicatorShown(false);
                    study_table->setColumnHidden(8,!output.simulated_pricing);
                    study_table->setColumnHidden(9,!output.simulated_pricing);
                    study_table->setColumnHidden(10,!output.simulated_pricing);
                    study_table->setColumnHidden(11,true);
                    study_model->set_rows(&study_rows,output.strike_enabled);
                    analysis_status->setText(
                        (output.loaded_saved_result ? "Loaded saved results for " : "Generated ")+
                        QString::number(study_rows.size())+
                        " parameter combinations. Click a header to sort"+
                        (output.simulated_pricing
                            ? "; double-click a row to view its cumulative profit chart." : "."));
                    last_analysis_result=std::move(output);
                    set_busy(false);
                    return;
                }
                if(!output.single_result) {
                    set_busy(false);
                    analysis_status->setText("Analysis failed to return a result.");
                    return;
                }
                const auto& result=*output.single_result;
                win_percentage->setText(QString::number(result.win_percentage,'f',2)+"%");
                wins->setText(averaged_count(result.wins));
                losses->setText(averaged_count(result.losses));
                ties->setText(averaged_count(result.ties));
                comparisons->setText(averaged_count(result.comparisons));
                dropped->setText(averaged_count(result.dropped_comparisons));
                analysis_status->setText(result.comparisons==0
                    ? "The selected analysis range is too short for this comparison window."
                    : output.loaded_saved_result ? "Loaded saved analysis result." : QString());
                const auto open_chart=
                    output.open_profit_chart && output.simulated_pricing && result.comparisons!=0;
                set_busy(false);
                if(open_chart)
                    show_profit_chart(&dialog,output.symbol+"  DTE="+
                        QString::number(output.window_days)+" skip="+
                        QString::number(output.skip_days)+" data="+
                        bar_resolution->currentText()+" price="+price_field->currentText()+" res="+
                        QString::number(output.strike_resolution,'f',2)+" strike_width="+
                        QString::number(output.strike_width,'f',2)+" strike_offset="+
                        QString::number(output.strike_offset),result,analyzed_bars);
                last_analysis_result=std::move(output);
            });

        const auto analyze=[&,set_busy,analysis_watcher](bool load_saved) {
            if(analysis_watcher->isRunning()) return;
            if(analysis_start->date()>analysis_end->date()) {
                analysis_status->setText("The analysis start must not be after the end.");
                return;
            }
            try {
                const auto run_symbol=analysis_symbol->currentText();
                analyzed_symbol=run_symbol;
                const auto source_bars=bar_store_->load({run_symbol.toStdString(),"alpaca","iex",
                    options::data::stock_bar_timeframe,"all",
                    analysis_start->date().toString(Qt::ISODate).toStdString(),
                    analysis_end->date().toString(Qt::ISODate).toStdString()});
                analyzed_bars=aggregate_regular_bars(
                    source_bars,bar_resolution->currentData().toInt());
                select_analysis_price(analyzed_bars,price_field->currentData().toString());
                if(analyzed_bars.empty()) {
                    analysis_status->setText(
                        "No regular-session bars are available for the selected range.");
                    return;
                }
                const auto current_preset=capture_preset({},{});
                const auto settings_signature=momentum_settings_signature(current_preset);
                const auto result_key=load_saved && preset
                    ? preset->saved_result_key
                    : QUuid::createUuid().toString(QUuid::WithoutBraces);
                MomentumAnalysisOutput output;
                output.symbol=run_symbol;
                output.saved_result_key=result_key;
                output.settings_signature=settings_signature;
                output.open_profit_chart=!load_saved;
                output.parametric=parametric->isChecked();
                output.strike_enabled=strike_enabled->isChecked();
                output.simulated_pricing=simulated_pricing->isChecked();
                output.window_days=window_days->value();
                output.skip_days=skip_days->value();
                output.strike_offset=strike_offset->value();
                output.strike_width=strike_width->value();
                output.strike_resolution=strike_resolution->value();
                std::vector<MomentumStudyRequest> requests;
                if(output.parametric) {
                    if(window_minimum->value()>window_maximum->value() ||
                       skip_minimum->value()>skip_maximum->value() ||
                       (output.strike_enabled && strike_minimum->value()>strike_maximum->value())) {
                        analysis_status->setText("Each parameter minimum must not exceed its maximum.");
                        return;
                    }
                    const auto window_count=window_maximum->value()-window_minimum->value()+1;
                    const auto skip_count=skip_maximum->value()-skip_minimum->value()+1;
                    const auto strike_count=output.strike_enabled
                        ? strike_maximum->value()-strike_minimum->value()+1 : 1;
                    const auto combinations=static_cast<qint64>(window_count)*skip_count*strike_count;
                    if(combinations>10000) {
                        analysis_status->setText(
                            "This study has "+QString::number(combinations)+
                            " combinations. Narrow the ranges to 10,000 or fewer.");
                        return;
                    }
                    requests.reserve(static_cast<std::size_t>(combinations));
                    for(int x=window_minimum->value();x<=window_maximum->value();++x) {
                        for(int d=skip_minimum->value();d<=skip_maximum->value();++d) {
                            const auto first_strike=output.strike_enabled ? strike_minimum->value() : 0;
                            const auto last_strike=output.strike_enabled ? strike_maximum->value() : 0;
                            for(int offset=first_strike;offset<=last_strike;++offset) {
                                MomentumStudyRequest request{x,d,offset};
                                if(output.strike_enabled)
                                    request.strike_adjustment=options::analysis::StrikeAdjustment{
                                        strike_width->value(),strike_resolution->value(),offset};
                                if(output.simulated_pricing)
                                    request.simulated_pricing=pricing_editor->pricing_for(offset);
                                if(output.simulated_pricing && !request.simulated_pricing) {
                                    analysis_status->setText(
                                        "Simulated pricing is missing for strike offset "+QString::number(offset)+".");
                                    return;
                                }
                                requests.push_back(std::move(request));
                            }
                        }
                    }
                } else {
                    MomentumStudyRequest request{
                        output.window_days,output.skip_days,output.strike_offset};
                    if(output.strike_enabled)
                        request.strike_adjustment=options::analysis::StrikeAdjustment{
                            strike_width->value(),strike_resolution->value(),output.strike_offset};
                    if(output.simulated_pricing)
                        request.simulated_pricing=pricing_editor->pricing_for(output.strike_offset);
                    if(output.simulated_pricing && !request.simulated_pricing) {
                        analysis_status->setText("Simulated pricing is missing for this strike offset.");
                        return;
                    }
                    requests.push_back(std::move(request));
                }
                if(output.simulated_pricing) {
                    for(auto& request:requests) {
                        request.simulated_pricing->allocation=allocation->value();
                        request.simulated_pricing->buy_slippage_per_share=buy_slippage->value();
                        request.simulated_pricing->sell_slippage_per_share=sell_slippage->value();
                        request.simulated_pricing->slippage_mode=selected_slippage_mode();
                    }
                }
                const auto request_count=requests.size();
                const auto rate=drop_rate->value();
                active_analysis_progress=std::make_shared<AnalysisProgress>();
                active_analysis_progress->total=request_count;
                active_analysis_progress->phase.store(load_saved ? 0 : 1,std::memory_order_relaxed);
                const auto progress=active_analysis_progress;
                const auto available_cores=std::max(1,QThread::idealThreadCount());
                const auto reserved_cores=available_cores>=4
                    ? std::max(2,available_cores/4) : available_cores>=2 ? 1 : 0;
                const auto analysis_workers=std::max(1,available_cores-reserved_cores);
                analysis_status->setText(load_saved
                    ? "Loading saved analysis results with up to "+
                        QString::number(analysis_workers)+" workers…" :
                    output.parametric
                        ? "Analyzing "+QString::number(request_count)+
                            " parameter combinations with up to "+QString::number(analysis_workers)+
                            " workers ("+QString::number(reserved_cores)+
                            " CPU cores reserved for responsiveness)…"
                        : "Running the analysis…");
                set_busy(true);
                std::vector<MomentumStudyRow> previous_rows;
                if(!load_saved) {
                    study_model->set_rows(nullptr,false);
                    previous_rows=std::move(study_rows);
                }
                analysis_watcher->setFuture(QtConcurrent::run(
                    [bars=analyzed_bars,requests=std::move(requests),rate,load_saved,
                     output=std::move(output),progress,analysis_workers,
                     previous_rows=std::move(previous_rows)]() mutable {
                        std::vector<MomentumStudyRow>().swap(previous_rows);
                        if(load_saved) {
                            if(auto saved=load_study_results(
                                output.saved_result_key,progress,analysis_workers)) {
                                saved->output.saved_result_key=output.saved_result_key;
                                saved->output.settings_signature=output.settings_signature;
                                saved->output.loaded_saved_result=true;
                                saved->output.open_profit_chart=output.open_profit_chart;
                                return std::move(saved->output);
                            }
                            output.saved_result_missing=true;
                            return output;
                        }
                        try {
                            if(output.parametric) {
                                std::vector<MomentumStudyRow> rows(requests.size());
                                std::atomic<std::size_t> next_index{};
                                std::atomic<bool> failed{};
                                std::mutex error_mutex;
                                std::string worker_error;
                                const auto worker_count=std::min(
                                    static_cast<std::size_t>(analysis_workers),requests.size());
                                {
                                    std::vector<std::jthread> workers;
                                    workers.reserve(worker_count);
                                    for(std::size_t worker=0;worker<worker_count;++worker) {
                                        workers.emplace_back([&] {
                                            while(!failed.load(std::memory_order_relaxed)) {
                                                const auto index=next_index.fetch_add(
                                                    1,std::memory_order_relaxed);
                                                if(index>=requests.size()) break;
                                                const auto& request=requests[index];
                                                try {
                                                    rows[index]={request.window_days,request.skip_days,
                                                        request.strike_offset,
                                                        options::analysis::analyze_momentum_drop_scenarios(
                                                            bars,static_cast<std::size_t>(request.window_days),
                                                            static_cast<std::size_t>(request.skip_days),
                                                            request.strike_adjustment,request.simulated_pricing,
                                                            rate,5,false,false),
                                                        request.strike_adjustment,request.simulated_pricing,rate};
                                                    progress->completed.fetch_add(1,std::memory_order_relaxed);
                                                } catch(const std::exception& error) {
                                                    {
                                                        std::lock_guard lock(error_mutex);
                                                        if(worker_error.empty()) worker_error=error.what();
                                                    }
                                                    failed.store(true,std::memory_order_relaxed);
                                                } catch(...) {
                                                    {
                                                        std::lock_guard lock(error_mutex);
                                                        if(worker_error.empty()) worker_error="unknown worker error";
                                                    }
                                                    failed.store(true,std::memory_order_relaxed);
                                                }
                                            }
                                        });
                                    }
                                }
                                if(!worker_error.empty()) throw std::runtime_error(worker_error);
                                output.study_rows=std::move(rows);
                            } else {
                                const auto& request=requests.front();
                                output.single_result=options::analysis::analyze_momentum_drop_scenarios(
                                    bars,static_cast<std::size_t>(request.window_days),
                                    static_cast<std::size_t>(request.skip_days),request.strike_adjustment,
                                    request.simulated_pricing,rate);
                                progress->completed.store(1,std::memory_order_relaxed);
                            }
                        } catch(const std::exception& error) {
                            output.error=QString::fromUtf8(error.what());
                        }
                        return output;
                    }));
            } catch(const std::exception& error) {
                set_busy(false);
                analysis_status->setText("Analysis failed: "+QString::fromUtf8(error.what()));
            }
        };
        connect(analyze_button,&QPushButton::clicked,&dialog,[&analyze] { analyze(false); });
        if(preset) analyze(true);
        else analyze(false);
        dialog.exec();
    }

    void load_symbol_data() {
        const auto symbol=load_symbol_->text().trimmed().toUpper();
        const auto start=load_start_->date();
        const auto end=load_end_->date();
        if(symbol.isEmpty()) {
            load_status_->setText("Enter a symbol.");
            return;
        }
        if(start>end) {
            load_status_->setText("The start date must not be after the end date.");
            return;
        }

        load_symbol_->setText(symbol);
        load_button_->setEnabled(false);
        load_symbol_->setEnabled(false);
        load_start_->setEnabled(false);
        load_end_->setEnabled(false);
        load_status_->setText("Loading "+symbol+" from Alpaca…");
        const auto database_path=database_path_;
        const auto start_text=start.toString(Qt::ISODate);
        const auto end_text=end.toString(Qt::ISODate);
        load_watcher_->setFuture(QtConcurrent::run([symbol,database_path,start_text,end_text] {
            LoadResult result{symbol,database_path};
            try {
                options::data::SqliteBarStore store(database_path.toStdString());
                const options::data::DateRange requested{start_text.toStdString(),end_text.toStdString()};
                const auto missing=store.missing_coverage(symbol.toStdString(),"alpaca","iex",
                    options::data::stock_bar_timeframe,"all",requested);
                result.cached=missing.empty();
                if(missing.empty()) return result;

                options::providers::alpaca::AlpacaClient client(
                    {environment("ALPACA_API_KEY"),environment("ALPACA_API_SECRET")});
                for(const auto& range:missing) {
                    options::providers::alpaca::BarsRequest request;
                    request.timeframe=options::data::stock_bar_timeframe;
                    request.symbols={symbol.toStdString()};
                    request.start=range.start;
                    request.end=range.end;
                    request.feed="iex";
                    const auto bars=client.fetch_bars(request);
                    store.upsert(bars,"alpaca",request.feed,request.timeframe,request.adjustment);
                    store.record_coverage(symbol.toStdString(),"alpaca",request.feed,
                        request.timeframe,request.adjustment,range);
                    result.downloaded+=bars.size();
                }
            } catch(const std::exception& error) {
                result.error=QString::fromUtf8(error.what());
            }
            return result;
        }));
    }

    void finish_symbol_load() {
        load_button_->setEnabled(true);
        load_symbol_->setEnabled(true);
        load_start_->setEnabled(true);
        load_end_->setEnabled(true);
        const auto result=load_watcher_->result();
        if(!result.error.isEmpty()) {
            load_status_->setText("Load failed: "+result.error);
            return;
        }
        load_status_->setText(result.cached
            ? result.symbol+" is already cached for the selected range."
            : result.downloaded==0
                ? "Alpaca returned no IEX one-minute bars for "+result.symbol+" in the selected range."
                : "Loaded "+QString::number(result.downloaded)+" one-minute bars for "+result.symbol+".");
        if(result.database_path==database_path_) {
            if(result.downloaded>0) intraday_study_cache_.clear();
            load_stock_symbols();
            const auto index=stock_symbol_->findText(result.symbol);
            if(index>=0) stock_symbol_->setCurrentIndex(index);
        }
    }

    void plot_underlying() {
        try {
            const auto symbol=stock_symbol_->currentText();
            const auto bars=bar_store_->load({symbol.toStdString(),"alpaca",
                "iex",options::data::stock_bar_timeframe,"all",
                stock_start_->date().toString(Qt::ISODate).toStdString(),
                stock_end_->date().toString(Qt::ISODate).toStdString()});
            if(bars.empty()) throw std::runtime_error("no bars found in the selected range");
            const CandleResolution resolution{
                candle_interval_->currentData().toInt(),candle_interval_->currentText()};
            const auto chart_candles=aggregate_chart_candles(bars,resolution);
            if(chart_candles.empty())
                throw std::runtime_error("no regular-session bars found in the selected range");
            auto* chart=new QChart;
            chart->setTitle(symbol+" Regular Session — "+resolution.label+" Candles");
            auto* candles=new QCandlestickSeries;
            candles->setName(symbol+" OHLC");
            candles->setIncreasingColor(QColor("#2e7d32"));
            candles->setDecreasingColor(QColor("#c62828"));
            candles->setPen(QPen(QColor("#333333"),1));
            candles->setBodyOutlineVisible(true);
            candles->setBodyWidth(0.8);
            candles->setMinimumColumnWidth(2.0);
            candles->setMaximumColumnWidth(12.0);
            candles->setCapsVisible(true);
            candles->setCapsWidth(0.5);
            double price_min=std::numeric_limits<double>::max();
            double price_max=std::numeric_limits<double>::lowest();
            for(std::size_t index=0;index<chart_candles.size();++index) {
                const auto& bar=chart_candles[index];
                auto* candle=new QCandlestickSet(
                    bar.open,bar.high,bar.low,bar.close,static_cast<qreal>(index));
                if(bar.open==bar.close) {
                    candle->setBrush(QColor("#757575"));
                    candle->setPen(QPen(QColor("#555555"),1));
                }
                candles->append(candle);
                price_min=std::min(price_min,bar.low);
                price_max=std::max(price_max,bar.high);
            }
            chart->addSeries(candles);
            chart->legend()->hide();
            auto* candle_positions=new QBarCategoryAxis;
            QStringList position_categories;
            position_categories.reserve(static_cast<qsizetype>(chart_candles.size()));
            for(std::size_t index=0;index<chart_candles.size();++index)
                position_categories.push_back(QString::number(index));
            candle_positions->append(position_categories);
            candle_positions->setVisible(false);
            auto* dates=new QCategoryAxis;
            dates->setTitleText("Market time (ET; closed periods omitted)");
            dates->setLabelsPosition(QCategoryAxis::AxisLabelsPositionOnValue);
            dates->setStartValue(-0.5);
            dates->setRange(-0.5,static_cast<qreal>(chart_candles.size())-0.5);
            const auto label_step=std::max<std::size_t>(1,(chart_candles.size()+7)/8);
            const auto label_format=resolution.minutes==0
                ? QString("MMM d yyyy")
                : chart_candles.front().timestamp.date()==chart_candles.back().timestamp.date()
                    ? QString("HH:mm") : QString("MMM d HH:mm");
            for(std::size_t index=0;index<chart_candles.size();index+=label_step)
                dates->append(chart_candles[index].timestamp.toString(label_format),
                    static_cast<qreal>(index));
            if((chart_candles.size()-1)%label_step!=0)
                dates->append(chart_candles.back().timestamp.toString(label_format),
                    static_cast<qreal>(chart_candles.size()-1));
            auto* values=new QValueAxis; values->setLabelFormat("$%.2f"); values->setTitleText("Price");
            const auto spread=price_max-price_min;
            const auto padding=spread>0.0 ? spread*0.05 : qMax(1.0,std::abs(price_min)*0.01);
            values->setRange(price_min-padding,price_max+padding);
            chart->addAxis(candle_positions,Qt::AlignBottom);
            chart->addAxis(dates,Qt::AlignBottom);
            chart->addAxis(values,Qt::AlignLeft);
            candles->attachAxis(candle_positions);
            candles->attachAxis(values);
            stock_chart_view_->setChart(chart);
            stock_chart_view_->set_candles(candles,chart_candles);
        } catch(const std::exception& error) {
            Q_UNUSED(error);
        }
    }

    QString database_path_;
    std::unique_ptr<options::data::SqliteBarStore> bar_store_;
    QComboBox *stock_symbol_{},*strategy_{},*candle_interval_{};
    QDateEdit *stock_start_{},*stock_end_{};
    QDate available_start_,available_end_;
    TimelineChartView* stock_chart_view_{};
    QComboBox* saved_studies_{};
    QPushButton *load_study_button_{},*delete_study_button_{};
    QLineEdit* load_symbol_{};
    QDateEdit *load_start_{},*load_end_{};
    QPushButton* load_button_{};
    QLabel* load_status_{};
    QFutureWatcher<LoadResult>* load_watcher_{};
    std::map<QString,std::shared_ptr<CachedIntradayDistributionStudy>> intraday_study_cache_;
    bool candle_interval_overridden_{};
};

}  // namespace

int main(int argc,char** argv) {
    QApplication app(argc,argv);
    ResultsWindow window;
    window.show();
    return app.exec();
}
