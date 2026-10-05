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

SecFilingDock::SecFilingDock(QWidget* parent, ResearchProcess* runner)
    : DockablePanel("SecFilingDock", "SEC Filing Viewer", parent)
    , m_apiClient(new SecApiClient(this, runner))
    , m_filingsModel(new QStandardItemModel(this))
    , m_transactionsModel(new QStandardItemModel(this))
{
    buildUi();
    connect(m_apiClient, &SecApiClient::filingsReady, this, &SecFilingDock::onFilingsReady);
    connect(m_apiClient, &SecApiClient::transactionsReady, this, &SecFilingDock::onTransactionsReady);
    connect(m_apiClient, &SecApiClient::financialsReady, this, &SecFilingDock::onFinancialsReady);
    connect(m_apiClient, &SecApiClient::apiError, this, &SecFilingDock::onApiError);
    connect(m_apiClient, &SecApiClient::statusUpdate, this, &SecFilingDock::onStatusUpdate);
}

QSize SecFilingDock::minimumSizeHint() const {
    return QSize(340, 380);
}

void SecFilingDock::buildUi() {
    QVBoxLayout* layout = new QVBoxLayout(m_contentWidget);
    QHBoxLayout* inputLayout = new QHBoxLayout();
    inputLayout->addWidget(new QLabel("Ticker:", m_contentWidget));
    m_tickerInput = new QLineEdit("AAPL", m_contentWidget);
    m_tickerInput->setObjectName("secTicker");
    m_tickerInput->setMinimumWidth(60);
    inputLayout->addWidget(m_tickerInput, 1);
    
    inputLayout->addWidget(new QLabel("Form Type:", m_contentWidget));
    m_formTypeCombo = new QComboBox(m_contentWidget);
    m_formTypeCombo->addItems({"10-K", "10-Q", "8-K", "Form 4", "All"});
    inputLayout->addWidget(m_formTypeCombo);
    
    m_fetchFilingsBtn = new QPushButton("Fetch Filings", m_contentWidget);
    m_fetchInsiderBtn = new QPushButton("Fetch Insider Tx", m_contentWidget);
    m_fetchFinancialsBtn = new QPushButton("Fetch Financials", m_contentWidget);
    
    auto* actions = new QHBoxLayout;
    actions->addWidget(m_fetchFilingsBtn);
    actions->addWidget(m_fetchInsiderBtn);
    actions->addWidget(m_fetchFinancialsBtn);
    
    layout->addLayout(inputLayout);
    layout->addLayout(actions);
    connect(m_tickerInput, &QLineEdit::textChanged, this, &SecFilingDock::tickerEdited);
    
    connect(m_fetchFilingsBtn, &QPushButton::clicked, this, &SecFilingDock::fetchFilings);
    connect(m_fetchInsiderBtn, &QPushButton::clicked, this, &SecFilingDock::fetchInsiderTransactions);
    connect(m_fetchFinancialsBtn, &QPushButton::clicked, this, &SecFilingDock::fetchFinancialSummary);
    
    // Tables and display
    QSplitter* splitter = new QSplitter(Qt::Vertical, m_contentWidget);
    
    QGroupBox* filingsGroup = new QGroupBox("Filings", m_contentWidget);
    QVBoxLayout* filingsLayout = new QVBoxLayout();
    m_filingsTable = new QTableView(filingsGroup);
    m_filingsModel->setHorizontalHeaderLabels({"Date", "Form Type", "Description"});
    m_filingsTable->setObjectName("secFilings");
    m_filingsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_filingsTable->setModel(m_filingsModel);
    m_filingsTable->horizontalHeader()->setStretchLastSection(true);
    filingsLayout->addWidget(m_filingsTable);
    filingsGroup->setLayout(filingsLayout);
    splitter->addWidget(filingsGroup);
    QGroupBox* transactionsGroup = new QGroupBox("Insider Transactions", m_contentWidget);
    QVBoxLayout* transactionsLayout = new QVBoxLayout();
    m_transactionsTable = new QTableView(transactionsGroup);
    m_transactionsModel->setHorizontalHeaderLabels({"Date", "Insider", "Transaction", "Shares", "Price"});
    m_transactionsTable->setObjectName("secTransactions");
    m_transactionsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_transactionsTable->setModel(m_transactionsModel);
    m_transactionsTable->horizontalHeader()->setStretchLastSection(true);
    transactionsLayout->addWidget(m_transactionsTable);
    transactionsGroup->setLayout(transactionsLayout);
    splitter->addWidget(transactionsGroup);
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
    m_statusLabel = new QLabel("AAPL · SEC EDGAR · Retrieved: Unknown · Choose a report", m_contentWidget);
    m_statusLabel->setObjectName("secStatus");
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setTextFormat(Qt::PlainText);
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
    
    beginRequest();
    m_apiClient->fetchFilings(ticker, formType);
}

void SecFilingDock::fetchInsiderTransactions() {
    QString ticker = m_tickerInput->text().trimmed().toUpper();
    if (ticker.isEmpty()) {
        updateStatus("Please enter a ticker symbol", true);
        return;
    }
    
    beginRequest();
    m_apiClient->fetchInsiderTransactions(ticker);
}

void SecFilingDock::fetchFinancialSummary() {
    QString ticker = m_tickerInput->text().trimmed().toUpper();
    if (ticker.isEmpty()) {
        updateStatus("Please enter a ticker symbol", true);
        return;
    }
    
    beginRequest();
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
    updateStatus(provenance() + " · Unavailable: " + error, true);
}

void SecFilingDock::onStatusUpdate(const QString& message) {
    updateStatus(provenance() + " · " + message);
}

void SecFilingDock::displayFilings(const QList<SecApiClient::Filing>& filings) {
    m_filingsModel->clear();
    m_filingsModel->setHorizontalHeaderLabels({"Date", "Form Type", "Description"});
    
    for (const auto& filing : filings) {
        QList<QStandardItem*> row;
        row << new QStandardItem(filing.date);
        row << new QStandardItem(filing.formType);
        row << new QStandardItem(filing.description);
        for (auto* item : row) item->setEditable(false);
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
        row << new QStandardItem(tx.shares ? QString::number(*tx.shares, 'g', 12) : QStringLiteral("Unknown"));
        row << new QStandardItem(tx.price ? QString::number(*tx.price, 'g', 12) : QStringLiteral("Unknown"));
        for (auto* item : row) item->setEditable(false);
        row[3]->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        row[4]->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
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
    m_statusLabel->setToolTip(message);
    Q_UNUSED(isError);
}

void SecFilingDock::clearResults() {
    m_filingsModel->removeRows(0, m_filingsModel->rowCount());
    m_transactionsModel->removeRows(0, m_transactionsModel->rowCount());
    m_financialsDisplay->clear();
}
void SecFilingDock::beginRequest() { clearResults(); }
QString SecFilingDock::provenance() const {
    const auto received = m_apiClient->retrievedAt();
    return QString("%1 · SEC EDGAR · Retrieved: %2").arg(m_tickerInput->text().trimmed().toUpper(),
        received.isValid() ? received.toString("yyyy-MM-dd HH:mm:ss 'UTC'") : QStringLiteral("Unknown"));
}
void SecFilingDock::tickerEdited() {
    m_apiClient->cancel();
    clearResults();
    const bool supported = ResearchProcess::isEquityTicker(m_tickerInput->text());
    m_fetchFilingsBtn->setEnabled(supported);
    m_fetchInsiderBtn->setEnabled(supported);
    m_fetchFinancialsBtn->setEnabled(supported);
    updateStatus(provenance() + (supported ? " · Choose a report" : " · Unavailable: SEC reports require an equity ticker; crypto pairs are unsupported"));
}
void SecFilingDock::onSymbolChanged(const QString& symbol) {
    m_tickerInput->setText(symbol.trimmed().toUpper());
}
