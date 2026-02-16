#include <QtWidgets/QApplication>      // запуск Qt приложения и цикла событий
#include <QtWidgets/QWidget>           // базовый виджет (окно)
#include <QtWidgets/QHBoxLayout>       // горизонтальная раскладка
#include <QtWidgets/QVBoxLayout>       // вертикальная раскладка
#include <QtWidgets/QLineEdit>         // поле ввода текста (Base URL)
#include <QtWidgets/QLabel>            // надписи
#include <QtWidgets/QPushButton>       // кнопки
#include <QtWidgets/QDateTimeEdit>     // выбор даты-времени
#include <QtWidgets/QMessageBox>       // всплывающее предупреждение

#include <QtNetwork/QNetworkAccessManager> // делает HTTP запросы
#include <QtNetwork/QNetworkRequest>       // объект запроса (URL, заголовки)
#include <QtNetwork/QNetworkReply>         // объект ответа (тело, ошибки)

#include <QtCore/QJsonDocument>        // разбор JSON из байтов
#include <QtCore/QJsonObject>          // JSON объект { ... }
#include <QtCore/QJsonArray>           // JSON массив [ ... ]
#include <QtCore/QUrlQuery>            // сборка query string ?from=...&to=...
#include <QtCore/QDateTime>            // дата-время
#include <QtCore/QTimeZone>            // часовой пояс (UTC)
#include <QtCore/QVector>              // контейнер Qt как vector
#include <QtCore/QtGlobal>             // типы и утилиты Qt

#include <QtGui/QPainter>              // рисование в виджете
#include <QtGui/QPainterPath>          // рисование линии по точкам

#include <algorithm>                   // sort/min/max
#include <limits>                      // NaN
#include <cmath>                       // isfinite/llround

struct StatsPoint {
    QDateTime tsUtc;                   // время точки в UTC
    double temp = 0.0;                 // температура точки
};

struct Stats {
    int count = 0;                     // сколько измерений
    double avg = std::numeric_limits<double>::quiet_NaN();
    double min = std::numeric_limits<double>::quiet_NaN();
    double max = std::numeric_limits<double>::quiet_NaN();
    QVector<StatsPoint> points;        // точки для графика
    QString error;                     // текст ошибки от сервера
};

static bool isFinite(double x) { return std::isfinite(x); } // число не NaN и не inf

// Разбор ответа /api/current
// Ожидаем JSON: {"ts":"...ISO...Z","temp": число} или {"error":"..."}
static bool parseCurrentJson(const QByteArray& body, QDateTime& tsUtc, double& temp, QString& errOut) {
    auto doc = QJsonDocument::fromJson(body);                         // bytes -> JSON
    if (!doc.isObject()) { errOut = "JSON is not object"; return false; }
    auto o = doc.object();

    if (o.contains("error")) { errOut = o.value("error").toString("server error"); return false; }
    if (!o.contains("ts") || !o.contains("temp")) { errOut = "missing ts/temp"; return false; }

    tsUtc = QDateTime::fromString(o.value("ts").toString(), Qt::ISODate); // строка ISO -> время
    tsUtc.setTimeZone(QTimeZone::utc());                                  // фиксируем UTC

    temp = o.value("temp").toDouble(std::numeric_limits<double>::quiet_NaN()); // temp -> double
    if (!tsUtc.isValid() || !isFinite(temp)) { errOut = "bad ts/temp"; return false; }

    return true;
}

// Разбор одной точки, если она пришла JSON объектом
// Поддерживаем разные имена полей, чтобы GUI работал и с lab5, и с lab6 сервером
static StatsPoint parsePointObj(const QJsonObject& p, bool& ok) {
    ok = false;

    QDateTime tsUtc;
    auto tsVal = p.contains("ts") ? p.value("ts") : (p.contains("time") ? p.value("time") : p.value("t"));

    if (tsVal.isString()) { // время как ISO строка
        tsUtc = QDateTime::fromString(tsVal.toString(), Qt::ISODate);
        tsUtc.setTimeZone(QTimeZone::utc());
    } else if (tsVal.isDouble()) { // время как epoch (сек или мс)
        double v = tsVal.toDouble();
        qint64 t = (qint64)std::llround(v);

        if (t > 200000000000LL) { // похоже на миллисекунды
            tsUtc = QDateTime::fromMSecsSinceEpoch(t, QTimeZone::utc());
        } else { // секунды
            tsUtc = QDateTime::fromSecsSinceEpoch(t, QTimeZone::utc());
        }
    } else {
        return {}; // непонятный тип времени
    }

    auto tempVal = p.contains("temp") ? p.value("temp") : (p.contains("value") ? p.value("value") : p.value("v"));
    double temp = tempVal.toDouble(std::numeric_limits<double>::quiet_NaN());

    if (!tsUtc.isValid() || !isFinite(temp)) return {};

    ok = true;
    return {tsUtc, temp};
}

// Разбор ответа /api/stats
// Ожидаем JSON: count/avg/min/max + массив точек (series или samples и др.)
static bool parseStatsJson(const QByteArray& body, Stats& st) {
    st = Stats{}; // сброс структуры

    auto doc = QJsonDocument::fromJson(body);
    if (!doc.isObject()) { st.error = "JSON is not object"; return false; }
    auto o = doc.object();

    if (o.contains("error")) { st.error = o.value("error").toString("server error"); return false; }

    st.count = o.value("count").toInt(0);
    st.avg = o.value("avg").toDouble(std::numeric_limits<double>::quiet_NaN());
    st.min = o.value("min").toDouble(std::numeric_limits<double>::quiet_NaN());
    st.max = o.value("max").toDouble(std::numeric_limits<double>::quiet_NaN());

    QJsonArray arr;
    if (o.contains("series")  && o.value("series").isArray())  arr = o.value("series").toArray();      // lab5 формат
    else if (o.contains("samples") && o.value("samples").isArray()) arr = o.value("samples").toArray(); // lab6 формат
    else if (o.contains("points")  && o.value("points").isArray())  arr = o.value("points").toArray();
    else if (o.contains("data")    && o.value("data").isArray())    arr = o.value("data").toArray();
    else if (o.contains("items")   && o.value("items").isArray())   arr = o.value("items").toArray();
    else if (o.contains("rows")    && o.value("rows").isArray())    arr = o.value("rows").toArray();
    else if (o.contains("values")  && o.value("values").isArray())  arr = o.value("values").toArray();
    else arr = QJsonArray();

    st.points.clear();
    st.points.reserve(arr.size());

    for (const auto& v : arr) {
        bool ok = false;

        if (v.isObject()) { // формат 1: {"ts":..., "temp":...}
            auto pt = parsePointObj(v.toObject(), ok);
            if (ok) st.points.push_back(pt);
            continue;
        }

        if (v.isArray()) { // формат 2: ["ts", temp] или [epoch, temp]
            QJsonArray a = v.toArray();
            if (a.size() >= 2) {
                QDateTime tsUtc;
                const auto tsVal = a.at(0);

                if (tsVal.isString()) {
                    tsUtc = QDateTime::fromString(tsVal.toString(), Qt::ISODate);
                    tsUtc.setTimeZone(QTimeZone::utc());
                } else if (tsVal.isDouble()) {
                    double vv = tsVal.toDouble();
                    qint64 t = (qint64)std::llround(vv);
                    if (t > 200000000000LL) tsUtc = QDateTime::fromMSecsSinceEpoch(t, QTimeZone::utc());
                    else tsUtc = QDateTime::fromSecsSinceEpoch(t, QTimeZone::utc());
                }

                double temp = a.at(1).toDouble(std::numeric_limits<double>::quiet_NaN());
                if (tsUtc.isValid() && std::isfinite(temp)) {
                    ok = true;
                    st.points.push_back({tsUtc, temp});
                }
            }
            continue;
        }
    }

    // На всякий случай упорядочим точки по времени, чтобы график рисовался правильно
    std::sort(st.points.begin(), st.points.end(), [](const StatsPoint& a, const StatsPoint& b){
        return a.tsUtc.toSecsSinceEpoch() < b.tsUtc.toSecsSinceEpoch();
    });

    return true;
}

// Виджет для графика: рисуем линию и точки вручную через QPainter
class PlotWidget : public QWidget {
public:
    explicit PlotWidget(QWidget* parent=nullptr) : QWidget(parent) {
        setMinimumHeight(320);               // чтобы график был виден
        setAutoFillBackground(true);
        QPalette pal = palette();
        pal.setColor(QPalette::Window, Qt::white); // белый фон
        setPalette(pal);
    }

    void setSeries(QVector<StatsPoint> pts) {
        points = std::move(pts); // забираем точки без копии
        update();                // просим Qt перерисовать виджет
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true); // сглаживание

        p.setPen(QPen(Qt::black, 2));    // рисуем черным
        p.setBrush(Qt::NoBrush);

        QRectF r = rect().adjusted(45, 12, -12, -30); // область графика с отступами
        p.drawRect(r);

        p.drawText(QPointF(10, 20), QString("pts=%1").arg(points.size())); // дебаг: сколько точек

        if (points.size() < 2) { // не из чего рисовать линию
            p.drawText(QPointF(10, 40), "No data");
            return;
        }

        qint64 tMin = points.first().tsUtc.toSecsSinceEpoch();
        qint64 tMax = points.last().tsUtc.toSecsSinceEpoch();

        double yMin = points.first().temp;
        double yMax = points.first().temp;
        for (const auto& pt : points) {         // ищем min/max по температуре
            yMin = std::min(yMin, pt.temp);
            yMax = std::max(yMax, pt.temp);
        }

        if (tMax == tMin) tMax = tMin + 1;      // защита от деления на ноль
        if (yMax == yMin) { yMax = yMin + 1.0; yMin -= 1.0; }

        // Перевод "время -> x пиксель" (нормирование 0..1)
        auto mapX = [&](qint64 t){
            double k = double(t - tMin) / double(tMax - tMin);
            return r.left() + k * r.width();
        };
        // Перевод "температура -> y пиксель" (ось y в GUI направлена вниз)
        auto mapY = [&](double y){
            double k = (y - yMin) / (yMax - yMin);
            return r.bottom() - k * r.height();
        };

        QPainterPath path; // линия по точкам
        path.moveTo(mapX(points[0].tsUtc.toSecsSinceEpoch()), mapY(points[0].temp));
        for (int i = 1; i < points.size(); ++i) {
            qint64 tCur = points[i].tsUtc.toSecsSinceEpoch();
            path.lineTo(mapX(tCur), mapY(points[i].temp));
        }
        p.drawPath(path);

        // Отдельно рисуем кружки точек, чтобы было видно даже если линия тонкая
        for (int i = 0; i < points.size(); ++i) {
            double x = mapX(points[i].tsUtc.toSecsSinceEpoch());
            double y = mapY(points[i].temp);
            p.drawEllipse(QPointF(x, y), 2.5, 2.5);
        }

        // дебаг подписи диапазонов
        p.drawText(QPointF(10, 40), QString("tMin=%1 tMax=%2").arg(tMin).arg(tMax));
        p.drawText(QPointF(10, 60), QString("yMin=%1 yMax=%2").arg(yMin,0,'f',3).arg(yMax,0,'f',3));
    }

private:
    QVector<StatsPoint> points; // текущие точки графика
};

int main(int argc, char** argv) {
    QApplication app(argc, argv); // запускает Qt и цикл событий (GUI живет пока exec)

    // base URL сервера, можно переопределить аргументом --base-url
    QString baseUrl = "http://127.0.0.1:8080";
    for (int i=1;i<argc;i++) {
        QString a = argv[i];
        if (a == "--base-url" && i+1<argc) baseUrl = argv[++i];
    }

    QNetworkAccessManager nam; // объект для HTTP запросов

    QWidget w;
    w.setWindowTitle("Lab6 Temp GUI");
    w.resize(1000, 600);

    auto* root = new QVBoxLayout(&w); // главный вертикальный layout

    // Верх: base URL и кнопка current
    auto* top = new QHBoxLayout();
    auto* baseEdit = new QLineEdit(baseUrl);
    auto* refreshBtn = new QPushButton("Refresh current");
    top->addWidget(new QLabel("Base URL:"));
    top->addWidget(baseEdit, 1);  // растягиваем поле URL
    top->addWidget(refreshBtn);
    root->addLayout(top);

    auto* curLbl = new QLabel("current: -"); // сюда пишем текущую температуру
    root->addWidget(curLbl);

    // Средний ряд: выбор периода времени
    auto* mid = new QHBoxLayout();
    auto* fromEdit = new QDateTimeEdit();
    auto* toEdit   = new QDateTimeEdit();

    fromEdit->setTimeZone(QTimeZone::utc()); // всегда работаем в UTC, чтобы не было сдвигов
    toEdit->setTimeZone(QTimeZone::utc());

    fromEdit->setDisplayFormat("yyyy-MM-dd HH:mm:ss"); // формат отображения
    toEdit->setDisplayFormat("yyyy-MM-dd HH:mm:ss");

    toEdit->setDateTime(QDateTime::currentDateTimeUtc());              // конец: сейчас
    fromEdit->setDateTime(QDateTime::currentDateTimeUtc().addSecs(-3600)); // начало: час назад

    auto* loadBtn = new QPushButton("Load stats");
    mid->addWidget(new QLabel("From (UTC):"));
    mid->addWidget(fromEdit);
    mid->addSpacing(10);
    mid->addWidget(new QLabel("To (UTC):"));
    mid->addWidget(toEdit);
    mid->addSpacing(10);
    mid->addWidget(loadBtn);
    root->addLayout(mid);

    auto* stLbl = new QLabel("stats: -"); // сюда пишем count/avg/min/max
    root->addWidget(stLbl);

    auto* plot = new PlotWidget(); // виджет графика
    root->addWidget(plot, 1);

    // Функция: запрос /api/current и обновление curLbl
    auto refreshCurrent = [&]() {
        QUrl url(baseEdit->text().trimmed() + "/api/current");
        QNetworkRequest req(url);

        auto* reply = nam.get(req); // отправка GET
        QObject::connect(reply, &QNetworkReply::finished, [reply, curLbl]() {
            if (!reply) return;
            QByteArray body = reply->readAll();      // тело ответа
            auto err = reply->error();               // код ошибки Qt
            QString errStr = reply->errorString();   // текст ошибки
            reply->deleteLater();                    // корректно удалить reply позже

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

    // Функция: запрос /api/stats?from=...&to=... и обновление stLbl + plot
    auto loadStats = [&]() {
        auto fromUtc = fromEdit->dateTime().toUTC();
        auto toUtc   = toEdit->dateTime().toUTC();

        if (!fromUtc.isValid() || !toUtc.isValid() || fromUtc >= toUtc) {
            QMessageBox::warning(&w, "Bad range", "From must be < To (UTC).");
            return;
        }

        QString fromIso = fromUtc.toString(Qt::ISODate);
        QString toIso   = toUtc.toString(Qt::ISODate);

        QUrl url(baseEdit->text().trimmed() + "/api/stats");
        QUrlQuery q;
        q.addQueryItem("from", fromIso);
        q.addQueryItem("to",   toIso);
        url.setQuery(q);

        QNetworkRequest req(url);
        auto* reply = nam.get(req);

        QObject::connect(reply, &QNetworkReply::finished, [reply, stLbl, plot]() {
            if (!reply) return;
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

            plot->setSeries(st.points); // обновляем график
        });
    };

    // Привязка кнопок к функциям
    QObject::connect(refreshBtn, &QPushButton::clicked, refreshCurrent);
    QObject::connect(loadBtn, &QPushButton::clicked, loadStats);

    w.show();          // показать окно
    refreshCurrent();  // сразу запросить current при старте
    return app.exec(); // цикл событий Qt (без него окно сразу закроется)
}
