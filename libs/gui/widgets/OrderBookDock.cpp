#include "OrderBookDock.hpp"
#include "ServiceLocator.hpp"
#include "../datasources/IGridDataSource.hpp"
#include <QApplication>
#include <QDateTime>
#include <QHeaderView>
#include <QHideEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QScrollBar>
#include <QShowEvent>
#include <QTableView>
#include <QVBoxLayout>
#include <algorithm>

DomBarDelegate::DomBarDelegate(DomModel* model, QObject* parent)
    : QStyledItemDelegate(parent), m_model(model) {}

void DomBarDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                           const QModelIndex& index) const
{
    // Paint native background/selection, then the liquidity bar, then native text.
    QStyleOptionViewItem opt(option);
    initStyleOption(&opt, index);
    const QString text = opt.text;
    opt.text.clear();
    const auto* style = opt.widget ? opt.widget->style() : QApplication::style();
    style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);
    const bool bid = index.column() == DomModel::Bid;
    const double maximum = bid ? m_model->maxBid() : m_model->maxAsk();
    const double qty = index.data(DomModel::QuantityRole).toDouble();
    if (maximum > 0 && qty > 0) {
        const int width = int(std::min(1.0, qty / maximum) * std::max(0, opt.rect.width() - 8));
        QRect bar = opt.rect.adjusted(4, 3, -4, -3);
        if (bid) bar.setLeft(bar.right() - width + 1);
        else bar.setWidth(width);
        painter->fillRect(bar, bid ? QColor(40, 125, 135, 100) : QColor(160, 115, 40, 100));
    }
    painter->save();
    painter->setFont(opt.font);
    painter->setPen(opt.palette.color(QPalette::Text));
    painter->drawText(opt.rect.adjusted(4, 0, -4, 0), Qt::AlignRight | Qt::AlignVCenter, text);
    painter->restore();
}

OrderBookDock::OrderBookDock(QWidget* parent, IGridDataSource* source)
    : DockablePanel("orderbook", "Order Book", parent), m_source(source ? source : ServiceLocator::dataSource())
{
    buildUi();
    m_timer.setInterval(67); // ~15 Hz; events only update ingestion state/dirty flags.
    m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, &QTimer::timeout, this, &OrderBookDock::refreshDisplay);
    connect(this, &QDockWidget::visibilityChanged, this, &OrderBookDock::setDisplayActive);
    if (!m_source) return;
    connect(m_source, &IGridDataSource::liveOrderBookUpdated, this, &OrderBookDock::onOrderBookUpdated, Qt::QueuedConnection);
    connect(m_source, &IGridDataSource::tradeReceived, this, &OrderBookDock::onTradeReceived, Qt::QueuedConnection);
    connect(m_source, &IGridDataSource::connectionStatusChanged, this, [this](bool connected) {
        m_freshness.connected = connected;
        m_freshness.awaitingBook = true; // A reconnect alone does not freshen retained data.
        m_dirty = true;
    }, Qt::QueuedConnection);
    connect(m_source, &IGridDataSource::bookSnapshotStaleChanged, this, [this](const QString& symbol, bool stale) {
        if (symbol != m_symbol) return;
        m_freshness.snapshotStale = stale;
        m_dirty = true;
    }, Qt::QueuedConnection);
    connect(m_source, &QObject::destroyed, this, [this] {
        m_freshness.connected = false;
        m_dirty = true;
    });
}

void OrderBookDock::buildUi()
{
    if (m_table) return;
    auto* layout = new QVBoxLayout(m_contentWidget);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(4);
    auto* heading = new QHBoxLayout;
    m_symbolLabel = new QLabel("No symbol", m_contentWidget);
    QFont font = m_symbolLabel->font();
    font.setBold(true);
    m_symbolLabel->setFont(font);
    m_symbolLabel->setMinimumWidth(0);
    m_recenter = new QPushButton("Recenter / follow", m_contentWidget);
    m_recenter->setObjectName("domRecenter");
    m_recenter->setToolTip("Follow the market. Scrolling pauses follow and pins the price ladder.");
    connect(m_recenter, &QPushButton::clicked, this, &OrderBookDock::recenter);
    heading->addWidget(m_symbolLabel, 1);
    heading->addWidget(m_recenter);
    layout->addLayout(heading);
    auto label = [&](const char* name) {
        auto* result = new QLabel(m_contentWidget);
        result->setObjectName(name);
        result->setWordWrap(true);
        result->setTextFormat(Qt::PlainText);
        layout->addWidget(result);
        return result;
    };
    m_aggregation = label("domAggregation");
    m_summary = label("domBookSummary");
    m_status = label("domFreshness");
    m_executions = label("domExecutions");
    m_status->setText("Waiting for book");
    m_model = new DomModel(this);
    m_table = new QTableView(m_contentWidget);
    m_table->setObjectName("domLadder");
    m_table->setModel(m_model);
    m_table->setAccessibleName("Order book price ladder");
    m_table->setToolTip("2,001 contiguous price buckets. Scroll to inspect; Recenter / follow moves the window to the market.");
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_table->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_table->verticalHeader()->hide();
    m_table->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    m_table->verticalHeader()->setDefaultSectionSize(22);
    m_table->verticalHeader()->setMinimumSectionSize(22);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_table->horizontalHeader()->setDefaultAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_table->horizontalHeader()->setMinimumSectionSize(72);
    m_table->setColumnWidth(DomModel::Bid, 100);
    m_table->setColumnWidth(DomModel::Price, 124);
    m_table->setColumnWidth(DomModel::Ask, 100);
    for (int c = DomModel::Buys; c < DomModel::Columns; ++c) m_table->setColumnWidth(c, 88);
    auto* delegate = new DomBarDelegate(m_model, m_table);
    m_table->setItemDelegateForColumn(DomModel::Bid, delegate);
    m_table->setItemDelegateForColumn(DomModel::Ask, delegate);
    m_table->viewport()->installEventFilter(this);
    m_table->installEventFilter(this);
    m_table->verticalScrollBar()->installEventFilter(this);
    connect(m_table->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] {
        if (!m_programmaticScroll) stopFollowing();
    });
    layout->addWidget(m_table, 1);
}

void OrderBookDock::onSymbolChanged(const QString& symbol)
{
    if (symbol == m_symbol) return;
    m_symbol = symbol;
    m_trades.clear();
    const auto connected = m_freshness.connected;
    m_freshness = {};
    m_freshness.connected = connected;
    m_dirty = m_symbolDirty = true;
    m_follow = true;
    // Including symbol/model/header changes: no display work until visible.
}

void OrderBookDock::onOrderBookUpdated(const QString& symbol, const std::vector<BookDelta>& deltas)
{
    Q_UNUSED(deltas);
    if (symbol != m_symbol) return;
    // The datasource already applied ALL deltas. Do not rescan it per event.
    m_freshness.awaitingBook = false;
    m_dirty = true;
}

void OrderBookDock::onTradeReceived(const Trade& trade)
{
    if (m_symbol.isEmpty() || trade.product_id != m_symbol.toStdString()) return;
    m_trades.ingest(trade); // Also before the first book, and while hidden.
    m_dirty = true;
}

void OrderBookDock::setDisplayActive(bool active)
{
    m_displayActive = active && isVisible();
    if (m_displayActive) {
        m_dirty = true;
        m_timer.start();
    } else m_timer.stop();
}

void OrderBookDock::showEvent(QShowEvent* event)
{
    DockablePanel::showEvent(event);
    setDisplayActive(true);
}
void OrderBookDock::hideEvent(QHideEvent* event)
{
    setDisplayActive(false);
    DockablePanel::hideEvent(event);
}

void OrderBookDock::stopFollowing()
{
    if (m_programmaticScroll || !m_follow) return;
    m_follow = false;
    m_recenter->setText("Recenter / follow");
}

bool OrderBookDock::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::Wheel ||
        (watched == m_table->verticalScrollBar() && event->type() == QEvent::MouseButtonPress)) stopFollowing();
    if (event->type() == QEvent::KeyPress) {
        const int key = static_cast<QKeyEvent*>(event)->key();
        if (key == Qt::Key_Up || key == Qt::Key_Down || key == Qt::Key_PageUp || key == Qt::Key_PageDown ||
            key == Qt::Key_Home || key == Qt::Key_End) stopFollowing();
    }
    return DockablePanel::eventFilter(watched, event);
}

void OrderBookDock::recenter()
{
    m_follow = true;
    m_dirty = true;
    refreshDisplay();
}

void OrderBookDock::refreshDisplay()
{
    if (!m_displayActive || !isVisible()) return;
    m_programmaticScroll = true;
    const bool needsLayout = m_symbolDirty || m_model->rowCount() == 0;
    if (m_symbolDirty) {
        m_model->clear(m_symbol);
        m_symbolLabel->setText(m_symbol.isEmpty() ? "No symbol" : m_symbol);
        m_symbolDirty = false;
    }
    if (m_source && !m_symbol.isEmpty()) {
        const auto& book = m_source->getDirectLiveOrderBook(m_symbol.toStdString());
        // This is the receive timestamp set by RemoteGridDataSource::applyUpdates.
        if (book.getTickSize() > 0) {
            m_freshness.receiveMs = std::chrono::duration_cast<std::chrono::milliseconds>(book.getLastUpdate().time_since_epoch()).count();
        }
        else m_freshness.awaitingBook = true;
        if (m_dirty) m_model->publish(book, m_trades, m_follow);
    }
    if (m_dirty) {
        m_recenter->setText(m_follow ? "Following · recenter" : "Recenter / follow");
        m_aggregation->setText(m_model->aggregation());
        m_summary->setText(m_model->summary());
        m_executions->setText(m_model->executionSummary());
        // New rows and wrapping header labels must establish the scroll range
        // before the first follow operation; regular ticks reuse that layout.
        if (needsLayout) {
            m_contentWidget->layout()->activate();
            m_table->doItemsLayout();
        }
        if (m_follow && m_model->centerRow() >= 0) m_table->scrollTo(m_model->index(m_model->centerRow(), DomModel::Price), QAbstractItemView::PositionAtCenter);
        m_dirty = false;
    }
    m_status->setText(m_freshness.text(QDateTime::currentMSecsSinceEpoch()));
    m_programmaticScroll = false;
}
