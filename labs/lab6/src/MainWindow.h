#pragma once

#include <QMainWindow>
#include <QNetworkAccessManager>
#include <QDateTime>

QT_BEGIN_NAMESPACE
class QLabel;
class QLineEdit;
class QPushButton;
class QDateTimeEdit;
class QTableWidget;
QT_END_NAMESPACE

namespace QtCharts {
class QChartView;
class QLineSeries;
class QValueAxis;
}

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);

private slots:
    void fetchCurrent();
    void fetchStats();

private:
    QString baseUrl() const;
    QUrl makeUrl(const QString& path, const QMap<QString, QString>& query = {}) const;

    void setStatus(const QString& s);
    void showCurrent(double temp, const QString& ts);
    void showStats(double avg, int count, double minv, double maxv);

    bool parseCurrentJson(const QByteArray& body, double& temp, QString& ts, QString& err);
    bool parseStatsJson(const QByteArray& body, double& avg, int& count, double& minv, double& maxv,
                        QVector<QPair<QDateTime,double>>& series, QString& err);

    double computeAvg(const QVector<QPair<QDateTime,double>>& series) const;
    void updateChart(const QVector<QPair<QDateTime,double>>& series);

private:
    QNetworkAccessManager m_net;

    QLineEdit* m_baseUrlEdit{};
    QPushButton* m_btnCurrent{};
    QLabel* m_currentLabel{};
    QLabel* m_statusLabel{};

    QDateTimeEdit* m_fromEdit{};
    QDateTimeEdit* m_toEdit{};
    QPushButton* m_btnStats{};
    QLabel* m_statsLabel{};

    QTableWidget* m_table{};

    QtCharts::QChartView* m_chartView{};
    QtCharts::QLineSeries* m_series{};
    QtCharts::QValueAxis* m_axisX{};
    QtCharts::QValueAxis* m_axisY{};
};
