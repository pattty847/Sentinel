#include "OrderBookDock.hpp"
#include "ServiceLocator.hpp"
#include "../datasources/IGridDataSource.hpp"
#include "../../core/marketdata/model/TradeData.h"
#include <QGridLayout>
#include <QFont>
#include <QHeaderView>
#include <QPainter>
#include <QScrollBar>
#include <cmath>
#include <vector>

namespace {

int priceDecimalsForTick(double tickSize)
{
    if (tickSize <= 0.0) {
        return 2;
    }

    int decimals = 0;
    double scaledTick = tickSize;
    while (decimals < 8 && std::abs(scaledTick - std::round(scaledTick)) > 1e-9) {
        scaledTick *= 10.0;
        ++decimals;
    }
    return std::max(2, decimals);
}

QString formatBookQty(double qty)
{
    if (qty <= 0.0) {
        return QString();
    }
    if (qty >= 1000.0) {
        return QString::number(qty, 'f', 0);
    }
    if (qty >= 100.0) {
        return QString::number(qty, 'f', 1);
    }
    if (qty >= 1.0) {
        return QString::number(qty, 'f', 2);
    }
    if (qty >= 0.01) {
        return QString::number(qty, 'f', 4);
    }
    return QString::number(qty, 'f', 6);
}

QString formatTopBookQty(double qty)
{
    const QString formatted = formatBookQty(qty);
    return formatted.isEmpty() ? QStringLiteral("---") : formatted;
}

} // namespace

// --- DomBarDelegate ---

DomBarDelegate::DomBarDelegate(BarSide side, QObject* parent)
    : QStyledItemDelegate(parent)
    , m_side(side)
{
}

void DomBarDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                           const QModelIndex& index) const
{
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);

    const QRect r = opt.rect;
    bool ok = false;
    double qty = index.data(Qt::UserRole).toDouble(&ok);
    if (!ok) {
        const QString text = index.data(Qt::DisplayRole).toString();
        qty = text.toDouble(&ok);
    }
    if (!ok || qty <= 0.0 || m_maxQty <= 0.0) {
        QStyledItemDelegate::paint(painter, opt, index);
        return;
    }

    const double ratio = std::min(1.0, qty / m_maxQty);
    const int barWidth = static_cast<int>(ratio * (r.width() - 8));
    if (barWidth <= 0) {
        QStyledItemDelegate::paint(painter, opt, index);
        return;
    }

    QRect barRect;
    if (m_side == BarBid) {
        barRect = QRect(r.right() - barWidth - 4, r.y() + 2, barWidth, r.height() - 4);
    } else {
        barRect = QRect(r.x() + 4, r.y() + 2, barWidth, r.height() - 4);
    }

    painter->save();
    painter->setPen(Qt::NoPen);
    if (m_side == BarBid) {
        painter->setBrush(QColor(26, 77, 26));
    } else {
        painter->setBrush(QColor(77, 26, 26));
    }
    painter->drawRoundedRect(barRect, 2, 2);
    painter->restore();

    opt.rect = r;
    opt.palette.setColor(QPalette::Text, m_side == BarBid ? QColor(0x4c, 0xaf, 0x50) : QColor(0xf4, 0x43, 0x36));
    QStyledItemDelegate::paint(painter, opt, index);
}

// --- OrderBookDock ---

OrderBookDock::OrderBookDock(QWidget* parent)
    : DockablePanel("orderbook", "Order Book", parent)
{
    buildUi();
}

void OrderBookDock::buildUi()
{
    if (m_domTable) {
        return;
    }

    auto* mainLayout = new QVBoxLayout(m_contentWidget);
    mainLayout->setContentsMargins(8, 8, 8, 8);
    mainLayout->setSpacing(4);

    m_symbolLabel = new QLabel("No Symbol", m_contentWidget);
    m_symbolLabel->setAlignment(Qt::AlignCenter);
    m_symbolLabel->setStyleSheet("QLabel { font-weight: bold; font-size: 14px; color: #ffffff; padding: 4px; }");
    mainLayout->addWidget(m_symbolLabel);

    setupSpreadLayout();
    mainLayout->addWidget(m_spreadFrame);

    // DOM table: BIDS | PRICE | ASKS | BUYS | SELLS | DELTA
    m_domTable = new QTableWidget(m_contentWidget);
    m_domTable->setColumnCount(6);
    m_domTable->setHorizontalHeaderLabels(
        { QStringLiteral("BIDS"), QStringLiteral("PRICE"), QStringLiteral("ASKS"),
          QStringLiteral("BUYS"), QStringLiteral("SELLS"), QStringLiteral("DELTA") });
    m_domTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_domTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_domTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_domTable->setShowGrid(true);
    m_domTable->verticalHeader()->setVisible(false);
    m_domTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_domTable->setAlternatingRowColors(false);
    m_domTable->setStyleSheet(
        "QTableWidget { background-color: #1a1a1a; color: #e0e0e0; gridline-color: #2a2a2a; font-size: 12px; }"
        "QHeaderView::section { background-color: #252525; color: #b0b0b0; padding: 6px; font-weight: bold; }");
    m_domTable->setMinimumHeight(320);
    m_domTable->setRowCount(0);

    m_bidBarDelegate = new DomBarDelegate(DomBarDelegate::BarBid, this);
    m_askBarDelegate = new DomBarDelegate(DomBarDelegate::BarAsk, this);
    m_domTable->setItemDelegateForColumn(0, m_bidBarDelegate);
    m_domTable->setItemDelegateForColumn(2, m_askBarDelegate);

    mainLayout->addWidget(m_domTable, 1);
    connectToMarketData();
}

void OrderBookDock::setupSpreadLayout()
{
    m_spreadFrame = new QFrame(m_contentWidget);
    m_spreadFrame->setFrameStyle(QFrame::Box);
    m_spreadFrame->setStyleSheet("QFrame { border: 1px solid #444; background-color: #2a2a2a; }");

    auto* gridLayout = new QGridLayout(m_spreadFrame);
    gridLayout->setContentsMargins(8, 8, 8, 8);
    gridLayout->setSpacing(4);

    m_bidFrame = new QFrame(m_spreadFrame);
    m_bidFrame->setStyleSheet("QFrame { background-color: #1a4d1a; border: 1px solid #2d7d32; border-radius: 4px; padding: 4px; }");
    auto* bidLayout = new QVBoxLayout(m_bidFrame);
    bidLayout->setContentsMargins(4, 4, 4, 4);

    auto* bidHeaderLabel = new QLabel("BID", m_bidFrame);
    bidHeaderLabel->setAlignment(Qt::AlignCenter);
    bidHeaderLabel->setStyleSheet("QLabel { font-weight: bold; color: #4caf50; font-size: 10px; }");

    m_bidPriceLabel = new QLabel("---.--", m_bidFrame);
    m_bidPriceLabel->setAlignment(Qt::AlignCenter);
    m_bidPriceLabel->setStyleSheet("QLabel { font-weight: bold; color: #4caf50; font-size: 16px; }");

    m_bidSizeLabel = new QLabel("(---)", m_bidFrame);
    m_bidSizeLabel->setAlignment(Qt::AlignCenter);
    m_bidSizeLabel->setStyleSheet("QLabel { color: #81c784; font-size: 12px; }");

    bidLayout->addWidget(bidHeaderLabel);
    bidLayout->addWidget(m_bidPriceLabel);
    bidLayout->addWidget(m_bidSizeLabel);

    auto* centerLayout = new QVBoxLayout();

    m_spreadLabel = new QLabel("Spread: ---.--", m_spreadFrame);
    m_spreadLabel->setAlignment(Qt::AlignCenter);
    m_spreadLabel->setStyleSheet("QLabel { color: #ffffff; font-size: 10px; }");

    m_midLabel = new QLabel("Mid: ---.--", m_spreadFrame);
    m_midLabel->setAlignment(Qt::AlignCenter);
    m_midLabel->setStyleSheet("QLabel { color: #ffeb3b; font-size: 12px; font-weight: bold; }");

    centerLayout->addWidget(m_spreadLabel);
    centerLayout->addWidget(m_midLabel);

    m_askFrame = new QFrame(m_spreadFrame);
    m_askFrame->setStyleSheet("QFrame { background-color: #4d1a1a; border: 1px solid #d32f2f; border-radius: 4px; padding: 4px; }");
    auto* askLayout = new QVBoxLayout(m_askFrame);
    askLayout->setContentsMargins(4, 4, 4, 4);

    auto* askHeaderLabel = new QLabel("ASK", m_askFrame);
    askHeaderLabel->setAlignment(Qt::AlignCenter);
    askHeaderLabel->setStyleSheet("QLabel { font-weight: bold; color: #f44336; font-size: 10px; }");

    m_askPriceLabel = new QLabel("---.--", m_askFrame);
    m_askPriceLabel->setAlignment(Qt::AlignCenter);
    m_askPriceLabel->setStyleSheet("QLabel { font-weight: bold; color: #f44336; font-size: 16px; }");

    m_askSizeLabel = new QLabel("(---)", m_askFrame);
    m_askSizeLabel->setAlignment(Qt::AlignCenter);
    m_askSizeLabel->setStyleSheet("QLabel { color: #ef5350; font-size: 12px; }");

    askLayout->addWidget(askHeaderLabel);
    askLayout->addWidget(m_askPriceLabel);
    askLayout->addWidget(m_askSizeLabel);

    gridLayout->addWidget(m_bidFrame, 0, 0);
    gridLayout->addLayout(centerLayout, 0, 1);
    gridLayout->addWidget(m_askFrame, 0, 2);

    gridLayout->setColumnStretch(0, 1);
    gridLayout->setColumnStretch(1, 1);
    gridLayout->setColumnStretch(2, 1);
}

double OrderBookDock::priceToTick(double price) const
{
    if (m_tickSize <= 0.0) return price;
    return std::round(price / m_tickSize) * m_tickSize;
}

int OrderBookDock::priceToRow(double price) const
{
    if (m_tickSize <= 0.0 || price < m_minPrice) return -1;
    return static_cast<int>(std::round((price - m_minPrice) / m_tickSize));
}

void OrderBookDock::refreshDomTable()
{
    if (!m_domTable) return;

    auto* dataSource = ServiceLocator::dataSource();
    if (!dataSource) return;

    const LiveOrderBook& liveBook = dataSource->getDirectLiveOrderBook(m_currentSymbol.toStdString());
    if (liveBook.isEmpty()) {
        m_domTable->setRowCount(0);
        return;
    }

    m_tickSize = liveBook.getTickSize();
    m_minPrice = liveBook.getMinPrice();
    if (m_tickSize <= 0.0) return;

    std::vector<std::pair<uint32_t, double>> bidBuffer;
    std::vector<std::pair<uint32_t, double>> askBuffer;
    auto view = liveBook.captureDenseNonZero(bidBuffer, askBuffer, static_cast<size_t>(kDomLevelsPerSide));

    double bestBid = 0.0;
    double bestAsk = 0.0;
    if (!view.bidLevels.empty()) {
        bestBid = view.minPrice + static_cast<double>(view.bidLevels.front().first) * view.tickSize;
    }
    if (!view.askLevels.empty()) {
        bestAsk = view.minPrice + static_cast<double>(view.askLevels.front().first) * view.tickSize;
    }

    struct DomRow {
        double price = 0.0;
        double bidQty = 0.0;
        double askQty = 0.0;
        bool isMid = false;
    };
    std::vector<DomRow> rows;

    // Asks: display high to low (reverse of askLevels: askLevels[0]=best ask, so iterate backward)
    for (size_t i = view.askLevels.size(); i-- > 0; ) {
        DomRow r;
        r.price = view.minPrice + static_cast<double>(view.askLevels[i].first) * view.tickSize;
        r.askQty = view.askLevels[i].second;
        rows.push_back(r);
    }

    // Mid row (between spread): one row at/near best bid or best ask for the cross
    const double midPrice = (bestBid > 0.0 && bestAsk > 0.0) ? (bestBid + bestAsk) / 2.0 : (bestBid + bestAsk);
    const double midTick = priceToTick(midPrice);
    DomRow midRow;
    midRow.price = midTick;
    midRow.isMid = true;
    for (size_t i = 0; i < view.bidLevels.size(); ++i) {
        double p = view.minPrice + static_cast<double>(view.bidLevels[i].first) * view.tickSize;
        if (std::abs(p - midTick) < view.tickSize * 0.5) {
            midRow.bidQty = view.bidLevels[i].second;
            break;
        }
    }
    for (size_t i = 0; i < view.askLevels.size(); ++i) {
        double p = view.minPrice + static_cast<double>(view.askLevels[i].first) * view.tickSize;
        if (std::abs(p - midTick) < view.tickSize * 0.5) {
            midRow.askQty = view.askLevels[i].second;
            break;
        }
    }
    rows.push_back(midRow);

    // Bids: display best bid first (already high to low from captureDenseNonZero)
    for (size_t i = 0; i < view.bidLevels.size(); ++i) {
        double p = view.minPrice + static_cast<double>(view.bidLevels[i].first) * view.tickSize;
        if (p >= midTick - view.tickSize * 0.5) continue;
        DomRow r;
        r.price = p;
        r.bidQty = view.bidLevels[i].second;
        rows.push_back(r);
    }

    double maxBidQty = 0.0;
    double maxAskQty = 0.0;
    for (const auto& r : rows) {
        if (r.bidQty > maxBidQty) maxBidQty = r.bidQty;
        if (r.askQty > maxAskQty) maxAskQty = r.askQty;
    }
    m_bidBarDelegate->setMaxQty(maxBidQty > 0.0 ? maxBidQty : 1.0);
    m_askBarDelegate->setMaxQty(maxAskQty > 0.0 ? maxAskQty : 1.0);

    m_domTable->setRowCount(static_cast<int>(rows.size()));
    const int decimals = priceDecimalsForTick(m_tickSize);

    for (int row = 0; row < static_cast<int>(rows.size()); ++row) {
        const DomRow& r = rows[static_cast<size_t>(row)];
        const double priceKey = priceToTick(r.price);
        auto it = m_tradeCountsByPrice.find(priceKey);
        const int buys = it != m_tradeCountsByPrice.end() ? it->second.buys : 0;
        const int sells = it != m_tradeCountsByPrice.end() ? it->second.sells : 0;
        const int delta = buys - sells;

        auto* bidItem = new QTableWidgetItem(formatBookQty(r.bidQty));
        auto* priceItem = new QTableWidgetItem(QString::number(r.price, 'f', decimals));
        auto* askItem = new QTableWidgetItem(formatBookQty(r.askQty));
        auto* buysItem = new QTableWidgetItem(QString::number(buys));
        auto* sellsItem = new QTableWidgetItem(QString::number(sells));
        auto* deltaItem = new QTableWidgetItem(QString::number(delta));

        bidItem->setData(Qt::UserRole, r.bidQty);
        askItem->setData(Qt::UserRole, r.askQty);

        if (r.isMid) {
            bidItem->setBackground(QColor(30, 60, 30));
            priceItem->setBackground(QColor(40, 70, 40));
            askItem->setBackground(QColor(60, 30, 30));
            buysItem->setBackground(QColor(30, 60, 30));
            sellsItem->setBackground(QColor(60, 30, 30));
            deltaItem->setBackground(QColor(30, 60, 30));
        }
        bidItem->setForeground(QColor(0x4c, 0xaf, 0x50));
        priceItem->setForeground(QColor(0xff, 0xff, 0xff));
        askItem->setForeground(QColor(0xf4, 0x43, 0x36));
        buysItem->setForeground(QColor(0x4c, 0xaf, 0x50));
        sellsItem->setForeground(QColor(0xf4, 0x43, 0x36));
        deltaItem->setForeground(delta >= 0 ? QColor(0x4c, 0xaf, 0x50) : QColor(0xf4, 0x43, 0x36));

        m_domTable->setItem(row, 0, bidItem);
        m_domTable->setItem(row, 1, priceItem);
        m_domTable->setItem(row, 2, askItem);
        m_domTable->setItem(row, 3, buysItem);
        m_domTable->setItem(row, 4, sellsItem);
        m_domTable->setItem(row, 5, deltaItem);
    }
}

void OrderBookDock::onSymbolChanged(const QString& symbol)
{
    m_currentSymbol = symbol;
    m_tradeCountsByPrice.clear();
    m_tradeHistory.clear();
    if (m_symbolLabel) {
        m_symbolLabel->setText(symbol.isEmpty() ? "No Symbol" : symbol);
    }
    updateSpreadDisplay(0.0, 0.0, 0.0, 0.0);
    refreshDomTable();
}

void OrderBookDock::connectToMarketData()
{
    auto* dataSource = ServiceLocator::dataSource();
    if (!dataSource) return;

    connect(dataSource, &IGridDataSource::liveOrderBookUpdated,
            this, &OrderBookDock::onOrderBookUpdated,
            Qt::QueuedConnection);
    connect(dataSource, &IGridDataSource::tradeReceived,
            this, &OrderBookDock::onTradeReceived,
            Qt::QueuedConnection);
}

void OrderBookDock::onOrderBookUpdated(const QString& symbol,
                                       const std::vector<BookDelta>& deltas)
{
    Q_UNUSED(deltas);

    if (symbol != m_currentSymbol) return;

    auto* dataSource = ServiceLocator::dataSource();
    if (!dataSource) return;

    const LiveOrderBook& liveBook = dataSource->getDirectLiveOrderBook(symbol.toStdString());

    std::vector<std::pair<uint32_t, double>> bidBuffer;
    std::vector<std::pair<uint32_t, double>> askBuffer;
    auto view = liveBook.captureDenseNonZero(bidBuffer, askBuffer, 1);

    double bidPrice = 0.0;
    double bidSize = 0.0;
    double askPrice = 0.0;
    double askSize = 0.0;

    if (!view.bidLevels.empty()) {
        bidPrice = view.minPrice + static_cast<double>(view.bidLevels.front().first) * view.tickSize;
        bidSize = view.bidLevels.front().second;
    }
    if (!view.askLevels.empty()) {
        askPrice = view.minPrice + static_cast<double>(view.askLevels.front().first) * view.tickSize;
        askSize = view.askLevels.front().second;
    }

    updateSpreadDisplay(bidPrice, bidSize, askPrice, askSize);
    refreshDomTable();
}

void OrderBookDock::onTradeReceived(const Trade& trade)
{
    if (m_currentSymbol.isEmpty()) return;
    if (QString::fromStdString(trade.product_id) != m_currentSymbol) return;
    if (m_tickSize <= 0.0) return;

    const double priceKey = priceToTick(trade.price);
    const bool isBuy = (trade.side == AggressorSide::Buy);

    m_tradeHistory.push_back({ priceKey, isBuy });
    if (m_tradeHistory.size() > static_cast<size_t>(kTradeCountCap)) {
        const auto& old = m_tradeHistory.front();
        auto it = m_tradeCountsByPrice.find(old.first);
        if (it != m_tradeCountsByPrice.end()) {
            if (old.second) {
                it->second.buys = std::max(0, it->second.buys - 1);
            } else {
                it->second.sells = std::max(0, it->second.sells - 1);
            }
            if (it->second.buys == 0 && it->second.sells == 0) {
                m_tradeCountsByPrice.erase(it);
            }
        }
        m_tradeHistory.erase(m_tradeHistory.begin());
    }

    PriceCounts& c = m_tradeCountsByPrice[priceKey];
    if (isBuy) {
        ++c.buys;
    } else {
        ++c.sells;
    }

    refreshDomTable();
}

void OrderBookDock::updateSpreadDisplay(double bidPrice, double bidSize, double askPrice, double askSize)
{
    m_lastBidPrice = bidPrice;
    m_lastBidSize = bidSize;
    m_lastAskPrice = askPrice;
    m_lastAskSize = askSize;

    if (!m_bidPriceLabel || !m_bidSizeLabel || !m_askPriceLabel || !m_askSizeLabel ||
        !m_spreadLabel || !m_midLabel) {
        return;
    }

    if (bidPrice > 0.0) {
        m_bidPriceLabel->setText(QString::number(bidPrice, 'f', priceDecimalsForTick(m_tickSize)));
        m_bidSizeLabel->setText(QString("(%1)").arg(formatTopBookQty(bidSize)));
    } else {
        m_bidPriceLabel->setText("---.--");
        m_bidSizeLabel->setText("(---)");
    }

    if (askPrice > 0.0) {
        m_askPriceLabel->setText(QString::number(askPrice, 'f', priceDecimalsForTick(m_tickSize)));
        m_askSizeLabel->setText(QString("(%1)").arg(formatTopBookQty(askSize)));
    } else {
        m_askPriceLabel->setText("---.--");
        m_askSizeLabel->setText("(---)");
    }

    if (bidPrice > 0.0 && askPrice > 0.0) {
        double spread = askPrice - bidPrice;
        double mid = (bidPrice + askPrice) / 2.0;
        m_spreadLabel->setText(QString("Spread: %1").arg(QString::number(spread, 'f', priceDecimalsForTick(m_tickSize))));
        m_midLabel->setText(QString("Mid: %1").arg(QString::number(mid, 'f', priceDecimalsForTick(m_tickSize))));
    } else {
        m_spreadLabel->setText("Spread: ---.--");
        m_midLabel->setText("Mid: ---.--");
    }
}
