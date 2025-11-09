#include "MarketDataPanel.hpp"
#include "ServiceLocator.hpp"
#include "../../core/marketdata/MarketDataCore.hpp"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QHeaderView>
#include <QModelIndex>
#include <QRegularExpression>

MarketDataPanel::MarketDataPanel(QWidget* parent)
    : DockablePanel("MarketDataPanel", "Market Data", parent)
{
    buildUi();
    
    // Connect to MarketDataCore signals
    auto* core = ServiceLocator::marketDataCore();
    if (core) {
        connect(core, &MarketDataCore::tradeReceived, this, &MarketDataPanel::onTradeReceived, Qt::QueuedConnection);
    }
}

void MarketDataPanel::buildUi() {
    QVBoxLayout* layout = new QVBoxLayout(m_contentWidget);
    
    // Filter/search box
    QHBoxLayout* filterLayout = new QHBoxLayout();
    filterLayout->addWidget(new QLabel("Filter:", m_contentWidget));
    m_filterEdit = new QLineEdit(m_contentWidget);
    filterLayout->addWidget(m_filterEdit);
    layout->addLayout(filterLayout);
    
    // Table view
    m_model = new MarketDataModel(this);
    m_proxyModel = new QSortFilterProxyModel(this);
    m_proxyModel->setSourceModel(m_model);
    m_proxyModel->setFilterCaseSensitivity(Qt::CaseInsensitive);
    
    m_tableView = new QTableView(m_contentWidget);
    m_tableView->setModel(m_proxyModel);
    m_tableView->setSortingEnabled(true);
    m_tableView->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tableView->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tableView->horizontalHeader()->setStretchLastSection(true);
    m_tableView->verticalHeader()->setVisible(false);
    
    connect(m_tableView, &QTableView::doubleClicked, this, &MarketDataPanel::onSymbolClicked);
    connect(m_filterEdit, &QLineEdit::textChanged, this, [this](const QString& text) {
        m_proxyModel->setFilterRegularExpression(QRegularExpression(text, QRegularExpression::CaseInsensitiveOption));
    });
    
    layout->addWidget(m_tableView);
    
    m_contentWidget->setLayout(layout);
}

void MarketDataPanel::onTradeReceived(const Trade& trade) {
    m_model->updateTrade(trade);
}

void MarketDataPanel::onSymbolClicked(const QModelIndex& index) {
    QModelIndex sourceIndex = m_proxyModel->mapToSource(index);
    if (sourceIndex.isValid()) {
        QString symbol = m_model->data(m_model->index(sourceIndex.row(), 0), Qt::DisplayRole).toString();
        // Emit signal to MainWindowGPU (will be connected)
        // For now, just log
    }
}

void MarketDataPanel::onSymbolChanged(const QString& symbol) {
    // Highlight the row for the active symbol
    // Implementation can be added later
    Q_UNUSED(symbol);
}

