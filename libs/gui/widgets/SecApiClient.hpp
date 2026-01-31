#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>

/**
 * SEC API client that talks to the local server endpoints.
 */
class SecApiClient : public QObject {
    Q_OBJECT

public:
    explicit SecApiClient(QObject* parent = nullptr);
    ~SecApiClient();

    struct Filing {
        QString accessionNo;
        QString date;
        QString formType;
        QString reportDate;
        QString description;
        QString url;
        QString primaryDocument;
    };

    struct Transaction {
        QString date;
        QString insiderName;
        QString position;
        QString transactionType;
        double shares;
        double price;
        double value;
        QString formUrl;
        QString primaryDocument;
    };

    struct FinancialMetric {
        QString name;
        QString value;
        QString period;
        QString form;
        QString periodType;
    };

    bool isReady() const { return m_serverReady; }

public slots:
    void fetchFilings(const QString& ticker, const QString& formType = QString());
    void fetchInsiderTransactions(const QString& ticker);
    void fetchFinancialSummary(const QString& ticker);

signals:
    void filingsReady(const QList<Filing>& filings);
    void transactionsReady(const QList<Transaction>& transactions);
    void financialsReady(const QList<FinancialMetric>& metrics,
                         const QString& periodEnd,
                         const QString& sourceForm,
                         const QString& entityName);
    void apiError(const QString& error);
    void statusUpdate(const QString& message);

private:
    void initializeServer();
    void postJson(const QString& path, const QJsonObject& payload, const QString& operation);
    void handleReply(QNetworkReply* reply, const QString& operation);
    QString baseUrl() const;
    void parseFilingsData(const QJsonObject& obj);
    void parseTransactionsData(const QJsonObject& obj);
    void parseFinancialsData(const QJsonObject& obj);

    QNetworkAccessManager* m_network;
    bool m_serverReady;
};
