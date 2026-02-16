#include "MainWindow.h"

#include <QtWidgets>           // весь Qt Widgets UI: QLabel, QLineEdit, layouts, QTimer и т.д.
#include <QNetworkReply>       // ответ на HTTP запрос
#include <QUrlQuery>           // сборка query string ?from=...&to=...
#include <QJsonDocument>       // JSON bytes -> document
#include <QJsonObject>         // JSON объект { ... }
#include <QJsonArray>          // JSON массив [ ... ]

#include <QtCharts/QChartView> // окно для графика
#include <QtCharts/QChart>     // сам график
#include <QtCharts/QLineSeries>// линия на графике
#include <QtCharts/QValueAxis> // оси (числовые)

#include <algorithm>
#include <cmath>
#include <limits>

using namespace QtCharts;

static bool isFinite(double x) { return std::isfinite(x); }

// Парсинг строки времени ISO 8601 в UTC
// Сервер обычно присылает ...Z, мы убираем Z и выставляем UTC вручную. Т.К, мы считаем, что строка это время в UTC, а не локальное
static QDateTime parseIsoUtc(QString s) {
    if (s.endsWith("Z")) s.chop(1);
    QDateTime dt = QDateTime::fromString(s, Qt::ISODate);
    dt.setTimeSpec(Qt::UTC);
    return dt;
}

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    // Заголовок окна
    setWindowTitle("Lab6: Temp GUI Client (Qt)");

    // Центральный виджет и корневой layout (все элементы лежат внутри cw)
    auto *cw = new QWidget(this);
    auto *root = new QVBoxLayout(cw);

    // Верхняя панель: base url + кнопка current
    // Base URL нужен, чтобы легко переключаться между сервером lab5 или заменит на другйо
    auto *top = new QHBoxLayout();
    top->addWidget(new QLabel("Base URL:", this));
    m_baseUrlEdit = new QLineEdit("http://127.0.0.1:8080", this);
    top->addWidget(m_baseUrlEdit, 1);

    m_btnCurrent = new QPushButton("Current", this);
    top->addWidget(m_btnCurrent);
    root->addLayout(top);

    // Лейбл для отображения текущей температуры
    // selectable - чтобы копипастаит значение/время
    m_currentLabel = new QLabel("Current: (not fetched)", this);
    m_currentLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    root->addWidget(m_currentLabel);

    // Панель выбора диапазона времени + кнопка stats
    // Диапазон храним в UTC, чтобы не ловить сдвиги по таймзоне и DST
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

    // Лейбл для агрегированной статистики (avg/count/min/max)
    m_statsLabel = new QLabel("Stats: (not fetched)", this);
    m_statsLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    root->addWidget(m_statsLabel);

    // Таблица последних точек
    // Показываем только последние N точек, чтобы UI не зависал на больших периодах
    m_table = new QTableWidget(this);
    m_table->setColumnCount(2);
    m_table->setHorizontalHeaderLabels({"ts (UTC)", "temp"});
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    root->addWidget(m_table, 1);

    // График QtCharts: одна линия температуры
    // QtCharts требует модуль Charts в сборке
    m_series = new QLineSeries(this);
    auto *chart = new QChart();
    chart->addSeries(m_series);
    chart->legend()->hide();
    chart->setTitle("Temperature over period");

    // Оси графика
    // По X используем unix seconds, чтобы не тянуть QDateTimeAxis
    m_axisX = new QValueAxis(this);
    m_axisY = new QValueAxis(this);
    m_axisX->setTitleText("time (unix sec)");
    m_axisY->setTitleText("temp");
    chart->addAxis(m_axisX, Qt::AlignBottom);
    chart->addAxis(m_axisY, Qt::AlignLeft);
    m_series->attachAxis(m_axisX);
    m_series->attachAxis(m_axisY);

    // View для графика
    m_chartView = new QChartView(chart, this);
    m_chartView->setRenderHint(QPainter::Antialiasing);
    root->addWidget(m_chartView, 2);

    // Статусная строка (что сейчас делает клиент)
    m_statusLabel = new QLabel("Status: ready", this);
    root->addWidget(m_statusLabel);

    // Центральный виджет окна
    setCentralWidget(cw);

    // Сигналы: кнопки запускают HTTP запросы
    connect(m_btnCurrent, &QPushButton::clicked, this, &MainWindow::fetchCurrent);
    connect(m_btnStats, &QPushButton::clicked, this, &MainWindow::fetchStats);

    // Таймер автоподтягивания current каждые 2 секунды для наглядности
    auto *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &MainWindow::fetchCurrent);
    timer->start(2000);
}

// Возврат base url без пробелов и без завершающего /
// Это нужно, чтобы при склейке base + "/api/current" не получилось "//api/current"
QString MainWindow::baseUrl() const {
    QString u = m_baseUrlEdit->text().trimmed();
    if (u.endsWith("/")) u.chop(1);
    return u;
}

// Сборка URL base + path + query параметры
// Пример: base + "/api/stats" + {"from": "...", "to": "..."}
QUrl MainWindow::makeUrl(const QString& path, const QMap<QString, QString>& query) const {
    QUrl url(baseUrl() + path);
    if (!query.isEmpty()) {
        QUrlQuery q;
        for (auto it = query.begin(); it != query.end(); ++it) q.addQueryItem(it.key(), it.value());
        url.setQuery(q);
    }
    return url;
}

// Обновление строки статуса
void MainWindow::setStatus(const QString& s) {
    m_statusLabel->setText("Status: " + s);
}

// Показ текущего значения
void MainWindow::showCurrent(double temp, const QString& ts) {
    m_currentLabel->setText(QString("Current: %1 °C  @ %2").arg(temp, 0, 'f', 3).arg(ts));
}

// Показ агрегированной статистики
void MainWindow::showStats(double avg, int count, double minv, double maxv) {
    m_statsLabel->setText(QString("Stats: avg=%1  count=%2  min=%3  max=%4")
                          .arg(avg, 0, 'f', 3).arg(count).arg(minv, 0, 'f', 3).arg(maxv, 0, 'f', 3));
}

// Парсер JSON ответа /api/current
// Ожидается объект с полями temp и ts
// Пример: {"temp": 23.5, "ts":"2026-02-17T12:00:00Z"}
bool MainWindow::parseCurrentJson(const QByteArray& body, double& temp, QString& ts, QString& err) {
    auto doc = QJsonDocument::fromJson(body);
    if (!doc.isObject()) { err = "current: json is not object"; return false; }
    auto o = doc.object();
    if (!o.contains("temp") || !o.contains("ts")) { err = "current: missing temp/ts"; return false; }
    temp = o.value("temp").toDouble();
    ts = o.value("ts").toString();
    return true;
}

// Среднее по серии, если сервер не прислал агрегаты
double MainWindow::computeAvg(const QVector<QPair<QDateTime,double>>& series) const {
    if (series.isEmpty()) return 0.0;
    double s = 0.0;
    for (auto& p : series) s += p.second;
    return s / double(series.size());
}

// Парсер JSON ответа /api/stats (расширенный)
// Поддерживаем точки как:
// 1) объект: {"ts": "...", "temp": 12.3} (или {"time":...,"value":...})
// 2) массив: ["2026-..Z", 12.3]
// 3) массив epoch: [1700000000, 12.3] или [1700000000000, 12.3] (сек/мс)
bool MainWindow::parseStatsJson(const QByteArray& body, double& avg, int& count, double& minv, double& maxv,
                                QVector<QPair<QDateTime,double>>& series, QString& err) {
    auto doc = QJsonDocument::fromJson(body);
    if (!doc.isObject()) { err = "stats: json is not object"; return false; }
    auto o = doc.object();

    // сервер мог вернуть {"error":"..."}
    if (o.contains("error")) {
        err = "stats: " + o.value("error").toString("server error");
        return false;
    }

    // агрегаты (если есть)
    bool hasAgg = o.contains("avg") && o.contains("count");
    if (hasAgg) {
        avg = o.value("avg").toDouble(std::numeric_limits<double>::quiet_NaN());
        count = o.value("count").toInt(0);
        minv = o.contains("min") ? o.value("min").toDouble(avg) : avg;
        maxv = o.contains("max") ? o.value("max").toDouble(avg) : avg;
    } else {
        avg = std::numeric_limits<double>::quiet_NaN();
        count = 0;
        minv = std::numeric_limits<double>::quiet_NaN();
        maxv = std::numeric_limits<double>::quiet_NaN();
    }

    // пробуем разные имена массива точек
    QJsonArray arr;
    auto pickArr = [&](const char* key) -> bool {
        if (o.contains(key) && o.value(key).isArray()) { arr = o.value(key).toArray(); return true; }
        return false;
    };

    if (!pickArr("measurements") &&
        !pickArr("series") &&
        !pickArr("samples") &&
        !pickArr("points") &&
        !pickArr("data") &&
        !pickArr("items") &&
        !pickArr("rows") &&
        !pickArr("values")) {
        arr = QJsonArray();
    }

    // если массива нет, но агрегаты есть - это норм (просто график будет пустой)
    if (arr.isEmpty()) {
        if (hasAgg) return true;
        err = "stats: no points array and no aggregates";
        return false;
    }

    series.clear();
    series.reserve(arr.size());

    auto parseTs = [&](const QJsonValue& v, QDateTime& out) -> bool {
        if (v.isString()) {
            QDateTime dt = parseIsoUtc(v.toString());
            if (!dt.isValid()) return false;
            out = dt;
            return true;
        }
        if (v.isDouble()) {
            double dv = v.toDouble();
            qint64 t = (qint64)std::llround(dv);
            if (t > 200000000000LL) out = QDateTime::fromMSecsSinceEpoch(t, Qt::UTC);
            else out = QDateTime::fromSecsSinceEpoch(t, Qt::UTC);
            return out.isValid();
        }
        return false;
    };

    auto parseTemp = [&](const QJsonValue& v, double& out) -> bool {
        if (!v.isDouble()) return false;
        double t = v.toDouble(std::numeric_limits<double>::quiet_NaN());
        if (!isFinite(t)) return false;
        out = t;
        return true;
    };

    for (const auto& v : arr) {
        // формат 1: объект
        if (v.isObject()) {
            auto obj = v.toObject();

            QJsonValue tsVal;
            if (obj.contains("ts")) tsVal = obj.value("ts");
            else if (obj.contains("time")) tsVal = obj.value("time");
            else if (obj.contains("t")) tsVal = obj.value("t");
            else continue;

            QJsonValue tempVal;
            if (obj.contains("temp")) tempVal = obj.value("temp");
            else if (obj.contains("value")) tempVal = obj.value("value");
            else if (obj.contains("v")) tempVal = obj.value("v");
            else continue;

            QDateTime dt;
            double tv;
            if (!parseTs(tsVal, dt)) continue;
            if (!parseTemp(tempVal, tv)) continue;

            series.push_back({dt, tv});
            continue;
        }

        // формат 2: массив [ts, temp]
        if (v.isArray()) {
            QJsonArray a = v.toArray();
            if (a.size() < 2) continue;

            QDateTime dt;
            double tv;
            if (!parseTs(a.at(0), dt)) continue;
            if (!parseTemp(a.at(1), tv)) continue;

            series.push_back({dt, tv});
            continue;
        }
    }

    if (series.isEmpty()) {
        if (hasAgg) return true;
        err = "stats: points array parsed to empty";
        return false;
    }

    // сортировка по времени
    std::sort(series.begin(), series.end(), [](auto& a, auto& b){ return a.first < b.first; });

    // если агрегатов не было, считаем по точкам
    if (!hasAgg) {
        avg = computeAvg(series);
        count = series.size();
        minv = series[0].second;
        maxv = series[0].second;
        for (auto& p : series) { minv = std::min(minv, p.second); maxv = std::max(maxv, p.second); }
    }

    // если агрегаты были, но NaN - тоже пересчитаем для корректности UI
    if (!isFinite(avg) || !isFinite(minv) || !isFinite(maxv) || count <= 0) {
        avg = computeAvg(series);
        count = series.size();
        minv = series[0].second;
        maxv = series[0].second;
        for (auto& p : series) { minv = std::min(minv, p.second); maxv = std::max(maxv, p.second); }
    }

    return true;
}

// Перерисовка графика по серии
// X: unix seconds, Y: temp
void MainWindow::updateChart(const QVector<QPair<QDateTime,double>>& series) {
    m_series->clear();
    if (series.isEmpty()) return;

    double xmin = series.first().first.toSecsSinceEpoch();
    double xmax = series.last().first.toSecsSinceEpoch();

    double ymin = series.first().second;
    double ymax = series.first().second;

    // Заполняем линию и одновременно считаем min/max
    for (auto& p : series) {
        double x = p.first.toSecsSinceEpoch();
        double y = p.second;
        m_series->append(x, y);
        ymin = std::min(ymin, y);
        ymax = std::max(ymax, y);
    }

    // Защита от нулевого диапазона, чтобы оси не схлопнулись
    if (xmin == xmax) xmax = xmin + 1;
    if (ymin == ymax) { ymin -= 1; ymax += 1; }

    m_axisX->setRange(xmin, xmax);
    m_axisY->setRange(ymin, ymax);
}

// GET /api/current
void MainWindow::fetchCurrent() {
    setStatus("GET /api/current ...");
    auto url = makeUrl("/api/current");                 // base + path
    auto *reply = m_net.get(QNetworkRequest(url));      // отправка GET

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        QByteArray body = reply->readAll();             // тело ответа

        // Ошибка сети/HTTP уровня Qt
        if (reply->error() != QNetworkReply::NoError) {
            setStatus("current error: " + reply->errorString());
            reply->deleteLater();
            return;
        }

        // Парсим JSON
        double temp = 0.0; QString ts; QString err;
        if (!parseCurrentJson(body, temp, ts, err)) {
            setStatus(err);
            reply->deleteLater();
            return;
        }

        // Обновляем UI
        showCurrent(temp, ts);
        setStatus("ok (current)");
        reply->deleteLater(); // удалить reply безопасно через event loop
    });
}

// GET /api/stats?from=...&to=...
void MainWindow::fetchStats() {
    QDateTime from = m_fromEdit->dateTime().toUTC();
    QDateTime to   = m_toEdit->dateTime().toUTC();
    if (!from.isValid() || !to.isValid() || from >= to) {
        setStatus("bad period: from>=to");
        return;
    }

    // Сервер ожидает ISO UTC с Z в конце
    // Пример: 2026-02-17T12:00:00Z
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

        // Данные статистики + серия точек
        double avg=0, minv=0, maxv=0;
        int count=0;
        QVector<QPair<QDateTime,double>> series;
        QString err;

        if (!parseStatsJson(body, avg, count, minv, maxv, series, err)) {
            setStatus(err);
            reply->deleteLater();
            return;
        }

        // Обновляем агрегаты
        showStats(avg, count, minv, maxv);

        // Обновляем таблицу (показываем максимум 50 последних точек)
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

            // Перерисовываем график
            updateChart(series);
        } else {
            m_series->clear(); // данных нет, график очищаем
        }

        setStatus("ok (stats)");
        reply->deleteLater();
    });
}
