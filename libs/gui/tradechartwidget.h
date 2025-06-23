#ifndef TRADECHARTWIDGET_H
#define TRADECHARTWIDGET_H

#include <QWidget>
#include <vector>
#include "tradedata.h"
#include <QPaintEvent>

class TradeChartWidget : public QWidget {
    Q_OBJECT

public:
    explicit TradeChartWidget(QWidget *parent = nullptr);
    void setSymbol(const std::string& symbol);

public slots:
    void addTrade(const Trade& trade);
    void updateOrderBook(const OrderBook& book);
    void clearTrades();

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    void drawGrid(QPainter* painter);
    void drawAxes(QPainter* painter);
    void drawTrades(QPainter* painter);
    void drawHeatmap(QPainter* painter);
    
    std::string m_symbol;
    std::vector<Trade> m_trades;
    OrderBook m_currentOrderBook;

    double m_minPrice = -1.0;
    double m_maxPrice = -1.0;
    
    // We'll use a circular buffer for historical heatmap data
    // For now, let's just store the latest book
};

#endif // TRADECHARTWIDGET_H