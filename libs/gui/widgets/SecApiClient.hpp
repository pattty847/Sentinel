#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QTimer>

/**
 * Direct Python SEC API client that runs helper scripts via subprocess.
 * Replaces the microservice approach with direct subprocess calls to scripts/sec_fetch_*.py.
 */
class SecApiClient : public QObject {
    Q_OBJECT

public:
    explicit SecApiClient(QObject* parent = nullptr);
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
        double shares;
        double price;
    };

    struct FinancialMetric {
        QString name;
        QString value;
        QString unit;
    };

    bool isReady() const { return m_pythonReady; }

public slots:
    void fetchFilings(const QString& ticker, const QString& formType = QString());
    void fetchInsiderTransactions(const QString& ticker);
    void fetchFinancialSummary(const QString& ticker);

signals:
    void filingsReady(const QList<Filing>& filings);
    void transactionsReady(const QList<Transaction>& transactions);
    void financialsReady(const QList<FinancialMetric>& metrics);
    void apiError(const QString& error);
    void statusUpdate(const QString& message);

private slots:
    void onPythonFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onPythonError(QProcess::ProcessError error);

private:
    void initializePython();
    void executePythonCommand(const QString& command, const QString& operation);
    void runSecScript(const QString& scriptName, const QStringList& args, const QString& operation);
    QString getPythonExecutable() const;
    QString getSecModulePath() const;
    QString getScriptsPath() const;
    void parseFilingsData(const QString& jsonStr);
    void parseTransactionsData(const QString& jsonStr);
    void parseFinancialsData(const QString& jsonStr);

    QProcess* m_pythonProcess;
    QString m_currentOperation;
    bool m_pythonReady;
    QTimer* m_initTimer;
};