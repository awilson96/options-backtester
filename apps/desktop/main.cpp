#include "options/analysis/momentum.hpp"
#include "options/data/sqlite_bar_store.hpp"
#include "options/providers/alpaca/alpaca_client.hpp"

#include <QApplication>
#include <QBrush>
#include <QChart>
#include <QChartView>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDateEdit>
#include <QDateTimeAxis>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHeaderView>
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
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QValueAxis>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtConcurrentRun>

#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <functional>
#include <memory>
#include <map>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
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

struct MomentumStudyRow {
    int window_days{};
    int skip_days{};
    int strike_offset{};
    options::analysis::MomentumResult result;
};

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

void show_profit_chart(QWidget* parent,const QString& title,
                       const options::analysis::MomentumResult& result) {
    QDialog dialog(parent);
    dialog.setWindowTitle("Simulated Profit — "+title);
    dialog.resize(900,600);
    auto* layout=new QVBoxLayout(&dialog);
    auto* chart=new QChart;
    chart->setTitle("Simulated account value — one contract, averaged across start phases");
    auto* series=new QLineSeries;
    series->setName("Account value");
    for(const auto& point:result.profit_curve) {
        const auto date=QDate::fromString(QString::fromStdString(point.date),Qt::ISODate);
        series->append(QDateTime(date,QTime(12,0),Qt::UTC).toMSecsSinceEpoch(),
                       result.allocation+point.cumulative_profit);
    }
    chart->addSeries(series);
    auto* dates=new QDateTimeAxis;
    dates->setFormat("MMM yyyy");
    dates->setTitleText("Exit date");
    auto* profit=new QValueAxis;
    profit->setLabelFormat("$%.2f");
    profit->setTitleText("Account value");
    chart->addAxis(dates,Qt::AlignBottom); chart->addAxis(profit,Qt::AlignLeft);
    series->attachAxis(dates); series->attachAxis(profit);
    auto* view=new QChartView(chart);
    view->setRenderHint(QPainter::Antialiasing);
    layout->addWidget(view,1);
    auto* summary=new QLabel("Allocation: $"+QString::number(result.allocation,'f',2)+
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
        strategy_=new QComboBox;
        strategy_->addItems({"Select strategy…","Momentum"});
        stock_start_=new QDateEdit; stock_end_=new QDateEdit;
        stock_start_->setCalendarPopup(true); stock_end_->setCalendarPopup(true);
        stock_controls->addWidget(new QLabel("Symbol:")); stock_controls->addWidget(stock_symbol_);
        stock_controls->addWidget(new QLabel("Start:")); stock_controls->addWidget(stock_start_);
        stock_controls->addWidget(new QLabel("End:")); stock_controls->addWidget(stock_end_);
        stock_controls->addWidget(new QLabel("Strategy:")); stock_controls->addWidget(strategy_);
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
        connect(strategy_,&QComboBox::activated,this,[this](int index){
            if(index==1) show_momentum_analysis();
            strategy_->setCurrentIndex(0);
        });
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

    void show_momentum_analysis() {
        const auto symbol=stock_symbol_->currentText();
        if(symbol.isEmpty() || !available_start_.isValid() || !available_end_.isValid()) {
            QMessageBox::information(this,"Momentum Analysis","Select a symbol with stored daily bars first.");
            return;
        }

        QDialog dialog(this);
        dialog.setWindowTitle("Momentum Analysis — "+symbol);
        dialog.setMinimumSize(900,660);
        dialog.resize(1040,760);
        auto* layout=new QVBoxLayout(&dialog);
        auto* description=new QLabel(
            "For each eligible trading-day close q, this compares the close at the first trading "
            "day on or after q plus x calendar days. The skip window d controls when the next q "
            "becomes eligible. Results are averaged across all d possible start phases. Optional "
            "strike adjustment compares r with a strike-grid price derived from q instead of q itself.");
        description->setWordWrap(true);
        layout->addWidget(description);

        auto* controls=new QFormLayout;
        auto* parametric=new QCheckBox("Run a parametric study");
        auto* window_days=new QSpinBox;
        window_days->setRange(1,3650);
        window_days->setValue(30);
        window_days->setSuffix(" day(s)");
        auto* skip_days=new QSpinBox;
        skip_days->setRange(1,3650);
        skip_days->setValue(1);
        skip_days->setSuffix(" day(s)");
        auto* strike_enabled=new QCheckBox("Compare against an option strike");
        auto* strike_width=new QDoubleSpinBox;
        strike_width->setRange(0.01,10000.0);
        strike_width->setDecimals(2);
        strike_width->setValue(5.0);
        strike_width->setPrefix("$");
        strike_width->setEnabled(false);
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
        auto* analysis_start=new QDateEdit(qMax(available_start_,available_end_.addYears(-1)));
        auto* analysis_end=new QDateEdit(available_end_);
        analysis_start->setCalendarPopup(true); analysis_end->setCalendarPopup(true);
        analysis_start->setDateRange(available_start_,available_end_);
        analysis_end->setDateRange(available_start_,available_end_);
        auto* window_days_label=new QLabel("Comparison window (x)");
        auto* skip_days_label=new QLabel("Skip window (d)");
        auto* strike_offset_label=new QLabel("Strike offset");
        controls->addRow("Ticker",new QLabel(symbol));
        controls->addRow("Study mode",parametric);
        controls->addRow(window_days_label,window_days);
        controls->addRow(skip_days_label,skip_days);
        controls->addRow("Strike analysis",strike_enabled);
        controls->addRow("Strike width",strike_width);
        controls->addRow(strike_offset_label,strike_offset);
        controls->addRow("Pseudo-backtest",simulated_pricing);
        controls->addRow("Trading allocation",allocation);
        controls->addRow("Slippage sides",slippage_mode);
        controls->addRow("Buy slippage",buy_slippage);
        controls->addRow("Sell slippage",sell_slippage);
        controls->addRow("Analysis start",analysis_start);
        controls->addRow("Analysis end",analysis_end);
        layout->addLayout(controls);

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
        pricing_editor->setVisible(false);
        layout->addWidget(pricing_editor);

        auto* single_results=new QWidget;
        auto* results=new QFormLayout(single_results);
        auto* win_percentage=new QLabel("—");
        auto* wins=new QLabel("—");
        auto* losses=new QLabel("—");
        auto* ties=new QLabel("—");
        auto* comparisons=new QLabel("—");
        results->addRow("Win Rate %",win_percentage);
        results->addRow("Average wins per start",wins);
        results->addRow("Average losses per start",losses);
        results->addRow("Average ties per start",ties);
        results->addRow("Average comparisons per start",comparisons);
        layout->addWidget(single_results);

        auto* study_table=new QTableWidget(0,10);
        study_table->setHorizontalHeaderLabels(
            {"Rank","DTE","Skip","Strike offset","Win Rate %","Avg wins",
             "Avg losses","Avg comparisons","Total profit","Profit %"});
        study_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        study_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        study_table->setSelectionBehavior(QAbstractItemView::SelectRows);
        study_table->horizontalHeader()->setSortIndicatorShown(false);
        study_table->setColumnHidden(8,true);
        study_table->setColumnHidden(9,true);
        study_table->setVisible(false);
        layout->addWidget(study_table,1);

        auto* analysis_status=new QLabel;
        analysis_status->setWordWrap(true);
        layout->addWidget(analysis_status);
        auto* analyze_button=new QPushButton("Run Analysis");
        layout->addWidget(analyze_button,0,Qt::AlignRight);

        int active_sort_column=-1;
        int sort_cycle=0;
        std::vector<MomentumStudyRow> study_rows;
        connect(study_table->horizontalHeader(),&QHeaderView::sectionClicked,&dialog,
            [=,&active_sort_column,&sort_cycle](int column) {
                if(study_table->rowCount()==0) return;
                if(column!=active_sort_column) {
                    active_sort_column=column;
                    sort_cycle=1;
                } else {
                    sort_cycle=(sort_cycle+1)%3;
                }
                if(sort_cycle==0) {
                    study_table->sortItems(0,Qt::AscendingOrder);
                    study_table->horizontalHeader()->setSortIndicatorShown(false);
                    active_sort_column=-1;
                    return;
                }
                const auto order=sort_cycle==1 ? Qt::DescendingOrder : Qt::AscendingOrder;
                study_table->horizontalHeader()->setSortIndicator(column,order);
                study_table->horizontalHeader()->setSortIndicatorShown(true);
                study_table->sortItems(column,order);
            });

        connect(study_table,&QTableWidget::cellDoubleClicked,&dialog,
            [&,symbol](int row,int) {
                if(!simulated_pricing->isChecked()) return;
                const auto* item=study_table->item(row,0);
                if(!item) return;
                const auto index=item->data(Qt::UserRole).toULongLong();
                if(index>=study_rows.size()) return;
                const auto& value=study_rows[static_cast<std::size_t>(index)];
                show_profit_chart(&dialog,symbol+"  x="+QString::number(value.window_days)+
                    " d="+QString::number(value.skip_days)+
                    " strike="+QString::number(value.strike_offset),value.result);
            });

        const auto rebuild_pricing=[=] {
            if(!simulated_pricing->isChecked() || !strike_enabled->isChecked()) {
                pricing_editor->setVisible(false);
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
            pricing_editor->setVisible(true);
        };
        const auto selected_slippage_mode=[=] {
            switch(slippage_mode->currentIndex()) {
            case 1: return options::analysis::SlippageMode::sell;
            case 2: return options::analysis::SlippageMode::buy;
            case 3: return options::analysis::SlippageMode::buy_and_sell;
            default: return options::analysis::SlippageMode::none;
            }
        };

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
        });
        connect(strike_enabled,&QCheckBox::toggled,&dialog,[=](bool enabled) {
            strike_width->setEnabled(enabled);
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
            rebuild_pricing();
        });
        connect(slippage_mode,&QComboBox::currentIndexChanged,&dialog,[=](int index) {
            buy_slippage->setEnabled(simulated_pricing->isChecked() && (index==2 || index==3));
            sell_slippage->setEnabled(simulated_pricing->isChecked() && (index==1 || index==3));
        });
        connect(strike_offset,&QSpinBox::valueChanged,&dialog,[=](int) { rebuild_pricing(); });
        connect(strike_minimum,&QSpinBox::valueChanged,&dialog,[=](int) { rebuild_pricing(); });
        connect(strike_maximum,&QSpinBox::valueChanged,&dialog,[=](int) { rebuild_pricing(); });

        const auto analyze=[&,symbol] {
            if(analysis_start->date()>analysis_end->date()) {
                analysis_status->setText("The analysis start must not be after the end.");
                return;
            }
            try {
                const auto bars=bar_store_->load({symbol.toStdString(),"alpaca","iex","1Day","all",
                    analysis_start->date().toString(Qt::ISODate).toStdString(),
                    analysis_end->date().toString(Qt::ISODate).toStdString()});
                if(parametric->isChecked()) {
                    if(window_minimum->value()>window_maximum->value() ||
                       skip_minimum->value()>skip_maximum->value() ||
                       (strike_enabled->isChecked() && strike_minimum->value()>strike_maximum->value())) {
                        analysis_status->setText("Each parameter minimum must not exceed its maximum.");
                        return;
                    }
                    const auto window_count=window_maximum->value()-window_minimum->value()+1;
                    const auto skip_count=skip_maximum->value()-skip_minimum->value()+1;
                    const auto strike_count=strike_enabled->isChecked()
                        ? strike_maximum->value()-strike_minimum->value()+1 : 1;
                    const auto combinations=static_cast<qint64>(window_count)*skip_count*strike_count;
                    if(combinations>10000) {
                        analysis_status->setText(
                            "This study has "+QString::number(combinations)+
                            " combinations. Narrow the ranges to 10,000 or fewer.");
                        return;
                    }
                    study_rows.clear();
                    study_rows.reserve(static_cast<std::size_t>(combinations));
                    for(int x=window_minimum->value();x<=window_maximum->value();++x) {
                        for(int d=skip_minimum->value();d<=skip_maximum->value();++d) {
                            const auto first_strike=strike_enabled->isChecked() ? strike_minimum->value() : 0;
                            const auto last_strike=strike_enabled->isChecked() ? strike_maximum->value() : 0;
                            for(int offset=first_strike;offset<=last_strike;++offset) {
                                std::optional<options::analysis::StrikeAdjustment> adjustment;
                                if(strike_enabled->isChecked())
                                    adjustment=options::analysis::StrikeAdjustment{strike_width->value(),offset};
                                std::optional<options::analysis::SimulatedPricing> pricing;
                                if(simulated_pricing->isChecked()) pricing=pricing_editor->pricing_for(offset);
                                if(pricing) {
                                    pricing->allocation=allocation->value();
                                    pricing->buy_slippage_per_share=buy_slippage->value();
                                    pricing->sell_slippage_per_share=sell_slippage->value();
                                    pricing->slippage_mode=selected_slippage_mode();
                                }
                                if(simulated_pricing->isChecked() && !pricing) {
                                    analysis_status->setText(
                                        "Simulated pricing is missing for strike offset "+QString::number(offset)+".");
                                    return;
                                }
                                study_rows.push_back({x,d,offset,options::analysis::analyze_momentum(
                                    bars,static_cast<std::size_t>(x),static_cast<std::size_t>(d),adjustment,pricing)});
                            }
                        }
                    }
                    active_sort_column=-1;
                    sort_cycle=0;
                    study_table->horizontalHeader()->setSortIndicatorShown(false);
                    study_table->setColumnHidden(8,!simulated_pricing->isChecked());
                    study_table->setColumnHidden(9,!simulated_pricing->isChecked());
                    study_table->setRowCount(static_cast<int>(study_rows.size()));
                    std::vector<std::size_t> comparison_rank(study_rows.size());
                    std::iota(comparison_rank.begin(),comparison_rank.end(),0);
                    std::ranges::sort(comparison_rank,[&study_rows](auto left,auto right) {
                        if(study_rows[left].result.comparisons!=study_rows[right].result.comparisons)
                            return study_rows[left].result.comparisons>study_rows[right].result.comparisons;
                        return study_rows[left].result.win_percentage>study_rows[right].result.win_percentage;
                    });
                    comparison_rank.resize(qMin<std::size_t>(10,comparison_rank.size()));
                    for(int row=0;row<static_cast<int>(study_rows.size());++row) {
                        const auto& value=study_rows[static_cast<std::size_t>(row)];
                        const QString cells[]{QString::number(row+1),QString::number(value.window_days),
                            QString::number(value.skip_days),strike_enabled->isChecked()
                                ? QString::number(value.strike_offset) : "Off",
                            QString::number(value.result.win_percentage,'f',2)+"%",
                            averaged_count(value.result.wins),averaged_count(value.result.losses),
                            averaged_count(value.result.comparisons),
                            "$"+QString::number(value.result.total_profit,'f',2),
                            QString::number(value.result.profit_percentage,'f',2)+"%"};
                        const double numeric_values[]{static_cast<double>(row+1),
                            static_cast<double>(value.window_days),static_cast<double>(value.skip_days),
                            static_cast<double>(value.strike_offset),value.result.win_percentage,
                            value.result.wins,value.result.losses,value.result.comparisons,
                            value.result.total_profit,value.result.profit_percentage};
                        const auto highlight=std::ranges::find(
                            comparison_rank,static_cast<std::size_t>(row))!=comparison_rank.end();
                        for(int column=0;column<10;++column) {
                            auto* item=new NumericTableWidgetItem(cells[column],numeric_values[column]);
                            item->setData(Qt::UserRole,static_cast<qulonglong>(row));
                            if(highlight) item->setBackground(QBrush(QColor("#c8e6c9")));
                            study_table->setItem(row,column,item);
                        }
                    }
                    analysis_status->setText(
                        "Generated "+QString::number(study_rows.size())+
                        " parameter combinations. Click a header to sort"+
                        (simulated_pricing->isChecked()
                            ? "; double-click a row to view its cumulative profit chart." : "."));
                    return;
                }
                std::optional<options::analysis::StrikeAdjustment> adjustment;
                if(strike_enabled->isChecked())
                    adjustment=options::analysis::StrikeAdjustment{
                        strike_width->value(),strike_offset->value()};
                std::optional<options::analysis::SimulatedPricing> pricing;
                if(simulated_pricing->isChecked()) pricing=pricing_editor->pricing_for(strike_offset->value());
                if(pricing) {
                    pricing->allocation=allocation->value();
                    pricing->buy_slippage_per_share=buy_slippage->value();
                    pricing->sell_slippage_per_share=sell_slippage->value();
                    pricing->slippage_mode=selected_slippage_mode();
                }
                if(simulated_pricing->isChecked() && !pricing) {
                    analysis_status->setText("Simulated pricing is missing for this strike offset.");
                    return;
                }
                const auto result=options::analysis::analyze_momentum(
                    bars,static_cast<std::size_t>(window_days->value()),
                    static_cast<std::size_t>(skip_days->value()),adjustment,pricing);
                win_percentage->setText(QString::number(result.win_percentage,'f',2)+"%");
                wins->setText(averaged_count(result.wins));
                losses->setText(averaged_count(result.losses));
                ties->setText(averaged_count(result.ties));
                comparisons->setText(averaged_count(result.comparisons));
                analysis_status->setText(result.comparisons==0
                    ? "The selected analysis range is too short for this comparison window."
                    : QString());
                if(simulated_pricing->isChecked() && result.comparisons!=0)
                    show_profit_chart(&dialog,symbol+"  x="+QString::number(window_days->value())+
                        " d="+QString::number(skip_days->value())+
                        " strike="+QString::number(strike_offset->value()),result);
            } catch(const std::exception& error) {
                analysis_status->setText("Analysis failed: "+QString::fromUtf8(error.what()));
            }
        };
        connect(analyze_button,&QPushButton::clicked,&dialog,analyze);
        analyze();
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
    QComboBox *stock_symbol_{},*strategy_{};
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
