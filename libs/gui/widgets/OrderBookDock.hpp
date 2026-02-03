#ifndef ORDERBOOKDOCK_HPP
#define ORDERBOOKDOCK_HPP

#include "DockablePanel.hpp"
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFrame>
#include <vector>

struct BookDelta;

class OrderBookDock : public DockablePanel {
    Q_OBJECT

public:
    explicit OrderBookDock(QWidget* parent = nullptr);
    ~OrderBookDock() override = default;

    // DockablePanel interface
    void buildUi() override;
    void onSymbolChanged(const QString& symbol) override;

private slots:
    void onOrderBookUpdated(const QString& symbol, const std::vector<BookDelta>& deltas);

private:
    void connectToMarketData();
    void updateSpreadDisplay(double bidPrice, double bidSize, double askPrice, double askSize);
    void setupSpreadLayout();
    
    // UI Components - Bid/Ask Spread
    QFrame* m_spreadFrame = nullptr;
    QLabel* m_symbolLabel = nullptr;
    QLabel* m_bidPriceLabel = nullptr;
    QLabel* m_bidSizeLabel = nullptr;
    QFrame* m_bidFrame = nullptr;
    
    // Ask side (right/red)  
    QLabel* m_askPriceLabel = nullptr;
    QLabel* m_askSizeLabel = nullptr;
    QFrame* m_askFrame = nullptr;
    QLabel* m_spreadLabel = nullptr;
    QLabel* m_midLabel = nullptr;

    QString m_currentSymbol;
    double m_lastBidPrice = 0.0;
    double m_lastBidSize = 0.0;
    double m_lastAskPrice = 0.0;
    double m_lastAskSize = 0.0;
};

#endif // ORDERBOOKDOCK_HPP
