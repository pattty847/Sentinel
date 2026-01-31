#include "SecApiClient.hpp"
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkRequest>

SecApiClient::SecApiClient(QObject* parent)
    : QObject(parent)
    , m_network(new QNetworkAccessManager(this))
    , m_serverReady(false)
{
    initializeServer();
}

SecApiClient::~SecApiClient() = default;

void SecApiClient::initializeServer() {
    emit statusUpdate("Checking SEC backend...");

    QNetworkRequest request(QUrl(baseUrl() + "/sec/ping"));
    auto* reply = m_network->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        const QByteArray payload = reply->readAll();
        const bool networkOk = reply->error() == QNetworkReply::NoError;
        const QString networkError = reply->errorString();
        reply->deleteLater();

        if (!networkOk) {
            m_serverReady = false;
            emit apiError("SEC backend not reachable: " + networkError);
            return;
        }

        QJsonParseError error;
        const QJsonDocument doc = QJsonDocument::fromJson(payload, &error);
        if (error.error != QJsonParseError::NoError || !doc.isObject()) {
            m_serverReady = false;
            emit apiError("SEC backend returned invalid JSON");
            return;
        }

        const QJsonObject obj = doc.object();
        if (!obj.value("ok").toBool()) {
            m_serverReady = false;
            emit apiError("SEC backend not ready");
            return;
        }

        m_serverReady = true;
        emit statusUpdate("SEC backend ready");
    });
}

void SecApiClient::fetchFilings(const QString& ticker, const QString& formType) {
    if (ticker.trimmed().isEmpty()) {
        emit apiError("Ticker required");
        return;
    }

    QJsonObject payload;
    payload["ticker"] = ticker.trimmed().toUpper();
    QString normalizedForm = formType.trimmed();
    if (normalizedForm == "Form 4") {
        normalizedForm = "4";
    }
    if (!normalizedForm.isEmpty() && normalizedForm != "All") {
        payload["form_type"] = normalizedForm;
    }

    emit statusUpdate(QString("Fetching filings for %1...").arg(ticker.trimmed().toUpper()));
    postJson("/sec/filings", payload, "filings");
}

void SecApiClient::fetchInsiderTransactions(const QString& ticker) {
    if (ticker.trimmed().isEmpty()) {
        emit apiError("Ticker required");
        return;
    }

    QJsonObject payload;
    payload["ticker"] = ticker.trimmed().toUpper();

    emit statusUpdate(QString("Fetching insider transactions for %1...").arg(ticker.trimmed().toUpper()));
    postJson("/sec/insider", payload, "transactions");
}

void SecApiClient::fetchFinancialSummary(const QString& ticker) {
    if (ticker.trimmed().isEmpty()) {
        emit apiError("Ticker required");
        return;
    }

    QJsonObject payload;
    payload["ticker"] = ticker.trimmed().toUpper();

    emit statusUpdate(QString("Fetching financial summary for %1...").arg(ticker.trimmed().toUpper()));
    postJson("/sec/financials", payload, "financials");
}

void SecApiClient::postJson(const QString& path, const QJsonObject& payload, const QString& operation) {
    QNetworkRequest request(QUrl(baseUrl() + path));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    auto* reply = m_network->post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, operation]() {
        handleReply(reply, operation);
    });
}

void SecApiClient::handleReply(QNetworkReply* reply, const QString& operation) {
    const QByteArray payload = reply->readAll();
    const bool networkOk = reply->error() == QNetworkReply::NoError;
    const QString networkError = reply->errorString();
    reply->deleteLater();

    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(payload, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        emit apiError("SEC backend invalid JSON response");
        return;
    }

    const QJsonObject obj = doc.object();
    if (!networkOk || !obj.value("ok").toBool(true)) {
        const QString detail = obj.value("error").toString();
        emit apiError(detail.isEmpty() ? networkError : detail);
        return;
    }

    if (operation == "filings") {
        parseFilingsData(obj);
    } else if (operation == "transactions") {
        parseTransactionsData(obj);
    } else if (operation == "financials") {
        parseFinancialsData(obj);
    } else {
        emit apiError("Unknown SEC operation");
    }
}

QString SecApiClient::baseUrl() const {
    const QByteArray portEnv = qgetenv("SENTINEL_SEC_PORT");
    bool ok = false;
    const int portValue = portEnv.toInt(&ok);
    const int port = (ok && portValue > 0) ? portValue : 17110;
    return QString("http://127.0.0.1:%1").arg(port);
}

void SecApiClient::parseFilingsData(const QJsonObject& obj) {
    const QJsonArray array = obj.value("data").toArray();
    QList<Filing> filings;
    filings.reserve(array.size());

    for (const QJsonValue& value : array) {
        const QJsonObject filingObj = value.toObject();
        Filing filing;
        filing.accessionNo = filingObj.value("accession_no").toString();
        filing.date = filingObj.value("filing_date").toString();
        filing.formType = filingObj.value("form").toString();
        filing.reportDate = filingObj.value("report_date").toString();
        filing.description = filingObj.value("primary_document_description").toString();
        filing.url = filingObj.value("url").toString();
        filing.primaryDocument = filingObj.value("primary_document").toString();
        if (filing.description.isEmpty()) {
            filing.description = filing.primaryDocument;
        }
        filings.append(filing);
    }

    emit filingsReady(filings);
    emit statusUpdate(QString("Loaded %1 filings").arg(filings.size()));
}

void SecApiClient::parseTransactionsData(const QJsonObject& obj) {
    const QJsonArray array = obj.value("data").toArray();
    QList<Transaction> transactions;
    transactions.reserve(array.size());

    for (const QJsonValue& value : array) {
        const QJsonObject txObj = value.toObject();
        Transaction tx;
        tx.date = txObj.value("date").toString();
        tx.insiderName = txObj.value("filer").toString();
        tx.position = txObj.value("position").toString();
        tx.transactionType = txObj.value("type").toString();
        tx.shares = txObj.value("shares").toDouble();
        tx.price = txObj.value("price").toDouble();
        tx.value = txObj.value("value").toDouble();
        tx.formUrl = txObj.value("form_url").toString();
        tx.primaryDocument = txObj.value("primary_document").toString();
        transactions.append(tx);
    }

    emit transactionsReady(transactions);
    emit statusUpdate(QString("Loaded %1 transactions").arg(transactions.size()));
}

static QString formatMetricValue(const QJsonValue& value) {
    if (value.isString()) {
        return value.toString();
    }
    if (value.isDouble()) {
        return QString::number(value.toDouble(), 'g', 12);
    }
    return QString();
}

void SecApiClient::parseFinancialsData(const QJsonObject& obj) {
    const QJsonObject dataObj = obj.value("data").toObject();
    QList<FinancialMetric> metrics;

    const QString entityName = dataObj.value("entityName").toString();
    const QString periodEnd = dataObj.value("period_end").toString();
    const QString sourceForm = dataObj.value("source_form").toString();

    const QStringList metaKeys = {"ticker", "entityName", "cik", "source_form", "period_end"};

    for (auto it = dataObj.begin(); it != dataObj.end(); ++it) {
        if (metaKeys.contains(it.key())) {
            continue;
        }
        const QJsonObject metricObj = it.value().toObject();
        const QJsonArray quarterly = metricObj.value("quarterly").toArray();
        const QJsonArray annual = metricObj.value("annual").toArray();

        QJsonObject entry;
        QString periodType;
        if (!quarterly.isEmpty()) {
            entry = quarterly.first().toObject();
            periodType = "quarterly";
        } else if (!annual.isEmpty()) {
            entry = annual.first().toObject();
            periodType = "annual";
        }

        FinancialMetric metric;
        metric.name = it.key();
        metric.period = entry.value("period").toString();
        metric.form = entry.value("form").toString();
        metric.periodType = periodType;
        metric.value = formatMetricValue(entry.value("value"));
        metrics.append(metric);
    }

    emit financialsReady(metrics, periodEnd, sourceForm, entityName);
    emit statusUpdate("Financial summary loaded");
}
