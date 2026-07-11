#include "options/backtest/long_call_backtest.hpp"
#include "options/data/sqlite_bar_store.hpp"
#include "options/data/sqlite_option_store.hpp"

#include <QApplication>
#include <QChart>
#include <QChartView>
#include <QComboBox>
#include <QDateEdit>
#include <QDateTimeAxis>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineSeries>
#include <QMainWindow>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTableWidget>
#include <QTabWidget>
#include <QValueAxis>
#include <QVBoxLayout>

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

QString percentage(double value) { return QString::number(value*100.0,'f',2)+"%"; }

QLineSeries* make_series(const QString& name,
                         const std::vector<options::backtest::EquityPoint>& points,
                         double initial) {
    auto* result=new QLineSeries;
    result->setName(name);
    for(const auto& point:points) {
        const auto date=QDate::fromString(QString::fromStdString(point.date),Qt::ISODate);
        result->append(QDateTime(date,QTime(12,0),Qt::UTC).toMSecsSinceEpoch(),point.value/initial);
    }
    return result;
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

class ResultsWindow final : public QMainWindow {
public:
    ResultsWindow() {
        setWindowTitle("Options Backtester");
        resize(1250,850);
        auto* central=new QWidget;
        auto* page=new QVBoxLayout(central);

        auto* database_row=new QHBoxLayout;
        database_label_=new QLabel;
        auto* open_button=new QPushButton("Open Database…");
        database_row->addWidget(new QLabel("Database:"));
        database_row->addWidget(database_label_,1);
        database_row->addWidget(open_button);
        page->addLayout(database_row);

        auto* tabs=new QTabWidget;
        auto* stock_page=new QWidget;
        auto* stock_layout=new QVBoxLayout(stock_page);
        auto* stock_controls=new QHBoxLayout;
        stock_symbol_=new QComboBox;
        stock_feed_=new QComboBox; stock_feed_->addItems({"iex","sip"});
        stock_start_=new QDateEdit; stock_end_=new QDateEdit;
        stock_start_->setCalendarPopup(true); stock_end_->setCalendarPopup(true);
        plot_stock_button_=new QPushButton("Plot Underlying");
        stock_controls->addWidget(new QLabel("Symbol:")); stock_controls->addWidget(stock_symbol_);
        stock_controls->addWidget(new QLabel("Feed:")); stock_controls->addWidget(stock_feed_);
        stock_controls->addWidget(new QLabel("Start:")); stock_controls->addWidget(stock_start_);
        stock_controls->addWidget(new QLabel("End:")); stock_controls->addWidget(stock_end_);
        stock_controls->addWidget(plot_stock_button_); stock_controls->addStretch();
        stock_layout->addLayout(stock_controls);
        stock_chart_view_=new QChartView; stock_chart_view_->setRenderHint(QPainter::Antialiasing);
        stock_layout->addWidget(stock_chart_view_,1);
        stock_status_=new QLabel("Select an underlying with stored equity bars.");
        stock_layout->addWidget(stock_status_);
        tabs->addTab(stock_page,"Underlying Price");

        auto* backtest_page=new QWidget;
        auto* backtest_layout=new QVBoxLayout(backtest_page);
        tabs->addTab(backtest_page,"Options Backtest");
        page->addWidget(tabs,1);

        auto* controls=new QGroupBox("Long-call strategy");
        auto* grid=new QGridLayout(controls);
        underlying_=new QComboBox; option_feed_=new QComboBox; equity_feed_=new QComboBox;
        equity_feed_->addItems({"iex","sip"});
        start_=new QDateEdit; end_=new QDateEdit;
        start_->setCalendarPopup(true); end_->setCalendarPopup(true);
        target_delta_=double_box(0.01,0.99,0.50,2);
        minimum_dte_=integer_box(0,365,30); maximum_dte_=integer_box(0,730,45);
        hold_sessions_=integer_box(1,252,5);
        maximum_spread_=double_box(0.0,5.0,0.25,2);
        minimum_open_interest_=integer_box(0,10000000,0);
        cash_=double_box(100.0,100000000.0,10000.0,2); cash_->setSingleStep(1000.0);
        run_button_=new QPushButton("Run Backtest");

        add_control(grid,0,0,"Underlying",underlying_); add_control(grid,0,2,"Option feed",option_feed_);
        add_control(grid,0,4,"Equity feed",equity_feed_);
        add_control(grid,1,0,"Start",start_); add_control(grid,1,2,"End",end_);
        add_control(grid,1,4,"Starting cash",cash_);
        add_control(grid,2,0,"Target delta",target_delta_); add_control(grid,2,2,"Minimum DTE",minimum_dte_);
        add_control(grid,2,4,"Maximum DTE",maximum_dte_);
        add_control(grid,3,0,"Hold sessions",hold_sessions_); add_control(grid,3,2,"Maximum spread",maximum_spread_);
        add_control(grid,3,4,"Minimum open interest",minimum_open_interest_);
        grid->addWidget(run_button_,4,4,1,2);
        backtest_layout->addWidget(controls);

        auto* body=new QHBoxLayout;
        auto* summary_box=new QGroupBox("Results");
        auto* summary=new QFormLayout(summary_box);
        strategy_equity_=new QLabel("—"); strategy_return_=new QLabel("—");
        strategy_drawdown_=new QLabel("—"); benchmark_return_=new QLabel("—");
        completed_trades_=new QLabel("—"); rejected_markets_=new QLabel("—");
        summary->addRow("Strategy final equity",strategy_equity_);
        summary->addRow("Strategy return",strategy_return_);
        summary->addRow("Maximum drawdown",strategy_drawdown_);
        summary->addRow("Buy & hold return",benchmark_return_);
        summary->addRow("Completed trades",completed_trades_);
        summary->addRow("Rejected markets",rejected_markets_);
        summary_box->setMaximumWidth(270); body->addWidget(summary_box);

        chart_view_=new QChartView; chart_view_->setRenderHint(QPainter::Antialiasing);
        body->addWidget(chart_view_,1); backtest_layout->addLayout(body,1);

        trades_=new QTableWidget(0,6);
        trades_->setHorizontalHeaderLabels({"Contract","Entry","Exit","Ask fill","Bid fill","P&L"});
        trades_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        trades_->setMinimumHeight(190); backtest_layout->addWidget(trades_);
        status_=new QLabel("Select a database and run a backtest."); backtest_layout->addWidget(status_);
        setCentralWidget(central);

        connect(open_button,&QPushButton::clicked,this,[this]{ choose_database(); });
        connect(underlying_,&QComboBox::currentTextChanged,this,[this]{ load_feeds(); });
        connect(option_feed_,&QComboBox::currentTextChanged,this,[this]{ load_dates(); });
        connect(stock_feed_,&QComboBox::currentTextChanged,this,[this]{ load_stock_symbols(); });
        connect(stock_symbol_,&QComboBox::currentTextChanged,this,[this]{ update_stock_range(); });
        connect(plot_stock_button_,&QPushButton::clicked,this,[this]{ plot_underlying(); });
        connect(run_button_,&QPushButton::clicked,this,[this]{ run_backtest(); });
        open_database(initial_database_path());
    }

private:
    static QDoubleSpinBox* double_box(double minimum,double maximum,double value,int decimals) {
        auto* box=new QDoubleSpinBox; box->setRange(minimum,maximum); box->setDecimals(decimals);
        box->setValue(value); return box;
    }
    static QSpinBox* integer_box(int minimum,int maximum,int value) {
        auto* box=new QSpinBox; box->setRange(minimum,maximum); box->setValue(value); return box;
    }
    static void add_control(QGridLayout* grid,int row,int column,const QString& label,QWidget* widget) {
        grid->addWidget(new QLabel(label),row,column); grid->addWidget(widget,row,column+1);
    }

    void choose_database() {
        const auto selected=QFileDialog::getOpenFileName(this,"Open market data database",
            QFileInfo(database_path_).absolutePath(),"SQLite databases (*.db);;All files (*)");
        if(!selected.isEmpty()) open_database(selected);
    }

    void open_database(const QString& path) {
        try {
            database_path_=QFileInfo(path).absoluteFilePath();
            option_store_=std::make_unique<options::data::SqliteOptionStore>(database_path_.toStdString());
            bar_store_=std::make_unique<options::data::SqliteBarStore>(database_path_.toStdString());
            QSettings("OptionsBacktester","OptionsBacktester").setValue("database",database_path_);
            database_label_->setText(database_path_);
            underlying_->blockSignals(true); underlying_->clear();
            for(const auto& value:option_store_->available_underlyings())
                underlying_->addItem(QString::fromStdString(value));
            underlying_->blockSignals(false);
            if(underlying_->count()==0) {
                status_->setText("No option snapshots were found in this database.");
                run_button_->setEnabled(false); option_feed_->clear();
            } else {
                run_button_->setEnabled(true); load_feeds();
                status_->setText("Loaded "+QString::number(underlying_->count())+" underlying symbol(s).");
            }
            load_stock_symbols();
        } catch(const std::exception& error) {
            QMessageBox::critical(this,"Open Database",error.what());
        }
    }

    void load_feeds() {
        if(!option_store_ || underlying_->currentText().isEmpty()) return;
        option_feed_->blockSignals(true); option_feed_->clear();
        for(const auto& feed:option_store_->available_snapshot_feeds(underlying_->currentText().toStdString()))
            option_feed_->addItem(QString::fromStdString(feed));
        option_feed_->blockSignals(false); load_dates(); update_stock_range();
    }

    void load_dates() {
        if(!option_store_ || underlying_->currentText().isEmpty() || option_feed_->currentText().isEmpty()) return;
        const auto range=option_store_->snapshot_date_range(underlying_->currentText().toStdString(),
                                                            option_feed_->currentText().toStdString());
        const auto first=QDate::fromString(QString::fromStdString(range.first),Qt::ISODate);
        const auto last=QDate::fromString(QString::fromStdString(range.second),Qt::ISODate);
        if(first.isValid() && last.isValid()) {
            start_->setDateRange(first,last); end_->setDateRange(first,last);
            start_->setDate(first); end_->setDate(last);
        }
    }

    void update_stock_range() {
        if(!bar_store_ || stock_symbol_->currentText().isEmpty()) return;
        const options::data::BarQuery query{stock_symbol_->currentText().toStdString(),"alpaca",
            stock_feed_->currentText().toStdString(),"1Day","all","",""};
        const auto range=bar_store_->date_range(query);
        const auto first=QDate::fromString(QString::fromStdString(range.start),Qt::ISODate);
        const auto last=QDate::fromString(QString::fromStdString(range.end),Qt::ISODate);
        const bool available=first.isValid() && last.isValid();
        plot_stock_button_->setEnabled(available);
        if(!available) {
            stock_status_->setText("No "+stock_feed_->currentText()+" daily bars are stored for "+
                                   stock_symbol_->currentText()+".");
            return;
        }
        stock_start_->setDateRange(first,last); stock_end_->setDateRange(first,last);
        stock_start_->setDate(first); stock_end_->setDate(last);
        plot_underlying();
    }

    void load_stock_symbols() {
        if(!bar_store_) return;
        const auto previous=stock_symbol_->currentText();
        stock_symbol_->blockSignals(true); stock_symbol_->clear();
        for(const auto& symbol:bar_store_->available_symbols(
                "alpaca",stock_feed_->currentText().toStdString(),"1Day","all"))
            stock_symbol_->addItem(QString::fromStdString(symbol));
        const auto previous_index=stock_symbol_->findText(previous);
        if(previous_index>=0) stock_symbol_->setCurrentIndex(previous_index);
        stock_symbol_->blockSignals(false);
        plot_stock_button_->setEnabled(stock_symbol_->count()>0);
        if(stock_symbol_->count()==0) {
            stock_status_->setText("No daily bars are stored for the selected feed.");
            return;
        }
        update_stock_range();
    }

    void plot_underlying() {
        try {
            const auto symbol=stock_symbol_->currentText();
            const auto bars=bar_store_->load({symbol.toStdString(),"alpaca",
                stock_feed_->currentText().toStdString(),"1Day","all",
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
            stock_status_->setText("Plotted "+QString::number(bars.size())+" daily bars from "+
                stock_start_->date().toString(Qt::ISODate)+" through "+stock_end_->date().toString(Qt::ISODate)+".");
        } catch(const std::exception& error) {
            stock_status_->setText("Plot failed: "+QString::fromUtf8(error.what()));
        }
    }

    void run_backtest() {
        try {
            const auto underlying=underlying_->currentText().toStdString();
            const auto start=start_->date().toString(Qt::ISODate).toStdString();
            const auto end=end_->date().toString(Qt::ISODate).toStdString();
            const auto observations=option_store_->load_snapshot_history(
                underlying,option_feed_->currentText().toStdString(),start,end);
            const auto bars=bar_store_->load({underlying,"alpaca",equity_feed_->currentText().toStdString(),
                                               "1Day","all",start,end});
            options::backtest::LongCallConfig config;
            config.initial_cash=cash_->value(); config.target_delta=target_delta_->value();
            config.minimum_dte=minimum_dte_->value(); config.maximum_dte=maximum_dte_->value();
            config.hold_sessions=static_cast<std::size_t>(hold_sessions_->value());
            config.maximum_spread_fraction=maximum_spread_->value();
            config.minimum_open_interest=minimum_open_interest_->value();
            show_result(options::backtest::run_long_call_strategy(observations,bars,config));
        } catch(const std::exception& error) {
            status_->setText("Backtest failed: "+QString::fromUtf8(error.what()));
            QMessageBox::warning(this,"Backtest",error.what());
        }
    }

    void show_result(const options::backtest::LongCallResult& result) {
        strategy_equity_->setText("$"+QString::number(result.strategy.final_equity,'f',2));
        strategy_return_->setText(percentage(result.strategy.total_return));
        strategy_drawdown_->setText(percentage(result.strategy.max_drawdown));
        benchmark_return_->setText(percentage(result.buy_and_hold.total_return));
        completed_trades_->setText(QString::number(result.trades.size()));
        rejected_markets_->setText(QString::number(result.rejected_markets));

        auto* chart=new QChart; chart->setTitle("Normalized Growth of $1 — positions marked at bid");
        auto* strategy=make_series("Long call",result.strategy_equity,result.config.initial_cash);
        auto* benchmark=make_series("Underlying buy & hold",result.buy_hold_equity,result.config.initial_cash);
        chart->addSeries(strategy); chart->addSeries(benchmark);
        auto* dates=new QDateTimeAxis; dates->setFormat("MMM d"); dates->setTitleText("Date");
        auto* values=new QValueAxis; values->setLabelFormat("%.2f"); values->setTitleText("Growth of $1");
        chart->addAxis(dates,Qt::AlignBottom); chart->addAxis(values,Qt::AlignLeft);
        strategy->attachAxis(dates); strategy->attachAxis(values);
        benchmark->attachAxis(dates); benchmark->attachAxis(values);
        chart_view_->setChart(chart);

        trades_->setRowCount(static_cast<int>(result.trades.size()));
        for(int row=0;row<static_cast<int>(result.trades.size());++row) {
            const auto& trade=result.trades[static_cast<std::size_t>(row)];
            const QString cells[]{QString::fromStdString(trade.symbol),QString::fromStdString(trade.entry_date),
                QString::fromStdString(trade.exit_date),QString::number(trade.entry_price,'f',2),
                QString::number(trade.exit_price,'f',2),QString::number(trade.pnl,'f',2)};
            for(int column=0;column<6;++column) trades_->setItem(row,column,new QTableWidgetItem(cells[column]));
        }
        status_->setText("Backtest completed using "+QString::number(result.strategy_equity.size())+
                         " snapshot sessions.");
    }

    QString database_path_;
    std::unique_ptr<options::data::SqliteOptionStore> option_store_;
    std::unique_ptr<options::data::SqliteBarStore> bar_store_;
    QLabel *database_label_{},*status_{},*strategy_equity_{},*strategy_return_{},
           *strategy_drawdown_{},*benchmark_return_{},*completed_trades_{},*rejected_markets_{};
    QComboBox *underlying_{},*option_feed_{},*equity_feed_{},*stock_symbol_{},*stock_feed_{};
    QDateEdit *start_{},*end_{};
    QDateEdit *stock_start_{},*stock_end_{};
    QDoubleSpinBox *target_delta_{},*maximum_spread_{},*cash_{};
    QSpinBox *minimum_dte_{},*maximum_dte_{},*hold_sessions_{},*minimum_open_interest_{};
    QPushButton* run_button_{};
    QPushButton* plot_stock_button_{};
    QChartView* chart_view_{};
    QChartView* stock_chart_view_{};
    QLabel* stock_status_{};
    QTableWidget* trades_{};
};

}  // namespace

int main(int argc,char** argv) {
    QApplication app(argc,argv);
    ResultsWindow window;
    window.show();
    return app.exec();
}
