#include "SecApiClient.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QDebug>
#include <QJsonParseError>

SecApiClient::SecApiClient(QObject* parent, ResearchProcess* runner)
    : QObject(parent), m_runner(runner ? runner : new ResearchProcess(this)) {
    connect(m_runner, &ResearchProcess::completed, this, &SecApiClient::acceptResult);
}
SecApiClient::~SecApiClient() { cancel(); }
void SecApiClient::cancel() {
    ++m_request;
    m_pending = false;
    m_retrievedAt = {};
    if (m_runner) m_runner->cancel();
}
void SecApiClient::fetchFilings(const QString& ticker, const QString& formType) {
    QStringList args{ticker};
    if (!formType.isEmpty()) args << formType;
    runSecScript("sec/sec_fetch_filings.py", args, "filings");
}
void SecApiClient::fetchInsiderTransactions(const QString& ticker) {
    runSecScript("sec/sec_fetch_transactions.py", {ticker}, "transactions");
}
void SecApiClient::fetchInsiderSignals(const QString& ticker, int daysBack) {
    runSecScript("sec/sec_fetch_signals.py", {ticker, QString::number(daysBack)}, "insider_signals");
}
void SecApiClient::fetchFinancialSummary(const QString& ticker) {
    runSecScript("sec/sec_fetch_financials.py", {ticker}, "financials");
}
void SecApiClient::runSecScript(const QString& scriptName, const QStringList& args, const QString& operation) {
    cancel();
    m_ticker = args.value(0).trimmed().toUpper();
    m_currentOperation = operation;
    if (!ResearchProcess::isEquityTicker(m_ticker)) {
        emit apiError("SEC supports equity tickers; crypto pairs and other instruments are unavailable");
        return;
    }
    if (!m_runner) { emit apiError("SEC request provider unavailable"); return; }
    const auto request = m_request;
    emit statusUpdate(QString("Loading %1 for %2 · SEC EDGAR").arg(operation, m_ticker));
    if (request != m_request || !m_runner) return;
    QStringList normalized = args;
    normalized[0] = m_ticker;
    m_pending = true;
    m_runner->run(m_request, QDir(getScriptsPath()).filePath(scriptName), normalized);
}
void SecApiClient::acceptResult(quint64 request, const QByteArray& output, const QString& error) {
    if (request != m_request || !m_pending) return;
    m_pending = false;
    // Match only the current operation's line, not a marker from another request.
    const QByteArray marker = m_currentOperation == "filings" ? "FILINGS_DATA:"
        : m_currentOperation == "transactions" ? "TRANSACTIONS_DATA:"
        : m_currentOperation == "insider_signals" ? "INSIDER_SIGNALS_DATA:" : "FINANCIALS_DATA:";
    QByteArray payload, providerError;
    for (const auto& line : output.split('\n')) {
        if (line.startsWith(marker)) payload = line.mid(marker.size()).trimmed();
        if (line.startsWith("ERROR_DATA:")) providerError = line.mid(11).trimmed();
    }
    if (!providerError.isEmpty()) {
        const auto doc = QJsonDocument::fromJson(providerError);
        emit apiError(doc.object().value("error").toString("SEC provider returned an error"));
        return;
    }
    if (!error.isEmpty()) { emit apiError(error); return; }
    if (payload.isEmpty()) { emit apiError("SEC provider returned no matching response"); return; }
    m_retrievedAt = QDateTime::currentDateTimeUtc();
    if (m_currentOperation == "filings") parseFilingsData(QString::fromUtf8(payload));
    else if (m_currentOperation == "transactions") parseTransactionsData(QString::fromUtf8(payload));
    else if (m_currentOperation == "insider_signals") parseInsiderSignalsData(QString::fromUtf8(payload));
    else parseFinancialsData(QString::fromUtf8(payload));
}
QString SecApiClient::getScriptsPath() const { return ResearchProcess::scriptsPath(); }

void SecApiClient::parseFilingsData(const QString& jsonStr) {
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(jsonStr.toUtf8(), &error);
    
    if (error.error != QJsonParseError::NoError || !doc.isArray()) {
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
    
    const auto request = m_request;
    emit filingsReady(filings);
    if (request == m_request) emit statusUpdate(QString("Loaded %1 filings").arg(filings.size()));
}

void SecApiClient::parseTransactionsData(const QString& jsonStr) {
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(jsonStr.toUtf8(), &error);
    
    if (error.error != QJsonParseError::NoError || !doc.isArray()) {
        emit apiError("Failed to parse transactions data: " + error.errorString());
        return;
    }
    
    QList<Transaction> transactions;
    QJsonArray array = doc.array();
    
    for (const QJsonValue& value : array) {
        QJsonObject obj = value.toObject();
        Transaction tx;
        tx.date = obj["date"].toString();
        tx.insiderName = obj["filer"].toString();
        tx.transactionType = obj["type"].toString();
        if (obj["shares"].isDouble()) tx.shares = obj["shares"].toDouble();
        if (obj["price"].isDouble()) tx.price = obj["price"].toDouble();
        transactions.append(tx);
    }
    
    const auto request = m_request;
    emit transactionsReady(transactions);
    if (request == m_request) emit statusUpdate(QString("Loaded %1 transactions").arg(transactions.size()));
}

void SecApiClient::parseInsiderSignalsData(const QString& jsonStr) {
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(jsonStr.toUtf8(), &error);

    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        emit apiError("Failed to parse insider signals data: " + error.errorString());
        return;
    }

    const QString symbol = doc.object().value("symbol").toString().trimmed().toUpper();
    if (symbol != m_ticker) { emit apiError("SEC signal response symbol did not match the request"); return; }
    const auto request = m_request;
    emit insiderSignalsReady(doc.object());
    if (request == m_request) emit statusUpdate("Insider signals loaded");
}

void SecApiClient::parseFinancialsData(const QString& jsonStr) {
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(jsonStr.toUtf8(), &error);
    
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        emit apiError("Failed to parse financials data: " + error.errorString());
        return;
    }
    
    QList<FinancialMetric> metrics;
    const QJsonObject obj = doc.object();
    const QString ticker = obj.value("ticker").toString().trimmed().toUpper();
    if (!ticker.isEmpty() && ticker != m_ticker) {
        emit apiError("SEC financial response symbol did not match the request");
        return;
    }
    auto displayValue = [](const QJsonValue& value) {
        if (value.isDouble()) return QString::number(value.toDouble(), 'g', 12);
        if (value.isString() && !value.toString().isEmpty()) return value.toString();
        return QStringLiteral("Unknown");
    };
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        FinancialMetric metric;
        metric.name = it.key();
        if (it.value().isObject()) {
            const auto history = it.value().toObject();
            if (history.contains("quarterly") || history.contains("annual")) {
                // The provider returns cleaned histories with period/date/value/form,
                // without units. Pick the newest observation; prefer quarterly on ties.
                QJsonObject selected;
                QDate selectedDate;
                for (const auto& cadence : {QStringLiteral("quarterly"), QStringLiteral("annual")}) {
                    for (const auto& value : history.value(cadence).toArray()) {
                        if (!value.isObject()) continue;
                        const auto entry = value.toObject();
                        const auto date = QDate::fromString(entry.value("date").toString(), Qt::ISODate);
                        if (selected.isEmpty() || (date.isValid() && (!selectedDate.isValid() || date > selectedDate))) {
                            selected = entry;
                            selectedDate = date;
                            metric.cadence = cadence;
                        }
                    }
                }
                metric.value = displayValue(selected.value("value"));
                metric.period = selected.value("period").toString("Unknown");
                metric.date = selectedDate.isValid() ? selectedDate.toString(Qt::ISODate) : QStringLiteral("Unknown");
                metric.unit = selected.value("unit").toString();
            } else {
                metric.value = displayValue(history.value("value"));
                metric.unit = history.value("unit").toString();
            }
        } else {
            metric.value = displayValue(it.value());
        }
        metrics.append(metric);
    }

    const auto request = m_request;
    emit financialsReady(metrics);
    if (request == m_request) emit statusUpdate("Financial summary loaded");
}
