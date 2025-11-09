#include "SecFilingDock.hpp"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QSplitter>
#include <QHeaderView>
#include <QJsonObject>
#include <QJsonArray>
#include <QMessageBox>
#include <QStandardItem>
#include <QList>

SecFilingDock::SecFilingDock(QWidget* parent)
    : DockablePanel("SecFilingDock", "SEC Filing Viewer", parent)
    , m_serviceBridge(new SecServiceBridge(this))
    , m_networkManager(new QNetworkAccessManager(this))
    , m_filingsModel(new QStandardItemModel(this))
    , m_transactionsModel(new QStandardItemModel(this))
{
    buildUi();
    
    connect(m_serviceBridge, &SecServiceBridge::serviceError, this, &SecFilingDock::onServiceError);
    
    // Start service
    if (!m_serviceBridge->startService()) {
        updateStatus("Failed to start SEC service", true);
    }
}

void SecFilingDock::buildUi() {
    QVBoxLayout* layout = new QVBoxLayout(m_contentWidget);
    
    // Input controls
    QHBoxLayout* inputLayout = new QHBoxLayout();
    inputLayout->addWidget(new QLabel("Ticker:", m_contentWidget));
    m_tickerInput = new QLineEdit("AAPL", m_contentWidget);
    inputLayout->addWidget(m_tickerInput);
    
    inputLayout->addWidget(new QLabel("Form Type:", m_contentWidget));
    m_formTypeCombo = new QComboBox(m_contentWidget);
    m_formTypeCombo->addItems({"10-K", "10-Q", "8-K", "Form 4", "All"});
    inputLayout->addWidget(m_formTypeCombo);
    
    m_fetchFilingsBtn = new QPushButton("Fetch Filings", m_contentWidget);
    m_fetchInsiderBtn = new QPushButton("Fetch Insider Tx", m_contentWidget);
    m_fetchFinancialsBtn = new QPushButton("Fetch Financials", m_contentWidget);
    
    inputLayout->addWidget(m_fetchFilingsBtn);
    inputLayout->addWidget(m_fetchInsiderBtn);
    inputLayout->addWidget(m_fetchFinancialsBtn);
    
    layout->addLayout(inputLayout);
    
    connect(m_fetchFilingsBtn, &QPushButton::clicked, this, &SecFilingDock::fetchFilings);
    connect(m_fetchInsiderBtn, &QPushButton::clicked, this, &SecFilingDock::fetchInsiderTransactions);
    connect(m_fetchFinancialsBtn, &QPushButton::clicked, this, &SecFilingDock::fetchFinancialSummary);
    
    // Tables and display
    QSplitter* splitter = new QSplitter(Qt::Vertical, m_contentWidget);
    
    // Filings table
    QGroupBox* filingsGroup = new QGroupBox("Filings", m_contentWidget);
    QVBoxLayout* filingsLayout = new QVBoxLayout();
    m_filingsTable = new QTableView(filingsGroup);
    m_filingsModel->setHorizontalHeaderLabels({"Date", "Form Type", "Description"});
    m_filingsTable->setModel(m_filingsModel);
    m_filingsTable->horizontalHeader()->setStretchLastSection(true);
    filingsLayout->addWidget(m_filingsTable);
    filingsGroup->setLayout(filingsLayout);
    splitter->addWidget(filingsGroup);
    
    // Transactions table
    QGroupBox* transactionsGroup = new QGroupBox("Insider Transactions", m_contentWidget);
    QVBoxLayout* transactionsLayout = new QVBoxLayout();
    m_transactionsTable = new QTableView(transactionsGroup);
    m_transactionsModel->setHorizontalHeaderLabels({"Date", "Insider", "Transaction", "Shares", "Price"});
    m_transactionsTable->setModel(m_transactionsModel);
    m_transactionsTable->horizontalHeader()->setStretchLastSection(true);
    transactionsLayout->addWidget(m_transactionsTable);
    transactionsGroup->setLayout(transactionsLayout);
    splitter->addWidget(transactionsGroup);
    
    // Financials display
    QGroupBox* financialsGroup = new QGroupBox("Financial Summary", m_contentWidget);
    QVBoxLayout* financialsLayout = new QVBoxLayout();
    m_financialsDisplay = new QTextEdit(financialsGroup);
    m_financialsDisplay->setReadOnly(true);
    financialsLayout->addWidget(m_financialsDisplay);
    financialsGroup->setLayout(financialsLayout);
    splitter->addWidget(financialsGroup);
    
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    splitter->setStretchFactor(2, 1);
    
    layout->addWidget(splitter);
    
    // Status bar
    m_statusLabel = new QLabel("Ready", m_contentWidget);
    m_statusLabel->setStyleSheet("QLabel { background-color: #333; color: white; padding: 4px; }");
    layout->addWidget(m_statusLabel);
    
    m_contentWidget->setLayout(layout);
}

void SecFilingDock::fetchFilings() {
    QString ticker = m_tickerInput->text().trimmed().toUpper();
    if (ticker.isEmpty()) {
        updateStatus("Please enter a ticker symbol", true);
        return;
    }
    
    QString formType = m_formTypeCombo->currentText();
    if (formType == "All") formType = "";
    
    QString url = QString("%1/api/filings?ticker=%2").arg(m_serviceBridge->serviceUrl().toString(), ticker);
    if (!formType.isEmpty()) {
        url += QString("&form_type=%1").arg(formType);
    }
    
    updateStatus("Fetching filings...");
    QNetworkReply* reply = m_networkManager->get(QNetworkRequest(QUrl(url)));
    connect(reply, &QNetworkReply::finished, this, &SecFilingDock::onFilingsReply);
}

void SecFilingDock::fetchInsiderTransactions() {
    QString ticker = m_tickerInput->text().trimmed().toUpper();
    if (ticker.isEmpty()) {
        updateStatus("Please enter a ticker symbol", true);
        return;
    }
    
    QString url = QString("%1/api/insider-transactions?ticker=%2").arg(m_serviceBridge->serviceUrl().toString(), ticker);
    
    updateStatus("Fetching insider transactions...");
    QNetworkReply* reply = m_networkManager->get(QNetworkRequest(QUrl(url)));
    connect(reply, &QNetworkReply::finished, this, &SecFilingDock::onTransactionsReply);
}

void SecFilingDock::fetchFinancialSummary() {
    QString ticker = m_tickerInput->text().trimmed().toUpper();
    if (ticker.isEmpty()) {
        updateStatus("Please enter a ticker symbol", true);
        return;
    }
    
    QString url = QString("%1/api/financial-summary?ticker=%2").arg(m_serviceBridge->serviceUrl().toString(), ticker);
    
    updateStatus("Fetching financial summary...");
    QNetworkReply* reply = m_networkManager->get(QNetworkRequest(QUrl(url)));
    connect(reply, &QNetworkReply::finished, this, &SecFilingDock::onFinancialsReply);
}

void SecFilingDock::onFilingsReply() {
    QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;
    
    if (reply->error() != QNetworkReply::NoError) {
        updateStatus("Error fetching filings: " + reply->errorString(), true);
        reply->deleteLater();
        return;
    }
    
    QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
    parseFilings(doc);
    reply->deleteLater();
}

void SecFilingDock::onTransactionsReply() {
    QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;
    
    if (reply->error() != QNetworkReply::NoError) {
        updateStatus("Error fetching transactions: " + reply->errorString(), true);
        reply->deleteLater();
        return;
    }
    
    QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
    parseTransactions(doc);
    reply->deleteLater();
}

void SecFilingDock::onFinancialsReply() {
    QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;
    
    if (reply->error() != QNetworkReply::NoError) {
        updateStatus("Error fetching financials: " + reply->errorString(), true);
        reply->deleteLater();
        return;
    }
    
    QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
    parseFinancials(doc);
    reply->deleteLater();
}

void SecFilingDock::parseFilings(const QJsonDocument& doc) {
    m_filingsModel->clear();
    m_filingsModel->setHorizontalHeaderLabels({"Date", "Form Type", "Description"});
    
    QJsonObject obj = doc.object();
    QJsonArray filings = obj["filings"].toArray();
    
    for (const QJsonValue& value : filings) {
        QJsonObject filing = value.toObject();
        QList<QStandardItem*> row;
        row << new QStandardItem(filing["filingDate"].toString());
        row << new QStandardItem(filing["form"].toString());
        row << new QStandardItem(filing["description"].toString());
        m_filingsModel->appendRow(row);
    }
    
    updateStatus(QString("Loaded %1 filings").arg(filings.size()));
}

void SecFilingDock::parseTransactions(const QJsonDocument& doc) {
    m_transactionsModel->clear();
    m_transactionsModel->setHorizontalHeaderLabels({"Date", "Insider", "Transaction", "Shares", "Price"});
    
    QJsonObject obj = doc.object();
    QJsonArray transactions = obj["transactions"].toArray();
    
    for (const QJsonValue& value : transactions) {
        QJsonObject tx = value.toObject();
        QList<QStandardItem*> row;
        row << new QStandardItem(tx["transactionDate"].toString());
        row << new QStandardItem(tx["insiderName"].toString());
        row << new QStandardItem(tx["transactionType"].toString());
        row << new QStandardItem(QString::number(tx["shares"].toDouble()));
        row << new QStandardItem(QString::number(tx["price"].toDouble()));
        m_transactionsModel->appendRow(row);
    }
    
    updateStatus(QString("Loaded %1 transactions").arg(transactions.size()));
}

void SecFilingDock::parseFinancials(const QJsonDocument& doc) {
    QJsonObject obj = doc.object();
    QJsonObject summary = obj["summary"].toObject();
    
    QString text = "Financial Summary\n\n";
    for (auto it = summary.begin(); it != summary.end(); ++it) {
        text += QString("%1: %2\n").arg(it.key(), it.value().toString());
    }
    
    m_financialsDisplay->setPlainText(text);
    updateStatus("Financial summary loaded");
}

void SecFilingDock::updateStatus(const QString& message, bool isError) {
    m_statusLabel->setText(message);
    m_statusLabel->setStyleSheet(QString("QLabel { background-color: %1; color: white; padding: 4px; }")
                                .arg(isError ? "#800" : "#333"));
}

void SecFilingDock::onSymbolChanged(const QString& symbol) {
    // Extract ticker from symbol (e.g., "BTC-USD" -> "BTC")
    QString ticker = symbol.split('-').first();
    m_tickerInput->setText(ticker);
    // Optionally auto-fetch filings
}

void SecFilingDock::onServiceError(const QString& error) {
    updateStatus("Service error: " + error, true);
}

