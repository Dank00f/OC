#include "MainWindow.h"

#include <QtWidgets>
#include <QNetworkReply>
#include <QUrlQuery>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

#include <QtCharts/QChartView>
#include <QtCharts/QChart>
#include <QtCharts/QLineSeries>
#include <QtCharts/QValueAxis>

using namespace QtCharts;

static QDateTime parseIsoUtc(QString s) {
    if (s.endsWith("Z")) s.chop(1);
    QDateTime dt = QDateTime::fromString(s, Qt::ISODate);
    dt.setTimeSpec(Qt::UTC);
    return dt;
}

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    setWindowTitle("Lab6: Temp GUI Client (Qt)");

    auto *cw = new QWidget(this);
    auto *root = new QVBoxLayout(cw);

    auto *top = new QHBoxLayout();
    top->addWidget(new QLabel("Base URL:", this));
    m_baseUrlEdit = new QLineEdit("http://127.0.0.1:8080", this);
    top->addWidget(m_baseUrlEdit, 1);

    m_btnCurrent = new QPushButton("Current", this);
    top->addWidget(m_btnCurrent);
    root->addLayout(top);

    m_currentLabel = new QLabel("Current: (not fetched)", this);
    m_currentLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    root->addWidget(m_currentLabel);

    auto *period = new QHBoxLayout();
    period->addWidget(new QLabel("From (UTC):", this));
    m_fromEdit = new QDateTimeEdit(QDateTime::currentDateTimeUtc().addSecs(-3600), this);
    m_fromEdit->setTimeSpec(Qt::UTC);
    m_fromEdit->setDisplayFormat("yyyy-MM-dd HH:mm:ss 'UTC'");
    period->addWidget(m_fromEdit);

    period->addWidget(new QLabel("To (UTC):", this));
    m_toEdit = new QDateTimeEdit(QDateTime::currentDateTimeUtc(), this);
    m_toEdit->setTimeSpec(Qt::UTC);
    m_toEdit->setDisplayFormat("yyyy-MM-dd HH:mm:ss 'UTC'");
    period->addWidget(m_toEdit);

    m_btnStats = new QPushButton("Stats", this);
    period->addWidget(m_btnStats);
    root->addLayout(period);

    m_statsLabel = new QLabel("Stats: (not fetched)", this);
    m_statsLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    root->addWidget(m_statsLabel);

    m_table = new QTableWidget(this);
    m_table->setColumnCount(2);
    m_table->setHorizontalHeaderLabels({"ts (UTC)", "temp"});
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    root->addWidget(m_table, 1);

    m_series = new QLineSeries(this);
    auto *chart = new QChart();
    chart->addSeries(m_series);
    chart->legend()->hide();
    chart->setTitle("Temperature over period");

    m_axisX = new QValueAxis(this);
    m_axisY = new QValueAxis(this);
    m_axisX->setTitleText("time (unix sec)");
    m_axisY->setTitleText("temp");
    chart->addAxis(m_axisX, Qt::AlignBottom);
    chart->addAxis(m_axisY, Qt::AlignLeft);
    m_series->attachAxis(m_axisX);
    m_series->attachAxis(m_axisY);

    m_chartView = new QChartView(chart, this);
    m_chartView->setRenderHint(QPainter::Antialiasing);
    root->addWidget(m_chartView, 2);

    m_statusLabel = new QLabel("Status: ready", this);
    root->addWidget(m_statusLabel);

    setCentralWidget(cw);

    connect(m_btnCurrent, &QPushButton::clicked, this, &MainWindow::fetchCurrent);
    connect(m_btnStats, &QPushButton::clicked, this, &MainWindow::fetchStats);

    auto *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &MainWindow::fetchCurrent);
    timer->start(2000);
}

QString MainWindow::baseUrl() const {
    QString u = m_baseUrlEdit->text().trimmed();
    if (u.endsWith("/")) u.chop(1);
    return u;
}

QUrl MainWindow::makeUrl(const QString& path, const QMap<QString, QString>& query) const {
    QUrl url(baseUrl() + path);
    if (!query.isEmpty()) {
        QUrlQuery q;
        for (auto it = query.begin(); it != query.end(); ++it) q.addQueryItem(it.key(), it.value());
        url.setQuery(q);
    }
    return url;
}

void MainWindow::setStatus(const QString& s) {
    m_statusLabel->setText("Status: " + s);
}

void MainWindow::showCurrent(double temp, const QString& ts) {
    m_currentLabel->setText(QString("Current: %1 °C  @ %2").arg(temp, 0, 'f', 3).arg(ts));
}

void MainWindow::showStats(double avg, int count, double minv, double maxv) {
    m_statsLabel->setText(QString("Stats: avg=%1  count=%2  min=%3  max=%4")
                          .arg(avg, 0, 'f', 3).arg(count).arg(minv, 0, 'f', 3).arg(maxv, 0, 'f', 3));
}

bool MainWindow::parseCurrentJson(const QByteArray& body, double& temp, QString& ts, QString& err) {
    auto doc = QJsonDocument::fromJson(body);
    if (!doc.isObject()) { err = "current: json is not object"; return false; }
    auto o = doc.object();
    if (!o.contains("temp") || !o.contains("ts")) { err = "current: missing temp/ts"; return false; }
    temp = o.value("temp").toDouble();
    ts = o.value("ts").toString();
    return true;
}

double MainWindow::computeAvg(const QVector<QPair<QDateTime,double>>& series) const {
    if (series.isEmpty()) return 0.0;
    double s = 0.0;
    for (auto& p : series) s += p.second;
    return s / double(series.size());
}

bool MainWindow::parseStatsJson(const QByteArray& body, double& avg, int& count, double& minv, double& maxv,
                                QVector<QPair<QDateTime,double>>& series, QString& err) {
    auto doc = QJsonDocument::fromJson(body);
    if (!doc.isObject()) { err = "stats: json is not object"; return false; }
    auto o = doc.object();

    bool hasAgg = o.contains("avg") && o.contains("count");
    if (hasAgg) {
        avg = o.value("avg").toDouble();
        count = o.value("count").toInt();
        minv = o.contains("min") ? o.value("min").toDouble() : avg;
        maxv = o.contains("max") ? o.value("max").toDouble() : avg;
    } else {
        avg = 0; count = 0; minv = 0; maxv = 0;
    }

    QJsonArray arr;
    if (o.contains("measurements") && o.value("measurements").isArray()) arr = o.value("measurements").toArray();
    else if (o.contains("series") && o.value("series").isArray()) arr = o.value("series").toArray();

    if (!arr.isEmpty()) {
        series.clear();
        series.reserve(arr.size());
        for (const auto& v : arr) {
            if (!v.isObject()) continue;
            auto it = v.toObject();
            if (!it.contains("ts") || !it.contains("temp")) continue;
            QDateTime dt = parseIsoUtc(it.value("ts").toString());
            double tv = it.value("temp").toDouble();
            if (!dt.isValid()) continue;
            series.push_back({dt, tv});
        }
        std::sort(series.begin(), series.end(), [](auto& a, auto& b){ return a.first < b.first; });

        if (!hasAgg) {
            avg = computeAvg(series);
            count = series.size();
            if (count > 0) {
                minv = series[0].second;
                maxv = series[0].second;
                for (auto& p : series) { minv = std::min(minv, p.second); maxv = std::max(maxv, p.second); }
            } else {
                minv = maxv = avg;
            }
        }
        return true;
    }

    if (hasAgg) return true;

    err = "stats: neither (avg,count) nor measurements array";
    return false;
}

void MainWindow::updateChart(const QVector<QPair<QDateTime,double>>& series) {
    m_series->clear();
    if (series.isEmpty()) return;

    double xmin = series.first().first.toSecsSinceEpoch();
    double xmax = series.last().first.toSecsSinceEpoch();

    double ymin = series.first().second;
    double ymax = series.first().second;

    for (auto& p : series) {
        double x = p.first.toSecsSinceEpoch();
        double y = p.second;
        m_series->append(x, y);
        ymin = std::min(ymin, y);
        ymax = std::max(ymax, y);
    }

    if (xmin == xmax) xmax = xmin + 1;
    if (ymin == ymax) { ymin -= 1; ymax += 1; }

    m_axisX->setRange(xmin, xmax);
    m_axisY->setRange(ymin, ymax);
}

void MainWindow::fetchCurrent() {
    setStatus("GET /api/current ...");
    auto url = makeUrl("/api/current");
    auto *reply = m_net.get(QNetworkRequest(url));

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        QByteArray body = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            setStatus("current error: " + reply->errorString());
            reply->deleteLater();
            return;
        }
        double temp = 0.0; QString ts; QString err;
        if (!parseCurrentJson(body, temp, ts, err)) {
            setStatus(err);
            reply->deleteLater();
            return;
        }
        showCurrent(temp, ts);
        setStatus("ok (current)");
        reply->deleteLater();
    });
}

void MainWindow::fetchStats() {
    QDateTime from = m_fromEdit->dateTime().toUTC();
    QDateTime to   = m_toEdit->dateTime().toUTC();
    if (!from.isValid() || !to.isValid() || from >= to) {
        setStatus("bad period: from>=to");
        return;
    }

    QString fromIso = from.toString(Qt::ISODate) + "Z";
    QString toIso   = to.toString(Qt::ISODate) + "Z";

    setStatus("GET /api/stats ...");
    auto url = makeUrl("/api/stats", {{"from", fromIso}, {"to", toIso}});
    auto *reply = m_net.get(QNetworkRequest(url));

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        QByteArray body = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            setStatus("stats error: " + reply->errorString());
            reply->deleteLater();
            return;
        }

        double avg=0, minv=0, maxv=0;
        int count=0;
        QVector<QPair<QDateTime,double>> series;
        QString err;

        if (!parseStatsJson(body, avg, count, minv, maxv, series, err)) {
            setStatus(err);
            reply->deleteLater();
            return;
        }

        showStats(avg, count, minv, maxv);

        m_table->setRowCount(0);
        if (!series.isEmpty()) {
            int showN = std::min(50, series.size());
            m_table->setRowCount(showN);
            int start = series.size() - showN;
            for (int i=0;i<showN;i++) {
                const auto& p = series[start+i];
                m_table->setItem(i, 0, new QTableWidgetItem(p.first.toString(Qt::ISODate) + "Z"));
                m_table->setItem(i, 1, new QTableWidgetItem(QString::number(p.second, 'f', 3)));
            }
            updateChart(series);
        } else {
            m_series->clear();
        }

        setStatus("ok (stats)");
        reply->deleteLater();
    });
}
