#include "OrderBookDock.hpp"
#include "ServiceLocator.hpp"
#include "../../core/marketdata/MarketDataCore.hpp"
#include "../../core/marketdata/cache/DataCache.hpp"
#include "../../../libs/core/SentinelLogging.hpp"
#include <QGridLayout>
#include <QFont>
#include <vector>

OrderBookDock::OrderBookDock(QWidget* parent)
    : DockablePanel("orderbook", "Order Book", parent)
{
    sLog_App("OrderBookDock: Constructing order book dock");
}

void OrderBookDock::buildUi()
{
    sLog_App("OrderBookDock: Building UI components");
    
    // Main vertical layout
    auto* mainLayout = new QVBoxLayout(m_contentWidget);
    mainLayout->setContentsMargins(8, 8, 8, 8);
    mainLayout->setSpacing(4);
    
    // Symbol header
    m_symbolLabel = new QLabel("No Symbol", m_contentWidget);
    m_symbolLabel->setAlignment(Qt::AlignCenter);
    m_symbolLabel->setStyleSheet("QLabel { font-weight: bold; font-size: 14px; color: #ffffff; padding: 4px; }");
    mainLayout->addWidget(m_symbolLabel);
    
    // Spread frame container
    setupSpreadLayout();
    mainLayout->addWidget(m_spreadFrame);
    
    // Future: Order book table will go here
    // mainLayout->addWidget(m_orderBookTable, 1);  // Takes remaining space
    
    mainLayout->addStretch();  // Push content to top
    
    // Connect to market data after UI is built
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
    
    // Bid side (left column)
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
    
    // Center column (spread info)
    auto* centerLayout = new QVBoxLayout();
    
    m_spreadLabel = new QLabel("Spread: ---.--", m_spreadFrame);
    m_spreadLabel->setAlignment(Qt::AlignCenter);
    m_spreadLabel->setStyleSheet("QLabel { color: #ffffff; font-size: 10px; }");
    
    m_midLabel = new QLabel("Mid: ---.--", m_spreadFrame);
    m_midLabel->setAlignment(Qt::AlignCenter);
    m_midLabel->setStyleSheet("QLabel { color: #ffeb3b; font-size: 12px; font-weight: bold; }");
    
    centerLayout->addWidget(m_spreadLabel);
    centerLayout->addWidget(m_midLabel);
    
    // Ask side (right column)
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
    
    // Add to grid: bid | center | ask
    gridLayout->addWidget(m_bidFrame, 0, 0);
    gridLayout->addLayout(centerLayout, 0, 1);
    gridLayout->addWidget(m_askFrame, 0, 2);
    
    gridLayout->setColumnStretch(0, 1);  // Equal columns
    gridLayout->setColumnStretch(1, 1);
    gridLayout->setColumnStretch(2, 1);
}

void OrderBookDock::onSymbolChanged(const QString& symbol)
{
    sLog_App(QString("OrderBookDock: Symbol changed to %1").arg(symbol));
    
    m_currentSymbol = symbol;
    if (m_symbolLabel) {
        m_symbolLabel->setText(symbol.isEmpty() ? "No Symbol" : symbol);
    }
    
    // Reset display
    updateSpreadDisplay(0.0, 0.0, 0.0, 0.0);
    
    // TODO: Subscribe to order book updates for new symbol
    // For now, we'll wait for market data core to provide order book signals
}

void OrderBookDock::connectToMarketData()
{
    auto* marketDataCore = ServiceLocator::marketDataCore();
    if (!marketDataCore) {
        sLog_App("OrderBookDock: MarketDataCore not available in ServiceLocator");
        return;
    }
    connect(marketDataCore, &MarketDataCore::liveOrderBookUpdated,
            this, &OrderBookDock::onOrderBookUpdated,
            Qt::QueuedConnection);
    
    sLog_App("OrderBookDock: Connected to MarketDataCore live order book updates");
}

void OrderBookDock::onOrderBookUpdated(const QString& symbol,
                                     const std::vector<BookDelta>& deltas)
{
    Q_UNUSED(deltas);

    if (symbol != m_currentSymbol) {
        return;  // Not our symbol
    }
    
    auto* cache = ServiceLocator::dataCache();
    if (!cache) {
        sLog_App("OrderBookDock: DataCache not available for order book updates");
        return;
    }

    const LiveOrderBook& liveBook = cache->getDirectLiveOrderBook(symbol.toStdString());

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

    sLog_Debug(QString("OrderBookDock: Top of book for %1 - Bid: %2@%3, Ask: %4@%5")
               .arg(symbol).arg(bidPrice).arg(bidSize).arg(askPrice).arg(askSize));
    
    updateSpreadDisplay(bidPrice, bidSize, askPrice, askSize);
}

void OrderBookDock::updateSpreadDisplay(double bidPrice, double bidSize, double askPrice, double askSize)
{
    m_lastBidPrice = bidPrice;
    m_lastBidSize = bidSize;
    m_lastAskPrice = askPrice;
    m_lastAskSize = askSize;
    
    if (bidPrice > 0.0) {
        m_bidPriceLabel->setText(QString::number(bidPrice, 'f', 2));
        m_bidSizeLabel->setText(QString("(%1)").arg(QString::number(bidSize, 'f', 0)));
    } else {
        m_bidPriceLabel->setText("---.--");
        m_bidSizeLabel->setText("(---)");
    }
    
    if (askPrice > 0.0) {
        m_askPriceLabel->setText(QString::number(askPrice, 'f', 2));
        m_askSizeLabel->setText(QString("(%1)").arg(QString::number(askSize, 'f', 0)));
    } else {
        m_askPriceLabel->setText("---.--");
        m_askSizeLabel->setText("(---)");
    }
    
    // Calculate and show spread/mid
    if (bidPrice > 0.0 && askPrice > 0.0) {
        double spread = askPrice - bidPrice;
        double mid = (bidPrice + askPrice) / 2.0;
        
        m_spreadLabel->setText(QString("Spread: %1").arg(QString::number(spread, 'f', 2)));
        m_midLabel->setText(QString("Mid: %1").arg(QString::number(mid, 'f', 2)));
    } else {
        m_spreadLabel->setText("Spread: ---.--");
        m_midLabel->setText("Mid: ---.--");
    }
}