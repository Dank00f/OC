#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

sudo apt-get update -y
sudo apt-get install -y cmake ninja-build g++ pkg-config qt6-base-dev qt6-base-dev-tools

mkdir -p src

cat > CMakeLists.txt <<'CMAKE'
cmake_minimum_required(VERSION 3.20)
project(lab6_temp_gui LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Network)

qt_standard_project_setup()

add_executable(temp_gui
  src/main.cpp
)

target_link_libraries(temp_gui PRIVATE Qt6::Widgets Qt6::Network)
CMAKE

cat > README.md <<'MD'
# Lab6 GUI client

GUI-клиент к серверу (lab5). Требует на сервере:
- GET /api/current  -> {"ts":"...Z","temp":N}
- GET /api/stats?from=...Z&to=...Z -> {"from":"...Z","to":"...Z","count":N,"avg":X,"min":Y,"max":Z}

## Build (Kali)
./setup_lab6_all.sh
./build/temp_gui
MD

cat > src/main.cpp <<'CPP'
#include <QtWidgets/QApplication>
#include <QtWidgets/QWidget>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QDateTimeEdit>
#include <QtWidgets/QMessageBox>
#include <QtCore/QTimer>
#include <QtCore/QUrlQuery>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

static QString isoUtc(const QDateTime& dt){
    return dt.toUTC().toString("yyyy-MM-ddTHH:mm:ssZ");
}

static bool parseCurrentJson(const QByteArray& body, QString& ts, double& temp){
    QJsonParseError err{};
    auto doc = QJsonDocument::fromJson(body, &err);
    if(err.error != QJsonParseError::NoError || !doc.isObject()) return false;
    auto o = doc.object();
    if(!o.contains("ts") || !o.contains("temp")) return false;
    ts = o.value("ts").toString();
    temp = o.value("temp").toDouble();
    return !ts.isEmpty();
}

static bool parseStatsJson(const QByteArray& body, QJsonObject& out){
    QJsonParseError err{};
    auto doc = QJsonDocument::fromJson(body, &err);
    if(err.error != QJsonParseError::NoError || !doc.isObject()) return false;
    out = doc.object();
    return true;
}

int main(int argc, char** argv){
    QApplication app(argc, argv);

    QWidget w;
    w.setWindowTitle("Temp GUI (Lab6)");
    w.resize(720, 420);

    auto* grid = new QGridLayout(&w);

    auto* lblBase = new QLabel("Base URL:");
    auto* baseUrl = new QLineEdit("http://127.0.0.1:8080");
    grid->addWidget(lblBase, 0, 0);
    grid->addWidget(baseUrl, 0, 1, 1, 3);

    auto* lblCur = new QLabel("Current:");
    auto* curVal = new QLabel("-");
    auto* curTs  = new QLabel("-");
    grid->addWidget(lblCur, 1, 0);
    grid->addWidget(curVal, 1, 1);
    grid->addWidget(curTs, 1, 2, 1, 2);

    auto* fromEdit = new QDateTimeEdit(QDateTime::currentDateTimeUtc().addSecs(-3600));
    auto* toEdit   = new QDateTimeEdit(QDateTime::currentDateTimeUtc());
    fromEdit->setDisplayFormat("yyyy-MM-dd HH:mm:ss 'UTC'");
    toEdit->setDisplayFormat("yyyy-MM-dd HH:mm:ss 'UTC'");
    fromEdit->setTimeSpec(Qt::UTC);
    toEdit->setTimeSpec(Qt::UTC);

    auto* btnStats = new QPushButton("Fetch stats");
    auto* btnNow   = new QPushButton("Set last hour");

    auto* row = new QHBoxLayout();
    row->addWidget(new QLabel("From:"));
    row->addWidget(fromEdit);
    row->addWidget(new QLabel("To:"));
    row->addWidget(toEdit);
    row->addWidget(btnNow);
    row->addWidget(btnStats);

    grid->addLayout(row, 2, 0, 1, 4);

    auto* table = new QTableWidget(1, 6);
    table->setHorizontalHeaderLabels({"from","to","count","avg","min","max"});
    table->verticalHeader()->setVisible(false);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionMode(QAbstractItemView::NoSelection);
    grid->addWidget(table, 3, 0, 1, 4);

    QNetworkAccessManager net;

    auto makeUrl = [&](const QString& path)->QUrl{
        QUrl u(baseUrl->text().trimmed());
        if(!u.isValid()) return QUrl();
        QString p = u.path();
        if(p.endsWith('/')) p.chop(1);
        u.setPath(p + path);
        return u;
    };

    auto fetchCurrent = [&](){
        QUrl u = makeUrl("/api/current");
        if(!u.isValid()){
            curVal->setText("-");
            curTs->setText("bad base url");
            return;
        }
        auto* rep = net.get(QNetworkRequest(u));
        QObject::connect(rep, &QNetworkReply::finished, [&](){
            QByteArray body = rep->readAll();
            int code = rep->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            rep->deleteLater();
            if(code != 200){
                curVal->setText("-");
                curTs->setText(QString("HTTP %1").arg(code));
                return;
            }
            QString ts; double t=0;
            if(!parseCurrentJson(body, ts, t)){
                curVal->setText("-");
                curTs->setText("bad json");
                return;
            }
            curVal->setText(QString::number(t, 'f', 3));
            curTs->setText(ts);
        });
    };

    auto fetchStats = [&](){
        QDateTime f = fromEdit->dateTime().toUTC();
        QDateTime t = toEdit->dateTime().toUTC();
        if(!f.isValid() || !t.isValid() || f >= t){
            QMessageBox::warning(&w, "Bad period", "from must be < to");
            return;
        }
        QUrl u = makeUrl("/api/stats");
        if(!u.isValid()){
            QMessageBox::warning(&w, "Bad base URL", "invalid base url");
            return;
        }
        QUrlQuery q;
        q.addQueryItem("from", isoUtc(f));
        q.addQueryItem("to", isoUtc(t));
        u.setQuery(q);

        auto* rep = net.get(QNetworkRequest(u));
        QObject::connect(rep, &QNetworkReply::finished, [&](){
            QByteArray body = rep->readAll();
            int code = rep->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            rep->deleteLater();
            if(code != 200){
                QMessageBox::critical(&w, "Stats error", QString("HTTP %1 (most likely /api/stats not implemented)").arg(code));
                return;
            }
            QJsonObject o;
            if(!parseStatsJson(body, o)){
                QMessageBox::critical(&w, "Stats error", "bad json");
                return;
            }

            auto setCell = [&](int col, const QString& v){
                auto* it = new QTableWidgetItem(v);
                table->setItem(0, col, it);
            };

            setCell(0, o.value("from").toString());
            setCell(1, o.value("to").toString());
            setCell(2, QString::number(o.value("count").toInt()));
            setCell(3, QString::number(o.value("avg").toDouble(), 'f', 3));
            setCell(4, QString::number(o.value("min").toDouble(), 'f', 3));
            setCell(5, QString::number(o.value("max").toDouble(), 'f', 3));
        });
    };

    QObject::connect(btnStats, &QPushButton::clicked, fetchStats);
    QObject::connect(btnNow, &QPushButton::clicked, [&](){
        toEdit->setDateTime(QDateTime::currentDateTimeUtc());
        fromEdit->setDateTime(QDateTime::currentDateTimeUtc().addSecs(-3600));
    });

    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, fetchCurrent);
    timer.start(1000);
    fetchCurrent();

    w.show();
    return app.exec();
}
CPP

cat > smoke_http_kali.sh <<'SH2'
#!/usr/bin/env bash
set -euo pipefail
BASE="${1:-http://127.0.0.1:8080}"

echo "== current =="
curl -sS "$BASE/api/current"; echo

FROM=$(python3 - <<'PY'
from datetime import datetime, timedelta, timezone
print((datetime.now(timezone.utc)-timedelta(hours=1)).strftime("%Y-%m-%dT%H:%M:%SZ"))
PY
)
TO=$(python3 - <<'PY'
from datetime import datetime, timezone
print(datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"))
PY
)

echo "== stats =="
curl -v "$BASE/api/stats?from=$FROM&to=$TO"
SH2
chmod +x smoke_http_kali.sh

rm -rf build
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
echo "OK: build/temp_gui should exist now"
