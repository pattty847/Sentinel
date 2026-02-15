#include "SecApiClient.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QDebug>
#include <QJsonParseError>

SecApiClient::SecApiClient(QObject* parent)
    : QObject(parent)
    , m_pythonProcess(nullptr)
    , m_pythonReady(true)   // uv handles venv activation — no init probe needed
{
    emit statusUpdate("SEC API ready");
}

SecApiClient::~SecApiClient() {
    if (m_pythonProcess) {
        m_pythonProcess->kill();
        m_pythonProcess->waitForFinished(3000);
    }
}


void SecApiClient::fetchFilings(const QString& ticker, const QString& formType) {
    if (!m_pythonReady) {
        emit apiError("SEC API not ready");
        return;
    }
    
    emit statusUpdate(QString("Fetching %1 filings for %2...").arg(formType.isEmpty() ? "all" : formType, ticker));

    QStringList args;
    args << ticker;
    if (!formType.isEmpty()) {
        args << formType;
    }

    runSecScript("sec/sec_fetch_filings.py", args, "filings");
}

void SecApiClient::fetchInsiderTransactions(const QString& ticker) {
    if (!m_pythonReady) {
        emit apiError("SEC API not ready");
        return;
    }
    
    emit statusUpdate(QString("Fetching insider transactions for %1...").arg(ticker));

    QStringList args;
    args << ticker;

    runSecScript("sec/sec_fetch_transactions.py", args, "transactions");
}

void SecApiClient::fetchFinancialSummary(const QString& ticker) {
    if (!m_pythonReady) {
        emit apiError("SEC API not ready");
        return;
    }
    
    emit statusUpdate(QString("Fetching financial summary for %1...").arg(ticker));

    QStringList args;
    args << ticker;

    runSecScript("sec/sec_fetch_financials.py", args, "financials");
}

void SecApiClient::runSecScript(const QString& scriptName,
                                const QStringList& args,
                                const QString& operation) {
    if (m_pythonProcess && m_pythonProcess->state() != QProcess::NotRunning) {
        m_pythonProcess->kill();
        m_pythonProcess->waitForFinished(1000);
    }
    if (m_pythonProcess) {
        m_pythonProcess->deleteLater();
    }

    m_pythonProcess = new QProcess(this);
    connect(m_pythonProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &SecApiClient::onPythonFinished);
    connect(m_pythonProcess, &QProcess::errorOccurred, this, &SecApiClient::onPythonError);

    const QString scriptsPath = getScriptsPath();
    const QString scriptPath  = QDir(scriptsPath).absoluteFilePath(scriptName);

    // uv run activates the venv from pyproject.toml automatically
    QStringList fullArgs = {"run", "python", scriptPath};
    fullArgs << args;

    m_currentOperation = operation;
    m_pythonProcess->setWorkingDirectory(scriptsPath);
    m_pythonProcess->start("uv", fullArgs);
}

void SecApiClient::onPythonFinished(int exitCode, QProcess::ExitStatus exitStatus) {
    if (exitStatus != QProcess::NormalExit || exitCode != 0) {
        QString error = QString("Python process failed (exit code %1): %2")
                       .arg(exitCode)
                       .arg(m_pythonProcess->readAllStandardError());
        emit apiError(error);
        return;
    }

    QString output = m_pythonProcess->readAllStandardOutput();
    
    // Parse data outputs
    if (output.contains("FILINGS_DATA:")) {
        QString jsonStr = output.mid(output.indexOf("FILINGS_DATA:") + 13).trimmed();
        parseFilingsData(jsonStr);
    }
    else if (output.contains("TRANSACTIONS_DATA:")) {
        QString jsonStr = output.mid(output.indexOf("TRANSACTIONS_DATA:") + 18).trimmed();
        parseTransactionsData(jsonStr);
    }
    else if (output.contains("FINANCIALS_DATA:")) {
        QString jsonStr = output.mid(output.indexOf("FINANCIALS_DATA:") + 16).trimmed();
        parseFinancialsData(jsonStr);
    }
    else {
        emit apiError("Unexpected output: " + output);
    }
}

void SecApiClient::onPythonError(QProcess::ProcessError error) {
    QString errorString = QString("Python process error (%1): %2")
                         .arg(error)
                         .arg(m_pythonProcess->errorString());
    emit apiError(errorString);
}

QString SecApiClient::getScriptsPath() const {
    QString appDir = QCoreApplication::applicationDirPath();
    QDir dir(appDir);
    if (dir.cd("scripts")) {
        return dir.absolutePath();
    }

    dir = QDir(QDir::current());
    if (dir.cd("scripts")) {
        return dir.absolutePath();
    }

    dir = QDir(appDir);
    if (dir.cdUp() && dir.cd("scripts")) {
        return dir.absolutePath();
    }

    return QDir::current().absoluteFilePath("scripts");
}

void SecApiClient::parseFilingsData(const QString& jsonStr) {
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(jsonStr.toUtf8(), &error);
    
    if (error.error != QJsonParseError::NoError) {
        emit apiError("Failed to parse filings data: " + error.errorString());
        return;
    }
    
    QList<Filing> filings;
    QJsonArray array = doc.array();
    
    for (const QJsonValue& value : array) {
        QJsonObject obj = value.toObject();
        Filing filing;
        filing.date = obj["filingDate"].toString();
        filing.formType = obj["form"].toString();
        filing.description = obj["description"].toString();
        filing.url = obj["url"].toString();
        filings.append(filing);
    }
    
    emit filingsReady(filings);
    emit statusUpdate(QString("Loaded %1 filings").arg(filings.size()));
}

void SecApiClient::parseTransactionsData(const QString& jsonStr) {
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(jsonStr.toUtf8(), &error);
    
    if (error.error != QJsonParseError::NoError) {
        emit apiError("Failed to parse transactions data: " + error.errorString());
        return;
    }
    
    QList<Transaction> transactions;
    QJsonArray array = doc.array();
    
    for (const QJsonValue& value : array) {
        QJsonObject obj = value.toObject();
        Transaction tx;
        tx.date = obj["transactionDate"].toString();
        tx.insiderName = obj["insiderName"].toString();
        tx.transactionType = obj["transactionType"].toString();
        tx.shares = obj["shares"].toDouble();
        tx.price = obj["price"].toDouble();
        transactions.append(tx);
    }
    
    emit transactionsReady(transactions);
    emit statusUpdate(QString("Loaded %1 transactions").arg(transactions.size()));
}

void SecApiClient::parseFinancialsData(const QString& jsonStr) {
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(jsonStr.toUtf8(), &error);
    
    if (error.error != QJsonParseError::NoError) {
        emit apiError("Failed to parse financials data: " + error.errorString());
        return;
    }
    
    QList<FinancialMetric> metrics;
    QJsonObject obj = doc.object();
    
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        FinancialMetric metric;
        metric.name = it.key();
        if (it.value().isString()) {
            metric.value = it.value().toString();
            metric.unit = "";
        } else if (it.value().isObject()) {
            QJsonObject metricObj = it.value().toObject();
            metric.value = metricObj["value"].toString();
            metric.unit = metricObj["unit"].toString();
        } else {
            metric.value = QString::number(it.value().toDouble());
            metric.unit = "";
        }
        metrics.append(metric);
    }
    
    emit financialsReady(metrics);
    emit statusUpdate("Financial summary loaded");
}