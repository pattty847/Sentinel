#ifndef TRADECHARTWIDGET_H
#define TRADECHARTWIDGET_H

#include <QtWidgets/QWidget>
#include <vector>
#include "tradedata.h"

class TradeChartWidget : public QWidget
{
    Q_OBJECT

public:
    explicit TradeChartWidget(QWidget *parent = nullptr);

public slots:
    void addTrade(const Trade& trade);
    void clearTrades();

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    std::vector<Trade> m_trades;
    // We'll add more data here later, like min/max price, etc.
};

#endif // TRADECHARTWIDGET_H 