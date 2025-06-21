#include "tradechartwidget.h"
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtCore/QDebug>
#include <QtCore/QDateTime> // For formatting time labels
#include <algorithm> // For std::minmax_element
#include <limits>    // For std::numeric_limits

TradeChartWidget::TradeChartWidget(QWidget *parent)
    : QWidget(parent)
{
    // It's good practice to set a minimum size for a custom widget
    setMinimumSize(400, 300);
}

void TradeChartWidget::addTrade(const Trade& trade)
{
    // 🚀 Only pay attention to BTC-USD for now
    if (trade.product_id != "BTC-USD") {
        return;
    }

    m_trades.push_back(trade);
    // Limit the number of trades we store for now to avoid using too much memory
    if (m_trades.size() > 1000) {
        m_trades.erase(m_trades.begin());
    }
    update(); // This schedules a repaint
}

void TradeChartWidget::clearTrades()
{
    m_trades.clear();
    update(); // Repaint to show the empty state
}

void TradeChartWidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    // Fill the background with black
    painter.fillRect(rect(), Qt::black);

    if (m_trades.size() < 2) {
        painter.setPen(Qt::white);
        painter.drawText(rect(), Qt::AlignCenter, "Waiting for trade data...");
        return;
    }

    // --- 1. Find Data Range ---
    auto [min_it, max_it] = std::minmax_element(m_trades.cbegin(), m_trades.cend(), 
        [](const Trade& a, const Trade& b) {
            return a.price < b.price;
        });
    double minPrice = min_it->price;
    double maxPrice = max_it->price;

    auto first_trade_time = m_trades.front().timestamp.time_since_epoch().count();
    auto last_trade_time = m_trades.back().timestamp.time_since_epoch().count();

    // Avoid division by zero if all prices/times are the same
    if (maxPrice == minPrice) maxPrice += 1.0;
    if (last_trade_time == first_trade_time) last_trade_time += 1;

    // --- 2. Define Drawing Area ---
    const int right_margin = 50; // Space for price labels
    const int top_margin = 10;
    const int bottom_margin = 20; // Space for time labels
    const int left_margin = 10;
    int chartWidth = width() - left_margin - right_margin;
    int chartHeight = height() - top_margin - bottom_margin;
    
    // --- 3. Draw Axes and Grid ---
    painter.setPen(QPen(Qt::darkGray, 0.5)); // Faint grid lines

    // Draw horizontal grid lines and price labels
    const int numPriceLines = 5;
    for (int i = 0; i <= numPriceLines; ++i) {
        double price = minPrice + (maxPrice - minPrice) * i / numPriceLines;
        double y = top_margin + chartHeight - (static_cast<double>(i) / numPriceLines * chartHeight);
        painter.drawLine(left_margin, y, left_margin + chartWidth, y);
        painter.drawText(QRect(left_margin + chartWidth + 3, y - 10, right_margin - 3, 20), Qt::AlignVCenter | Qt::AlignLeft, QString::number(price, 'f', 2));
    }

    // Draw vertical grid lines and time labels
    const int numTimeLines = 4;
    for (int i = 0; i <= numTimeLines; ++i) {
        long long time_ns = first_trade_time + (last_trade_time - first_trade_time) * i / numTimeLines;
        double x = left_margin + (static_cast<double>(i) / numTimeLines * chartWidth);
        painter.drawLine(x, top_margin, x, top_margin + chartHeight);
        QDateTime dt = QDateTime::fromMSecsSinceEpoch(time_ns / 1000000);
        painter.drawText(QRect(x - 30, top_margin + chartHeight, 60, bottom_margin), Qt::AlignCenter, dt.toString("HH:mm:ss"));
    }

    // --- 4. Draw Price Line ---
    QPainterPath pricePath;
    
    // Move to the first point
    double x_start = left_margin + static_cast<double>(m_trades[0].timestamp.time_since_epoch().count() - first_trade_time) / (last_trade_time - first_trade_time) * chartWidth;
    double y_start = top_margin + (maxPrice - m_trades[0].price) / (maxPrice - minPrice) * chartHeight;
    pricePath.moveTo(x_start, y_start);

    // Create lines to subsequent points
    for (size_t i = 1; i < m_trades.size(); ++i) {
        double x = left_margin + static_cast<double>(m_trades[i].timestamp.time_since_epoch().count() - first_trade_time) / (last_trade_time - first_trade_time) * chartWidth;
        double y = top_margin + (maxPrice - m_trades[i].price) / (maxPrice - minPrice) * chartHeight;
        pricePath.lineTo(x, y);
    }
    
    painter.setPen(QPen(Qt::white, 1.5));
    painter.drawPath(pricePath);

    // --- 5. Draw Individual Trades ---
    for (const auto& trade : m_trades) {
        double x = left_margin + static_cast<double>(trade.timestamp.time_since_epoch().count() - first_trade_time) / (last_trade_time - first_trade_time) * chartWidth;
        double y = top_margin + (maxPrice - trade.price) / (maxPrice - minPrice) * chartHeight;

        if (trade.side == AggressorSide::Buy) {
            painter.setBrush(Qt::green);
        } else if (trade.side == AggressorSide::Sell) {
            painter.setBrush(Qt::red);
        } else {
            painter.setBrush(Qt::gray);
        }

        // Make the pen transparent so the dots don't have outlines
        painter.setPen(Qt::NoPen);
        
        // Draw a small rectangle for each trade. We can make the size dynamic later.
        painter.drawRect(QRectF(x - 1, y - 1, 3, 3));
    }
} 