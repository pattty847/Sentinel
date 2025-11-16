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
    , m_apiClient(new SecApiClient(this))
    , m_filingsModel(new QStandardItemModel(this))
    , m_transactionsModel(new QStandardItemModel(this))
{
    buildUi();
    
    // Connect to API client signals
    connect(m_apiClient, &SecApiClient::filingsReady, this, &SecFilingDock::onFilingsReady);
    connect(m_apiClient, &SecApiClient::transactionsReady, this, &SecFilingDock::onTransactionsReady);
    connect(m_apiClient, &SecApiClient::financialsReady, this, &SecFilingDock::onFinancialsReady);
    connect(m_apiClient, &SecApiClient::apiError, this, &SecFilingDock::onApiError);
    connect(m_apiClient, &SecApiClient::statusUpdate, this, &SecFilingDock::onStatusUpdate);
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
    
    m_apiClient->fetchFilings(ticker, formType);
}

void SecFilingDock::fetchInsiderTransactions() {
    QString ticker = m_tickerInput->text().trimmed().toUpper();
    if (ticker.isEmpty()) {
        updateStatus("Please enter a ticker symbol", true);
        return;
    }
    
    m_apiClient->fetchInsiderTransactions(ticker);
}

void SecFilingDock::fetchFinancialSummary() {
    QString ticker = m_tickerInput->text().trimmed().toUpper();
    if (ticker.isEmpty()) {
        updateStatus("Please enter a ticker symbol", true);
        return;
    }
    
    m_apiClient->fetchFinancialSummary(ticker);
}

void SecFilingDock::onFilingsReady(const QList<SecApiClient::Filing>& filings) {
    displayFilings(filings);
}

void SecFilingDock::onTransactionsReady(const QList<SecApiClient::Transaction>& transactions) {
    displayTransactions(transactions);
}

void SecFilingDock::onFinancialsReady(const QList<SecApiClient::FinancialMetric>& metrics) {
    displayFinancials(metrics);
}

void SecFilingDock::onApiError(const QString& error) {
    updateStatus("API Error: " + error, true);
}

void SecFilingDock::onStatusUpdate(const QString& message) {
    updateStatus(message);
}

void SecFilingDock::displayFilings(const QList<SecApiClient::Filing>& filings) {
    m_filingsModel->clear();
    m_filingsModel->setHorizontalHeaderLabels({"Date", "Form Type", "Description"});
    
    for (const auto& filing : filings) {
        QList<QStandardItem*> row;
        row << new QStandardItem(filing.date);
        row << new QStandardItem(filing.formType);
        row << new QStandardItem(filing.description);
        m_filingsModel->appendRow(row);
    }
}

void SecFilingDock::displayTransactions(const QList<SecApiClient::Transaction>& transactions) {
    m_transactionsModel->clear();
    m_transactionsModel->setHorizontalHeaderLabels({"Date", "Insider", "Transaction", "Shares", "Price"});
    
    for (const auto& tx : transactions) {
        QList<QStandardItem*> row;
        row << new QStandardItem(tx.date);
        row << new QStandardItem(tx.insiderName);
        row << new QStandardItem(tx.transactionType);
        row << new QStandardItem(QString::number(tx.shares));
        row << new QStandardItem(QString::number(tx.price));
        m_transactionsModel->appendRow(row);
    }
}

void SecFilingDock::displayFinancials(const QList<SecApiClient::FinancialMetric>& metrics) {
    QString text = "Financial Summary\n\n";
    for (const auto& metric : metrics) {
        QString value = metric.value;
        if (!metric.unit.isEmpty()) {
            value += " " + metric.unit;
        }
        text += QString("%1: %2\n").arg(metric.name, value);
    }
    
    m_financialsDisplay->setPlainText(text);
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


