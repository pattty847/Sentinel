#include "SecFilingDock.hpp"
#include "TableChrome.hpp"
#include "EmptyState.hpp"
#include <QVBoxLayout>
#include <QHBoxLayout>
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
    
    // The reason the fetch buttons can be disabled sits under the field it depends on.
    m_tickerHint = new QLabel("Equity tickers only", m_contentWidget);
    m_tickerHint->setObjectName("secTickerHint");
    m_tickerHint->setStyleSheet(QStringLiteral("color:%1;").arg(theme::text3().name()));
    const QString actionStyle = QStringLiteral(
        "QPushButton { background:%1; color:%2; border:1px solid %3; border-radius:3px; padding:4px 10px; }"
        "QPushButton:hover { border-color:%4; }"
        "QPushButton:pressed { background:%5; }"
        "QPushButton:disabled { background:transparent; color:%6; border-color:%3; }")
        .arg(theme::bg2().name(), theme::text().name(), theme::line().name(),
             theme::accent().name(), theme::sel().name(), theme::textOff().name());
    for (auto* button : {m_fetchFilingsBtn, m_fetchInsiderBtn, m_fetchFinancialsBtn})
        button->setStyleSheet(actionStyle);

    layout->addLayout(inputLayout);
    layout->addWidget(m_tickerHint);
    layout->addLayout(actions);
    connect(m_tickerInput, &QLineEdit::textChanged, this, &SecFilingDock::tickerEdited);
    
    connect(m_fetchFilingsBtn, &QPushButton::clicked, this, &SecFilingDock::fetchFilings);
    connect(m_fetchInsiderBtn, &QPushButton::clicked, this, &SecFilingDock::fetchInsiderTransactions);
    connect(m_fetchFinancialsBtn, &QPushButton::clicked, this, &SecFilingDock::fetchFinancialSummary);
    
    // Tables and display
    QSplitter* splitter = new QSplitter(Qt::Vertical, m_contentWidget);
    
    auto section = [splitter](const QString& title, QWidget* body) {
        auto* box = new QWidget(splitter);
        auto* column = new QVBoxLayout(box);
        column->setContentsMargins(0, 4, 0, 0);
        column->setSpacing(4);
        auto* label = new QLabel(title, box);
        label->setStyleSheet(QStringLiteral("color:%1;").arg(theme::text2().name()));
        column->addWidget(label);
        body->setParent(box);
        column->addWidget(body, 1);
        splitter->addWidget(box);
    };
    m_filingsTable = new QTableView;
    m_filingsModel->setHorizontalHeaderLabels({"Date", "Form Type", "Description"});
    m_filingsTable->setObjectName("secFilings");
    m_filingsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_filingsTable->setModel(m_filingsModel);
    tableChrome::apply(m_filingsTable);
    m_filingsTable->horizontalHeader()->setStretchLastSection(true);
    section("Filings", m_filingsTable);
    m_transactionsTable = new QTableView;
    m_transactionsModel->setHorizontalHeaderLabels({"Date", "Insider", "Transaction", "Shares", "Price"});
    tableChrome::align(m_transactionsModel, 3, true);
    tableChrome::align(m_transactionsModel, 4, true);
    m_transactionsTable->setObjectName("secTransactions");
    m_transactionsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_transactionsTable->setModel(m_transactionsModel);
    tableChrome::apply(m_transactionsTable);
    m_transactionsTable->horizontalHeader()->setStretchLastSection(true);
    section("Insider transactions", m_transactionsTable);
    m_financialsDisplay = new QTextEdit;
    m_financialsDisplay->setObjectName("secFinancials");
    m_financialsDisplay->setReadOnly(true);
    section("Financial summary", m_financialsDisplay);
    for (auto* table : {m_filingsTable, m_transactionsTable}) fitEmptyColumns(table);
    m_filingsEmpty = new EmptyState(m_filingsTable, QString());
    m_transactionsEmpty = new EmptyState(m_transactionsTable, QString());
    updateEmptyHints();
    
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    splitter->setStretchFactor(2, 1);
    
    layout->addWidget(splitter);
    
    // Status bar
    m_statusLabel = new QLabel("AAPL · SEC EDGAR · Retrieved: Unknown · Choose a report", m_contentWidget);
    m_statusLabel->setObjectName("secStatus");
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setTextFormat(Qt::PlainText);
    m_statusLabel->setStyleSheet(QStringLiteral("color:%1;").arg(theme::text3().name()));
    layout->addWidget(m_statusLabel);
    
    m_contentWidget->setLayout(layout);
}

void SecFilingDock::fetchFilings() {
    QString ticker = m_tickerInput->text().trimmed().toUpper();
    if (ticker.isEmpty()) {
        updateStatus("Enter a ticker", true);
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
        updateStatus("Enter a ticker", true);
        return;
    }
    
    beginRequest();
    m_apiClient->fetchInsiderTransactions(ticker);
}

void SecFilingDock::fetchFinancialSummary() {
    QString ticker = m_tickerInput->text().trimmed().toUpper();
    if (ticker.isEmpty()) {
        updateStatus("Enter a ticker", true);
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
    updateStatus(QStringLiteral("%1: %2").arg(m_tickerInput->text().trimmed().toUpper(), error), true);
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
    m_filingsTable->resizeColumnsToContents();
    fitEmptyColumns(m_filingsTable);
}

void SecFilingDock::displayTransactions(const QList<SecApiClient::Transaction>& transactions) {
    m_transactionsModel->clear();
    m_transactionsModel->setHorizontalHeaderLabels({"Date", "Insider", "Transaction", "Shares", "Price"});
    tableChrome::align(m_transactionsModel, 3, true);
    tableChrome::align(m_transactionsModel, 4, true);
    
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
    m_transactionsTable->resizeColumnsToContents();
    fitEmptyColumns(m_transactionsTable);
}

void SecFilingDock::displayFinancials(const QList<SecApiClient::FinancialMetric>& metrics) {
    QString text = "Financial Summary · Latest observation per metric\n\n";
    for (const auto& metric : metrics) {
        QString value = metric.value;
        if (!metric.unit.isEmpty()) {
            value += " " + metric.unit;
        }
        if (!metric.cadence.isEmpty()) {
            value += QString(" [%1 · %2 · %3 · unit: %4]").arg(metric.cadence, metric.period, metric.date,
                metric.unit.isEmpty() ? QStringLiteral("Unknown") : metric.unit);
        }
        text += QString("%1: %2\n").arg(metric.name, value);
    }
    
    m_financialsDisplay->setPlainText(text);
}

void SecFilingDock::updateStatus(const QString& message, bool isError) {
    // An error is one clause in the error role with its glyph; normal status stays dim.
    const QString text = isError ? QString::fromUtf8("× ") + message : message;
    m_statusLabel->setText(text);
    m_statusLabel->setToolTip(text);
    m_statusLabel->setStyleSheet(QStringLiteral("color:%1;")
        .arg((isError ? theme::error() : theme::text3()).name()));
}

void SecFilingDock::clearResults() {
    m_filingsModel->removeRows(0, m_filingsModel->rowCount());
    m_transactionsModel->removeRows(0, m_transactionsModel->rowCount());
    m_financialsDisplay->clear();
    for (auto* table : {m_filingsTable, m_transactionsTable}) fitEmptyColumns(table);
}

void SecFilingDock::fitEmptyColumns(QTableView* table) {
    if (table->model()->rowCount() != 0) return;
    // Empty columns take their header width, so every header fits and the last one stretches.
    for (int column = 0; column < table->model()->columnCount(); ++column)
        table->setColumnWidth(column, table->horizontalHeader()->sectionSizeHint(column));
}

void SecFilingDock::updateEmptyHints() {
    const bool supported = ResearchProcess::isEquityTicker(m_tickerInput->text());
    m_filingsEmpty->setText(supported ? QStringLiteral("No filings loaded · Fetch Filings") : QString());
    m_transactionsEmpty->setText(supported ? QStringLiteral("No insider transactions loaded · Fetch Insider Tx")
                                           : QString());
    m_financialsDisplay->setPlaceholderText(supported ? QStringLiteral("No financial summary loaded · Fetch Financials")
                                                      : QString());
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
    m_tickerHint->setText(supported ? QStringLiteral("Equity tickers only")
                                    : QStringLiteral("! Equity tickers only"));
    m_tickerHint->setStyleSheet(QStringLiteral("color:%1;").arg((supported ? theme::text3() : theme::warn()).name()));
    updateEmptyHints();
    updateStatus(provenance() + (supported ? " · Choose a report" : QString()));
}
void SecFilingDock::onSymbolChanged(const QString& symbol) {
    m_tickerInput->setText(symbol.trimmed().toUpper());
}
