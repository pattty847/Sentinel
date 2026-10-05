#include "OrderBookDock.hpp"
#include "ServiceLocator.hpp"
#include "../datasources/IGridDataSource.hpp"
#include <QApplication>
#include <QDateTime>
#include <QHeaderView>
#include <QHideEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QScrollBar>
#include <QShowEvent>
#include <QTableView>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

DomBarDelegate::DomBarDelegate(DomModel* model, QObject* parent)
    : QStyledItemDelegate(parent), m_model(model) {}

QString DomBarDelegate::fittedQuantity(double value, const QFontMetrics& metrics, int width)
{
    QString full = QString::number(value, 'g', 8);
    if (metrics.horizontalAdvance(full) <= width) return full;
    double scaled = value;
    QString suffix;
    if (std::abs(value) >= 1e9) { scaled /= 1e9; suffix = "b"; }
    else if (std::abs(value) >= 1e6) { scaled /= 1e6; suffix = "m"; }
    else if (std::abs(value) >= 1e3) { scaled /= 1e3; suffix = "k"; }
    for (int precision = 3; precision >= 1; --precision) {
        const auto text = QString::number(scaled, 'g', precision) + suffix;
        if (metrics.horizontalAdvance(text) <= width) return text;
    }
    return QString::number(value, 'e', 1); // Complete number; paint scales only this last fallback.
}

void DomBarDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                           const QModelIndex& index) const
{
    // Paint native background/selection, then the liquidity bar, then native text.
    QStyleOptionViewItem opt(option);
    initStyleOption(&opt, index);
    QString text = opt.text;
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
    const int textWidth = std::max(1, opt.rect.width() - 6);
    if (qty > 0) text = fittedQuantity(qty, opt.fontMetrics, textWidth);
    if (index.column() >= DomModel::Buys && index.data().toInt() == 0) text.clear();
    // Price and count digits stay intact. Only exceptional long values need a smaller font.
    const int measured = opt.fontMetrics.horizontalAdvance(text);
    if (measured > textWidth) opt.font.setPixelSize(std::max(1, int(opt.fontMetrics.height() * 0.8 * textWidth / measured)));
    painter->save();
    painter->setFont(opt.font);
    painter->setPen(opt.palette.color(QPalette::Text));
    painter->drawText(opt.rect.adjusted(3, 0, -3, 0), Qt::AlignRight | Qt::AlignVCenter, text);
    painter->restore();
}

OrderBookDock::OrderBookDock(QWidget* parent, IGridDataSource* source)
    : DockablePanel("orderbook", "Order Book", parent), m_source(source ? source : ServiceLocator::dataSource())
{
    buildUi();
    m_timer.setParent(this);
    m_timer.setObjectName("domPublishTimer");
    m_timer.setInterval(67); // ~15 Hz; events only update ingestion state/dirty flags.
    m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, &QTimer::timeout, this, &OrderBookDock::refreshDisplay);
    connect(this, &QDockWidget::visibilityChanged, this, &OrderBookDock::setDisplayActive);
    installEventFilter(this);
    connect(this, &QDockWidget::topLevelChanged, this, [this] { watchWindow(); updateDisplayTimer(); });
    if (!m_source) return;
    m_freshness.connected = m_source->connectionState();
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
    layout->setContentsMargins(4, 4, 4, 4);
    setMinimumWidth(180);
    layout->setSpacing(2);
    auto* heading = new QHBoxLayout;
    m_symbolLabel = new QLabel("No symbol", m_contentWidget);
    QFont font = m_symbolLabel->font();
    font.setBold(true);
    m_symbolLabel->setFont(font);
    m_symbolLabel->setMinimumWidth(0);
    m_recenter = new QPushButton("Recenter", m_contentWidget);
    m_recenter->setObjectName("domRecenter");
    m_recenter->setAccessibleName("Recenter and follow the market");
    m_recenter->setCheckable(true);
    m_recenter->setToolTip("Follow the market. Scrolling pauses follow and pins the price ladder.");
    connect(m_recenter, &QPushButton::clicked, this, &OrderBookDock::recenter);
    heading->addWidget(m_symbolLabel, 1);
    heading->addWidget(m_recenter);
    layout->addLayout(heading);
    auto label = [&](const char* name) {
        auto* result = new QLabel(m_contentWidget);
        result->setObjectName(name);
        result->setWordWrap(false);
        result->setMinimumWidth(0);
        result->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        result->setTextFormat(Qt::PlainText);
        return result;
    };
    m_aggregation = label("domAggregation");
    m_summary = label("domBookSummary");
    m_status = label("domFreshness");
    layout->addWidget(m_aggregation);
    auto* summaryLine = new QHBoxLayout;
    summaryLine->setSpacing(4);
    summaryLine->addWidget(m_summary, 1);
    summaryLine->addWidget(m_status, 2);
    m_status->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    layout->addLayout(summaryLine);
    m_status->setText("Waiting for book");
    m_model = new DomModel(this);
    m_table = new QTableView(m_contentWidget);
    m_table->setObjectName("domLadder");
    m_table->setModel(m_model);
    m_table->setAccessibleName("Order book price ladder");
    m_table->setToolTip("2,001 contiguous price buckets. Scroll to inspect; Recenter / follow moves the window to the market.");
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_table->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_table->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    m_table->setTextElideMode(Qt::ElideNone);
    m_table->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_table->verticalHeader()->hide();
    m_table->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    m_table->verticalHeader()->setDefaultSectionSize(22);
    m_table->verticalHeader()->setMinimumSectionSize(22);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    m_table->horizontalHeader()->setDefaultAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_table->horizontalHeader()->setMinimumSectionSize(20);
    m_table->setItemDelegate(new DomBarDelegate(m_model, m_table));
    m_table->viewport()->installEventFilter(this);
    m_table->installEventFilter(this);
    m_table->verticalScrollBar()->installEventFilter(this);
    layout->addWidget(m_table, 1);
}

void OrderBookDock::onSymbolChanged(const QString& symbol)
{
    if (symbol == m_symbol) return;
    m_symbol = symbol;
    m_symbolId = symbol.toStdString();
    m_trades.clear();
    const auto connected = m_freshness.connected;
    m_freshness = {};
    m_freshness.connected = m_source ? m_source->connectionState() : connected;
    m_freshness.snapshotStale = m_source && m_source->isBookSnapshotStale(symbol);
    m_dirty = m_symbolDirty = m_bookDirty = true;
    m_follow = true;
    // Including symbol/model/header changes: no display work until visible.
}

void OrderBookDock::onOrderBookUpdated(const QString& symbol, const std::vector<BookDelta>& deltas)
{
    if (symbol != m_symbol) return;
    // The datasource already applied ALL deltas. Do not rescan it per event.
    m_freshness.awaitingBook = deltas.empty();
    m_dirty = m_bookDirty = true;
}

void OrderBookDock::onTradeReceived(const Trade& trade)
{
    if (m_symbol.isEmpty() || trade.product_id != m_symbolId) return;
    m_trades.ingest(trade); // Also before the first book, and while hidden.
    m_dirty = true;
}

void OrderBookDock::setDisplayActive(bool active)
{
    m_exposed = active;
    updateDisplayTimer();
}

bool OrderBookDock::minimized() const
{
    return window()->isMinimized() || (m_hostWindow && m_hostWindow->isMinimized());
}

void OrderBookDock::watchWindow()
{
    QWidget* host = parentWidget() ? parentWidget()->window() : window();
    if (host == m_hostWindow) return;
    if (m_hostWindow && m_hostWindow != this) m_hostWindow->removeEventFilter(this);
    m_hostWindow = host;
    if (host && host != this) host->installEventFilter(this);
}

void OrderBookDock::updateDisplayTimer()
{
    m_displayActive = m_exposed && isVisible() && !minimized();
    if (m_displayActive) {
        m_dirty = true;
        m_timer.start();
    } else m_timer.stop();
}

void OrderBookDock::showEvent(QShowEvent* event)
{
    DockablePanel::showEvent(event);
    watchWindow();
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
    m_recenter->setText("Recenter");
    m_recenter->setChecked(false);
}

bool OrderBookDock::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == this || watched == m_hostWindow) {
        if (event->type() == QEvent::WindowStateChange) updateDisplayTimer();
        return DockablePanel::eventFilter(watched, event);
    }
    if (watched == m_table->viewport() && event->type() == QEvent::Resize && m_displayActive) {
        fitColumns();
        updateSummary();
    }
    const bool ladderInput = watched == m_table || watched == m_table->viewport() || watched == m_table->verticalScrollBar();
    if (!ladderInput) return DockablePanel::eventFilter(watched, event);
    if (event->type() == QEvent::MouseMove && (static_cast<QMouseEvent*>(event)->buttons() & Qt::LeftButton)) stopFollowing();
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
    if (!m_displayActive || !isVisible() || minimized()) return;
    m_programmaticScroll = true;
    const bool needsLayout = m_symbolDirty || m_model->rowCount() == 0;
    if (m_symbolDirty) {
        m_model->clear(m_symbol);
        m_symbolLabel->setText(m_symbol.isEmpty() ? "No symbol" : m_symbol);
        m_symbolDirty = false;
    }
    if (m_source && !m_symbol.isEmpty()) {
        const auto& book = m_source->getDirectLiveOrderBook(m_symbolId);
        // Capture the replica timestamp only after a change notification (or
        // initial symbol hydration). Unchanged L2 messages also touch that
        // timestamp, but must not keep resetting the displayed last-change age.
        if (book.getTickSize() > 0 && m_bookDirty) {
            m_freshness.lastChangeMs = std::chrono::duration_cast<std::chrono::milliseconds>(book.getLastUpdate().time_since_epoch()).count();
        }
        if (book.getTickSize() <= 0) m_freshness.awaitingBook = true;
        m_bookDirty = false;
        if (m_dirty) m_model->publish(book, m_trades, m_follow);
    }
    if (m_dirty) {
        m_recenter->setText(m_follow ? "Following" : "Recenter");
        m_recenter->setChecked(m_follow);
        m_table->horizontalHeader()->setToolTip(m_model->executionSummary());
        fitColumns();
        // New rows must establish the scroll range before the first follow
        // operation; regular ticks reuse that layout.
        if (needsLayout) {
            m_contentWidget->layout()->activate();
            m_table->doItemsLayout();
        }
        if (m_follow && m_model->centerRow() >= 0) m_table->scrollTo(m_model->index(m_model->centerRow(), DomModel::Price), QAbstractItemView::PositionAtCenter);
        m_dirty = false;
    }
    updateSummary();
    m_programmaticScroll = false;
}

void OrderBookDock::fitColumns()
{
    const int width = m_table->viewport()->width();
    const QFontMetrics fm(m_table->font());
    const int priceWidth = std::max(54, fm.horizontalAdvance(m_model->index(0, DomModel::Price).data().toString()) + 8);
    const int sizeWidth = fm.horizontalAdvance("0.0353") + 8;
    const int countWidth = fm.horizontalAdvance("Sell #") + 10;
    const bool counts = width >= priceWidth + sizeWidth * 2 + countWidth * 3;
    const bool delta = width >= priceWidth + sizeWidth * 2 + countWidth;
    for (int col = DomModel::Buys; col < DomModel::Columns; ++col) {
        const bool visible = col == DomModel::Delta ? delta : counts;
        m_table->setColumnHidden(col, !visible);
        if (visible) m_table->setColumnWidth(col, countWidth);
    }
    const int core = width - (counts ? countWidth * 3 : delta ? countWidth : 0);
    const int price = std::min(priceWidth, core / 2);
    const int side = (core - price) / 2;
    m_table->setColumnWidth(DomModel::Bid, side);
    m_table->setColumnWidth(DomModel::Price, price);
    m_table->setColumnWidth(DomModel::Ask, core - price - side);
    m_table->horizontalScrollBar()->setValue(0);
}

void OrderBookDock::updateSummary()
{
    const bool wide = m_contentWidget->width() >= 720;
    const auto fullStatus = m_freshness.text(QDateTime::currentMSecsSinceEpoch());
    QString status;
    if (m_freshness.connected == false) status = "Offline";
    else if (m_freshness.snapshotStale) status = "Stale";
    else if (m_freshness.awaitingBook || m_freshness.lastChangeMs <= 0) status = "Waiting";
    else if (m_freshness.connected == true) status = "Live";
    else status = "Unknown";
    if (m_freshness.lastChangeMs > 0) {
        const double seconds = std::max<qint64>(0, QDateTime::currentMSecsSinceEpoch() - m_freshness.lastChangeMs) / 1000.0;
        const auto age = seconds < 60 ? QString::number(seconds, 'f', 1) + "s"
            : seconds < 3600 ? QString::number(seconds / 60, 'f', 0) + "m"
                             : QString::number(seconds / 3600, 'g', 2) + "h";
        status += QString(" chg %1").arg(age);
    }
    m_status->setText(wide ? fullStatus : status);
    m_status->setToolTip(fullStatus + "\nAge is time since the last book change, not feed silence.");
    m_status->setMaximumWidth(m_status->fontMetrics().horizontalAdvance(m_status->text()) + 2);
    m_aggregation->setText(m_model->aggregation(true));
    m_aggregation->setToolTip(m_model->aggregation() + "\n" + m_model->executionSummary());
    m_summary->setText(m_model->summary(!wide));
    m_summary->setToolTip(m_model->summary() + "\n" + m_model->executionSummary());
}
