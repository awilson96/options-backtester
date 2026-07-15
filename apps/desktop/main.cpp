#include "options/data/sqlite_bar_store.hpp"
#include "options/providers/alpaca/alpaca_client.hpp"

#include <QApplication>
#include <QChart>
#include <QChartView>
#include <QComboBox>
#include <QDateEdit>
#include <QDateTimeAxis>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLineSeries>
#include <QMainWindow>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QValueAxis>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtConcurrentRun>

#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

struct LoadResult {
    QString symbol;
    QString database_path;
    std::size_t downloaded{};
    bool cached{};
    QString error;
};

std::string environment(const char* name) {
    const auto* value=std::getenv(name);
    if(!value || !*value) throw std::runtime_error(QString("Missing environment variable: "+
        QString::fromUtf8(name)).toStdString());
    return value;
}

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

class TimelineChartView final : public QChartView {
public:
    explicit TimelineChartView(std::function<void(int)> shift_window)
        : shift_window_(std::move(shift_window)) {}

protected:
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
    int pending_direction_{};
    int pending_events_{};
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
        stock_start_=new QDateEdit; stock_end_=new QDateEdit;
        stock_start_->setCalendarPopup(true); stock_end_->setCalendarPopup(true);
        stock_controls->addWidget(new QLabel("Symbol:")); stock_controls->addWidget(stock_symbol_);
        stock_controls->addWidget(new QLabel("Start:")); stock_controls->addWidget(stock_start_);
        stock_controls->addWidget(new QLabel("End:")); stock_controls->addWidget(stock_end_);
        stock_controls->addStretch();
        stock_controls->addWidget(open_button);
        stock_layout->addLayout(stock_controls);
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
        load_start_=new QDateEdit(QDate(1970,1,1));
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
        load_status_=new QLabel("Enter a symbol to load its available Alpaca IEX daily history.");
        load_status_->setWordWrap(true);
        load_layout->addWidget(load_status_);
        load_layout->addStretch();
        tabs->addTab(load_page,"Load Data");

        page->addWidget(tabs,1);
        setCentralWidget(central);

        connect(open_button,&QPushButton::clicked,this,[this]{ choose_database(); });
        connect(stock_symbol_,&QComboBox::currentTextChanged,this,[this]{ update_stock_range(); });
        connect(stock_start_,&QDateEdit::dateChanged,this,[this]{ persist_dates(); plot_underlying(); });
        connect(stock_end_,&QDateEdit::dateChanged,this,[this]{ persist_dates(); plot_underlying(); });
        connect(load_button_,&QPushButton::clicked,this,[this]{ load_symbol_data(); });
        connect(load_symbol_,&QLineEdit::returnPressed,this,[this]{ load_symbol_data(); });
        load_watcher_=new QFutureWatcher<LoadResult>(this);
        connect(load_watcher_,&QFutureWatcher<LoadResult>::finished,this,[this]{ finish_symbol_load(); });
        open_database(initial_database_path());
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
            QSettings("OptionsBacktester","OptionsBacktester").setValue("database",database_path_);
            load_stock_symbols();
        } catch(const std::exception& error) {
            QMessageBox::critical(this,"Open Database",error.what());
        }
    }

    void update_stock_range() {
        if(!bar_store_ || stock_symbol_->currentText().isEmpty()) return;
        const options::data::BarQuery query{stock_symbol_->currentText().toStdString(),"alpaca",
            "iex","1Day","all","",""};
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
        plot_underlying();
    }

    void load_stock_symbols() {
        if(!bar_store_) return;
        const auto previous=stock_symbol_->currentText();
        stock_symbol_->blockSignals(true); stock_symbol_->clear();
        for(const auto& symbol:bar_store_->available_symbols(
                "alpaca","iex","1Day","all"))
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
                    "1Day","all",requested);
                result.cached=missing.empty();
                if(missing.empty()) return result;

                options::providers::alpaca::AlpacaClient client(
                    {environment("ALPACA_API_KEY"),environment("ALPACA_API_SECRET")});
                for(const auto& range:missing) {
                    options::providers::alpaca::BarsRequest request;
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
                ? "Alpaca returned no IEX daily bars for "+result.symbol+" in the selected range."
                : "Loaded "+QString::number(result.downloaded)+" daily bars for "+result.symbol+".");
        if(result.database_path==database_path_) {
            load_stock_symbols();
            const auto index=stock_symbol_->findText(result.symbol);
            if(index>=0) stock_symbol_->setCurrentIndex(index);
        }
    }

    void plot_underlying() {
        try {
            const auto symbol=stock_symbol_->currentText();
            const auto bars=bar_store_->load({symbol.toStdString(),"alpaca",
                "iex","1Day","all",
                stock_start_->date().toString(Qt::ISODate).toStdString(),
                stock_end_->date().toString(Qt::ISODate).toStdString()});
            if(bars.empty()) throw std::runtime_error("no bars found in the selected range");
            auto* prices=new QLineSeries; prices->setName(symbol+" close");
            prices->setPointsVisible(bars.size()==1);
            for(const auto& bar:bars) {
                const auto date=QDate::fromString(QString::fromStdString(bar.timestamp.substr(0,10)),Qt::ISODate);
                prices->append(QDateTime(date,QTime(12,0),Qt::UTC).toMSecsSinceEpoch(),bar.close);
            }
            auto* chart=new QChart; chart->setTitle(symbol+" Daily Closing Price");
            chart->addSeries(prices);
            auto* dates=new QDateTimeAxis; dates->setFormat("MMM yyyy"); dates->setTitleText("Date");
            dates->setTickCount(8);
            const auto first_date=QDate::fromString(QString::fromStdString(bars.front().timestamp.substr(0,10)),Qt::ISODate);
            const auto last_date=QDate::fromString(QString::fromStdString(bars.back().timestamp.substr(0,10)),Qt::ISODate);
            const auto axis_end=last_date==first_date ? last_date.addDays(1) : last_date;
            dates->setRange(QDateTime(first_date,QTime(12,0),Qt::UTC),
                            QDateTime(axis_end,QTime(12,0),Qt::UTC));
            auto* values=new QValueAxis; values->setLabelFormat("$%.2f"); values->setTitleText("Close");
            chart->addAxis(dates,Qt::AlignBottom); chart->addAxis(values,Qt::AlignLeft);
            prices->attachAxis(dates); prices->attachAxis(values);
            stock_chart_view_->setChart(chart);
        } catch(const std::exception& error) {
            Q_UNUSED(error);
        }
    }

    QString database_path_;
    std::unique_ptr<options::data::SqliteBarStore> bar_store_;
    QComboBox* stock_symbol_{};
    QDateEdit *stock_start_{},*stock_end_{};
    QDate available_start_,available_end_;
    TimelineChartView* stock_chart_view_{};
    QLineEdit* load_symbol_{};
    QDateEdit *load_start_{},*load_end_{};
    QPushButton* load_button_{};
    QLabel* load_status_{};
    QFutureWatcher<LoadResult>* load_watcher_{};
};

}  // namespace

int main(int argc,char** argv) {
    QApplication app(argc,argv);
    ResultsWindow window;
    window.show();
    return app.exec();
}
