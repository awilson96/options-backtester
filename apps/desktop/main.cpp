#include "options/data/sqlite_bar_store.hpp"

#include <QApplication>
#include <QChart>
#include <QChartView>
#include <QComboBox>
#include <QDateEdit>
#include <QDateTimeAxis>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineSeries>
#include <QMainWindow>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QValueAxis>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

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
            if(delta!=0) shift_window_(delta>0 ? -1 : 1);
            event->accept();
            return;
        }
        QChartView::wheelEvent(event);
    }

private:
    std::function<void(int)> shift_window_;
};

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
        stock_layout->addLayout(stock_controls);
        stock_chart_view_=new TimelineChartView([this](int direction){ shift_date_window(direction); });
        stock_chart_view_->setRenderHint(QPainter::Antialiasing);
        stock_layout->addWidget(stock_chart_view_,1);
        stock_status_=new QLabel("Select an underlying with stored equity bars.");
        stock_layout->addWidget(stock_status_);
        page->addWidget(stock_page,1);
        setCentralWidget(central);

        connect(open_button,&QPushButton::clicked,this,[this]{ choose_database(); });
        connect(stock_symbol_,&QComboBox::currentTextChanged,this,[this]{ update_stock_range(); });
        connect(stock_start_,&QDateEdit::dateChanged,this,[this]{ plot_underlying(); });
        connect(stock_end_,&QDateEdit::dateChanged,this,[this]{ plot_underlying(); });
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
            database_label_->setText(database_path_);
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
            stock_status_->setText("No iex daily bars are stored for "+stock_symbol_->currentText()+".");
            return;
        }
        available_start_=first;
        available_end_=last;
        const QSignalBlocker block_start(stock_start_);
        const QSignalBlocker block_end(stock_end_);
        stock_start_->setDateRange(first,last); stock_end_->setDateRange(first,last);
        stock_start_->setDate(first); stock_end_->setDate(last);
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
            stock_status_->setText("No iex daily bars are stored.");
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
        plot_underlying();
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
            stock_status_->setText("Plotted "+QString::number(bars.size())+" daily bars from "+
                stock_start_->date().toString(Qt::ISODate)+" through "+stock_end_->date().toString(Qt::ISODate)+".");
        } catch(const std::exception& error) {
            stock_status_->setText("Plot failed: "+QString::fromUtf8(error.what()));
        }
    }

    QString database_path_;
    std::unique_ptr<options::data::SqliteBarStore> bar_store_;
    QLabel* database_label_{};
    QComboBox* stock_symbol_{};
    QDateEdit *stock_start_{},*stock_end_{};
    QDate available_start_,available_end_;
    TimelineChartView* stock_chart_view_{};
    QLabel* stock_status_{};
};

}  // namespace

int main(int argc,char** argv) {
    QApplication app(argc,argv);
    ResultsWindow window;
    window.show();
    return app.exec();
}
