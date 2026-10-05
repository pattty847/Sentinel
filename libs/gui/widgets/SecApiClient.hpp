#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include "ResearchProcess.hpp"
#include <QDateTime>
#include <optional>

/**
 * Direct Python SEC API client that runs helper scripts via subprocess.
 * Replaces the microservice approach with direct subprocess calls to scripts/sec_fetch_*.py.
 */
class SecApiClient : public QObject {
    Q_OBJECT

public:
    explicit SecApiClient(QObject* parent = nullptr, ResearchProcess* runner = nullptr);
    ~SecApiClient();

    struct Filing {
        QString date;
        QString formType;
        QString description;
        QString url;
    };

    struct Transaction {
        QString date;
        QString insiderName;
        QString transactionType;
        std::optional<double> shares;
        std::optional<double> price;
    };

    struct FinancialMetric {
        QString name;
        QString value;
        QString unit;
        QString cadence, period, date;
    };

    bool isReady() const { return true; }
    void cancel();
    QString requestedTicker() const { return m_ticker; }
    QDateTime retrievedAt() const { return m_retrievedAt; }
    quint64 requestId() const { return m_request; }

public slots:
    void fetchFilings(const QString& ticker, const QString& formType = QString());
    void fetchInsiderTransactions(const QString& ticker);
    void fetchInsiderSignals(const QString& ticker, int daysBack = 180);
    void fetchFinancialSummary(const QString& ticker);

signals:
    void filingsReady(const QList<Filing>& filings);
    void transactionsReady(const QList<Transaction>& transactions);
    void insiderSignalsReady(const QJsonObject& payload);
    void financialsReady(const QList<FinancialMetric>& metrics);
    void apiError(const QString& error);
    void statusUpdate(const QString& message);

private:
    void runSecScript(const QString& scriptName, const QStringList& args, const QString& operation);
    QString getScriptsPath() const;
    void parseFilingsData(const QString& jsonStr);
    void parseTransactionsData(const QString& jsonStr);
    void parseInsiderSignalsData(const QString& jsonStr);
    void parseFinancialsData(const QString& jsonStr);

    void acceptResult(quint64 request, const QByteArray& output, const QString& error);
    QPointer<ResearchProcess> m_runner;
    quint64 m_request = 0;
    bool m_pending = false;
    QString m_currentOperation, m_ticker;
    QDateTime m_retrievedAt;
};
