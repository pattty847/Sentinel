#ifndef ORDERBOOKDOCK_HPP
#define ORDERBOOKDOCK_HPP

#include "DockablePanel.hpp"
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFrame>

class MarketDataCore;

/**
 * @brief Order book visualization dock starting with best bid/ask
 * 
 * Current: Shows best bid/ask spread with visual formatting
 * Future: Expandable to full order book with configurable tick aggregation
 * 
 * Design considerations:
 * - Uses ServiceLocator pattern for MarketDataCore access
 * - Thread-safe updates via queued connections
 * - Prepared for order book depth data expansion
 * - Visual design matches trading terminal aesthetics
 */
class OrderBookDock : public DockablePanel {
    Q_OBJECT

public:
    explicit OrderBookDock(QWidget* parent = nullptr);
    ~OrderBookDock() override = default;

    // DockablePanel interface
    void buildUi() override;
    void onSymbolChanged(const QString& symbol) override;

private slots:
    /**
     * @brief Handle order book updates from MarketDataCore
     * @param symbol Trading symbol
     * @param bidPrice Best bid price
     * @param bidSize Best bid size
     * @param askPrice Best ask price  
     * @param askSize Best ask size
     * 
     * Connected via Qt::QueuedConnection for thread safety
     */
    void onOrderBookUpdated(const QString& symbol, double bidPrice, double bidSize, 
                           double askPrice, double askSize);

private:
    void connectToMarketData();
    void updateSpreadDisplay(double bidPrice, double bidSize, double askPrice, double askSize);
    void setupSpreadLayout();
    
    // UI Components - Bid/Ask Spread
    QFrame* m_spreadFrame = nullptr;
    QLabel* m_symbolLabel = nullptr;
    
    // Bid side (left/green)
    QLabel* m_bidPriceLabel = nullptr;
    QLabel* m_bidSizeLabel = nullptr;
    QFrame* m_bidFrame = nullptr;
    
    // Ask side (right/red)  
    QLabel* m_askPriceLabel = nullptr;
    QLabel* m_askSizeLabel = nullptr;
    QFrame* m_askFrame = nullptr;
    
    // Spread info (center)
    QLabel* m_spreadLabel = nullptr;      // Price difference
    QLabel* m_midLabel = nullptr;         // Mid price
    
    // Future expansion placeholders
    // TODO: QTableView* m_orderBookTable = nullptr;  // Full depth
    // TODO: Tick size configuration
    // TODO: Price level aggregation
    
    // State
    QString m_currentSymbol;
    double m_lastBidPrice = 0.0;
    double m_lastBidSize = 0.0;
    double m_lastAskPrice = 0.0;
    double m_lastAskSize = 0.0;
};

#endif // ORDERBOOKDOCK_HPP