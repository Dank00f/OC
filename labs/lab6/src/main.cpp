#include <QtWidgets/QApplication>
#include <QtWidgets/QWidget>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QDateTimeEdit>
#include <QtWidgets/QMessageBox>

#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkRequest>
#include <QtNetwork/QNetworkReply>

#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonArray>
#include <QtCore/QUrlQuery>
#include <QtCore/QDateTime>
#include <QtCore/QTimeZone>
#include <QtCore/QVector>
#include <QtCore/QtGlobal>

#include <QtGui/QPainter>
#include <QtGui/QPainterPath>

#include <algorithm>
#include <limits>
#include <cmath>

struct StatsPoint {
    QDateTime tsUtc;
    double temp = 0.0;
};

struct Stats {
    int count = 0;
    double avg = std::numeric_limits<double>::quiet_NaN();
    double min = std::numeric_limits<double>::quiet_NaN();
    double max = std::numeric_limits<double>::quiet_NaN();
    QVector<StatsPoint> points;
    QString error; // если сервер вернул {"error": "..."}
};

static bool isFinite(double x) { return std::isfinite(x); }

static bool parseCurrentJson(const QByteArray& body, QDateTime& tsUtc, double& temp, QString& errOut) {
    auto doc = QJsonDocument::fromJson(body);
    if (!doc.isObject()) { errOut = "JSON is not object"; return false; }
    auto o = doc.object();

    if (o.contains("error")) { errOut = o.value("error").toString("server error"); return false; }

    if (!o.contains("ts") || !o.contains("temp")) { errOut = "missing ts/temp"; return false; }

    tsUtc = QDateTime::fromString(o.value("ts").toString(), Qt::ISODate);
    tsUtc.setTimeZone(QTimeZone::utc());

    temp = o.value("temp").toDouble(std::numeric_limits<double>::quiet_NaN());
    if (!tsUtc.isValid() || !isFinite(temp)) { errOut = "bad ts/temp"; return false; }

    return true;
}

static StatsPoint parsePointObj(const QJsonObject& p, bool& ok) {
    ok = false;

    // сервер может отдавать "ts" как ISO, либо как число (сек/мс)
    QDateTime tsUtc;
    if (p.value("ts").isString()) {
        tsUtc = QDateTime::fromString(p.value("ts").toString(), Qt::ISODate);
        tsUtc.setTimeZone(QTimeZone::utc());
    } else if (p.value("ts").isDouble()) {
        // если это сек или мс — определить грубо по величине
        double v = p.value("ts").toDouble();
        qint64 t = (qint64)std::llround(v);
        if (t > 200000000000LL) { // похоже на миллисекунды
            tsUtc = QDateTime::fromMSecsSinceEpoch(t, QTimeZone::utc());
        } else { // секунды
            tsUtc = QDateTime::fromSecsSinceEpoch(t, QTimeZone::utc());
        }
    } else {
        return {};
    }

    double temp = p.value("temp").toDouble(std::numeric_limits<double>::quiet_NaN());
    if (!tsUtc.isValid() || !isFinite(temp)) return {};

    ok = true;
    return {tsUtc, temp};
}

static bool parseStatsJson(const QByteArray& body, Stats& st) {
    st = Stats{};

    auto doc = QJsonDocument::fromJson(body);
    if (!doc.isObject()) { st.error = "JSON is not object"; return false; }
    auto o = doc.object();

    if (o.contains("error")) { st.error = o.value("error").toString("server error"); return false; }

    // поля статистики могут быть (count/avg/min/max), а массив точек — series ИЛИ samples
    st.count = o.value("count").toInt(0);
    st.avg = o.value("avg").toDouble(std::numeric_limits<double>::quiet_NaN());
    st.min = o.value("min").toDouble(std::numeric_limits<double>::quiet_NaN());
    st.max = o.value("max").toDouble(std::numeric_limits<double>::quiet_NaN());

    QJsonArray arr;
    if (o.contains("series") && o.value("series").isArray()) arr = o.value("series").toArray();
    else if (o.contains("samples") && o.value("samples").isArray()) arr = o.value("samples").toArray();
    else arr = QJsonArray(); // допустимо: статистика есть, точек нет

    st.points.clear();
    st.points.reserve(arr.size());

    for (const auto& v : arr) {
        if (!v.isObject()) continue;
        bool ok = false;
        auto pt = parsePointObj(v.toObject(), ok);
        if (ok) st.points.push_back(pt);
    }

    // сортировка по времени (если сервер прислал неупорядоченно)
    std::sort(st.points.begin(), st.points.end(), [](const StatsPoint& a, const StatsPoint& b){
        return a.tsUtc.toSecsSinceEpoch() < b.tsUtc.toSecsSinceEpoch();
    });

    return true;
}

class PlotWidget : public QWidget {
public:
    explicit PlotWidget(QWidget* parent=nullptr) : QWidget(parent) {
        setMinimumHeight(320);
        setAutoFillBackground(true);
        QPalette pal = palette();
        pal.setColor(QPalette::Window, Qt::white);
        setPalette(pal);
    }

    void setSeries(QVector<StatsPoint> pts) {
        points = std::move(pts);
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        QRectF r = rect().adjusted(45, 12, -12, -30);
        p.drawRect(r);

        if (points.size() < 2) {
            p.drawText(QPointF(10, 20), "No data");
            return;
        }

        qint64 tMin = points.first().tsUtc.toSecsSinceEpoch();
        qint64 tMax = points.last().tsUtc.toSecsSinceEpoch();

        double yMin = points.first().temp;
        double yMax = points.first().temp;
        for (const auto& pt : points) {
            yMin = std::min(yMin, pt.temp);
            yMax = std::max(yMax, pt.temp);
        }

        if (tMax == tMin) tMax = tMin + 1;
        if (yMax == yMin) { yMax = yMin + 1.0; yMin -= 1.0; }

        auto mapX = [&](qint64 t){
            double k = double(t - tMin) / double(tMax - tMin);
            return r.left() + k * r.width();
        };
        auto mapY = [&](double y){
            double k = (y - yMin) / (yMax - yMin);
            return r.bottom() - k * r.height();
        };

        // разрывы по времени: если между точками слишком большой гэп — НЕ соединяем
        // Подстрой если хочешь: 5 сек обычно хватает.
        const qint64 GAP_SEC = 5;

        QPainterPath path;
        auto t0 = points[0].tsUtc.toSecsSinceEpoch();
        path.moveTo(mapX(t0), mapY(points[0].temp));

        for (int i = 1; i < points.size(); ++i) {
            qint64 tPrev = points[i-1].tsUtc.toSecsSinceEpoch();
            qint64 tCur  = points[i].tsUtc.toSecsSinceEpoch();

            double x = mapX(tCur);
            double y = mapY(points[i].temp);

            if (tCur - tPrev > GAP_SEC) path.moveTo(x, y);
            else path.lineTo(x, y);
        }

        p.drawPath(path);

        // подписи
        p.drawText(QPointF(10, 20), QString("max=%1").arg(yMax, 0, 'f', 3));
        p.drawText(QPointF(10, 40), QString("min=%1").arg(yMin, 0, 'f', 3));
    }

private:
    QVector<StatsPoint> points;
};

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    QString baseUrl = "http://127.0.0.1:8080";
    for (int i=1;i<argc;i++) {
        QString a = argv[i];
        if (a == "--base-url" && i+1<argc) baseUrl = argv[++i];
    }

    QNetworkAccessManager nam;

    QWidget w;
    w.setWindowTitle("Lab6 Temp GUI");
    w.resize(1000, 600);

    auto* root = new QVBoxLayout(&w);

    // top row
    auto* top = new QHBoxLayout();
    auto* baseEdit = new QLineEdit(baseUrl);
    auto* refreshBtn = new QPushButton("Refresh current");
    top->addWidget(new QLabel("Base URL:"));
    top->addWidget(baseEdit, 1);
    top->addWidget(refreshBtn);
    root->addLayout(top);

    auto* curLbl = new QLabel("current: -");
    root->addWidget(curLbl);

    // date range row
    auto* mid = new QHBoxLayout();
    auto* fromEdit = new QDateTimeEdit();
    auto* toEdit   = new QDateTimeEdit();

    fromEdit->setTimeZone(QTimeZone::utc());
    toEdit->setTimeZone(QTimeZone::utc());

    // важное: пусть Qt сам хранит UTC, а формат — просто отображение
    fromEdit->setDisplayFormat("yyyy-MM-dd HH:mm:ss");
    toEdit->setDisplayFormat("yyyy-MM-dd HH:mm:ss");

    toEdit->setDateTime(QDateTime::currentDateTimeUtc());
    fromEdit->setDateTime(QDateTime::currentDateTimeUtc().addSecs(-3600));

    auto* loadBtn = new QPushButton("Load stats");
    mid->addWidget(new QLabel("From (UTC):"));
    mid->addWidget(fromEdit);
    mid->addSpacing(10);
    mid->addWidget(new QLabel("To (UTC):"));
    mid->addWidget(toEdit);
    mid->addSpacing(10);
    mid->addWidget(loadBtn);
    root->addLayout(mid);

    auto* stLbl = new QLabel("stats: -");
    root->addWidget(stLbl);

    auto* plot = new PlotWidget();
    root->addWidget(plot, 1);

    auto refreshCurrent = [&]() {
        QUrl url(baseEdit->text().trimmed() + "/api/current");
        QNetworkRequest req(url);

        auto* reply = nam.get(req);
        QObject::connect(reply, &QNetworkReply::finished, [&]() {
            QByteArray body = reply->readAll();
            auto err = reply->error();
            QString errStr = reply->errorString();
            reply->deleteLater();

            if (err != QNetworkReply::NoError) {
                curLbl->setText("current: HTTP error: " + errStr);
                return;
            }

            QDateTime ts; double temp=0.0; QString perr;
            if (!parseCurrentJson(body, ts, temp, perr)) {
                curLbl->setText("current: parse/server error: " + perr);
                return;
            }

            curLbl->setText(QString("current: %1  @ %2")
                            .arg(temp, 0, 'f', 3)
                            .arg(ts.toString(Qt::ISODate)));
        });
    };

    auto loadStats = [&]() {
        auto fromUtc = fromEdit->dateTime().toUTC();
        auto toUtc   = toEdit->dateTime().toUTC();

        if (!fromUtc.isValid() || !toUtc.isValid() || fromUtc >= toUtc) {
            QMessageBox::warning(&w, "Bad range", "From must be < To (UTC).");
            return;
        }

        // Сервер у тебя, судя по ошибкам, НЕ принимает ISO строки, а принимает число (скорее всего ms)
        // Поэтому шлём миллисекунды epoch — так будет стабильно.
        qint64 fromMs = fromUtc.toMSecsSinceEpoch();
        qint64 toMs   = toUtc.toMSecsSinceEpoch();

        QUrl url(baseEdit->text().trimmed() + "/api/stats");
        QUrlQuery q;
        q.addQueryItem("from", QString::number(fromMs));
        q.addQueryItem("to",   QString::number(toMs));
        url.setQuery(q);

        QNetworkRequest req(url);
        auto* reply = nam.get(req);

        QObject::connect(reply, &QNetworkReply::finished, [&]() {
            QByteArray body = reply->readAll();
            auto err = reply->error();
            QString errStr = reply->errorString();
            reply->deleteLater();

            if (err != QNetworkReply::NoError) {
                stLbl->setText("stats: HTTP error: " + errStr);
                plot->setSeries({});
                return;
            }

            Stats st;
            if (!parseStatsJson(body, st)) {
                stLbl->setText("stats: parse/server error: " + (st.error.isEmpty() ? "unknown" : st.error));
                plot->setSeries({});
                return;
            }

            stLbl->setText(QString("stats: count=%1 avg=%2 min=%3 max=%4 points=%5")
                           .arg(st.count)
                           .arg(st.avg, 0, 'f', 3)
                           .arg(st.min, 0, 'f', 3)
                           .arg(st.max, 0, 'f', 3)
                           .arg(st.points.size()));

            plot->setSeries(st.points);
        });
    };

    QObject::connect(refreshBtn, &QPushButton::clicked, refreshCurrent);
    QObject::connect(loadBtn, &QPushButton::clicked, loadStats);

    w.show();
    refreshCurrent();
    return app.exec();
}
