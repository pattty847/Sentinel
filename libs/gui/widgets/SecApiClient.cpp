#include "SecApiClient.hpp"
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QDebug>
#include <QJsonParseError>

SecApiClient::SecApiClient(QObject* parent)
    : QObject(parent)
    , m_pythonProcess(nullptr)
    , m_pythonReady(false)
    , m_initTimer(new QTimer(this))
{
    initializePython();
}

SecApiClient::~SecApiClient() {
    if (m_pythonProcess) {
        m_pythonProcess->kill();
        m_pythonProcess->waitForFinished(3000);
    }
}

void SecApiClient::initializePython() {
    emit statusUpdate("Initializing SEC API...");
    
    // Test if we can import the SEC module
    QString testCommand = QString(
        "import sys; "
        "sys.path.insert(0, r'%1'); "
        "from sec.sec_api import SECDataFetcher; "
        "print('SEC_API_READY')"
    ).arg(getSecModulePath());
    
    m_currentOperation = "init";
    executePythonCommand(testCommand, "init");
}

void SecApiClient::fetchFilings(const QString& ticker, const QString& formType) {
    if (!m_pythonReady) {
        emit apiError("SEC API not ready");
        return;
    }
    
    emit statusUpdate(QString("Fetching %1 filings for %2...").arg(formType.isEmpty() ? "all" : formType, ticker));
    
    QString command = QString(
        "import sys, json, asyncio; "
        "sys.path.insert(0, r'%1'); "
        "from sec.sec_api import SECDataFetcher; "
        "async def fetch(): "
        "    f = SECDataFetcher(); "
        "    data = await f.get_filings_by_form('%2', %3); "
        "    print('FILINGS_DATA:' + json.dumps(data)); "
        "asyncio.run(fetch())"
    ).arg(getSecModulePath(), ticker, formType.isEmpty() ? "None" : QString("'%1'").arg(formType));
    
    m_currentOperation = "filings";
    executePythonCommand(command, "filings");
}

void SecApiClient::fetchInsiderTransactions(const QString& ticker) {
    if (!m_pythonReady) {
        emit apiError("SEC API not ready");
        return;
    }
    
    emit statusUpdate(QString("Fetching insider transactions for %1...").arg(ticker));
    
    QString command = QString(
        "import sys, json, asyncio; "
        "sys.path.insert(0, r'%1'); "
        "from sec.sec_api import SECDataFetcher; "
        "async def fetch(): "
        "    f = SECDataFetcher(); "
        "    data = await f.fetch_insider_filings('%2'); "
        "    print('TRANSACTIONS_DATA:' + json.dumps(data)); "
        "asyncio.run(fetch())"
    ).arg(getSecModulePath(), ticker);
    
    m_currentOperation = "transactions";
    executePythonCommand(command, "transactions");
}

void SecApiClient::fetchFinancialSummary(const QString& ticker) {
    if (!m_pythonReady) {
        emit apiError("SEC API not ready");
        return;
    }
    
    emit statusUpdate(QString("Fetching financial summary for %1...").arg(ticker));
    
    QString command = QString(
        "import sys, json, asyncio; "
        "sys.path.insert(0, r'%1'); "
        "from sec.sec_api import SECDataFetcher; "
        "async def fetch(): "
        "    f = SECDataFetcher(); "
        "    data = await f.get_financial_summary('%2'); "
        "    print('FINANCIALS_DATA:' + json.dumps(data)); "
        "asyncio.run(fetch())"
    ).arg(getSecModulePath(), ticker);
    
    m_currentOperation = "financials";
    executePythonCommand(command, "financials");
}

void SecApiClient::executePythonCommand(const QString& command, const QString& operation) {
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
    
    QStringList args;
    args << "-c" << command;
    
    QString pythonExe = getPythonExecutable();
    qDebug() << "Executing Python command:" << pythonExe << args;
    
    m_pythonProcess->start(pythonExe, args);
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
    qDebug() << "Python output:" << output;
    
    if (m_currentOperation == "init") {
        if (output.contains("SEC_API_READY")) {
            m_pythonReady = true;
            emit statusUpdate("SEC API ready");
        } else {
            emit apiError("Failed to initialize SEC API: " + output);
        }
        return;
    }
    
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

QString SecApiClient::getPythonExecutable() const {
    // Try to use the venv python first
    QDir currentDir = QDir::current();
    QString venvPython = currentDir.absoluteFilePath(".venv/Scripts/python.exe");
    
    if (QFileInfo::exists(venvPython)) {
        return venvPython;
    }
    
    // Fallback to system python
    #ifdef Q_OS_WIN
    return "python";
    #else
    return "python3";
    #endif
}

QString SecApiClient::getSecModulePath() const {
    return QDir::current().absolutePath();
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