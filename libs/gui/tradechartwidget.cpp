#include "tradechartwidget.h"
#include <QPainter>
#include <QDebug>
#include <algorithm>
#include <iomanip>
#include <sstream>

TradeChartWidget::TradeChartWidget(QWidget *parent) : QWidget(parent) {
    // Set a default size
    setMinimumSize(800, 600);
    // Set background to black
    setAutoFillBackground(true);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, Qt::black);
    setPalette(pal);
}

void TradeChartWidget::setSymbol(const std::string& symbol) {
    m_symbol = symbol;
    clearTrades();
}

void TradeChartWidget::addTrade(const Trade& trade) {
    if (trade.product_id != m_symbol) {
        return;
    }
    
    // TODO: Remove this
    // qDebug() << "📊 TradeChartWidget::addTrade received:" << QString::fromStdString(trade.product_id) 
    //          << "$" << trade.price << "size:" << trade.size << "Total trades now:" << (m_trades.size() + 1);
    
    m_trades.push_back(trade);

    // Update min/max prices
    if (m_minPrice < 0 || trade.price < m_minPrice) m_minPrice = trade.price;
    if (m_maxPrice < 0 || trade.price > m_maxPrice) m_maxPrice = trade.price;

    // Prune old trades to keep the display fresh
    const size_t max_trades_to_display = 1000;
    if (m_trades.size() > max_trades_to_display) {
        m_trades.erase(m_trades.begin(), m_trades.begin() + (m_trades.size() - max_trades_to_display));
    }

    // Trigger a repaint
    update();
}

void TradeChartWidget::updateOrderBook(const OrderBook& book) {
    if (book.product_id != m_symbol) {
        return; 
    }
    qDebug() << "Received order book for" << QString::fromStdString(book.product_id) 
             << "with" << book.bids.size() << "bids and" << book.asks.size() << "asks.";
    m_currentOrderBook = book;
    update(); 
}


void TradeChartWidget::clearTrades() {
    m_trades.clear();
    m_minPrice = -1.0;
    m_maxPrice = -1.0;
    update();
}

void TradeChartWidget::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter painter(this);

    if (m_trades.empty()) {
        painter.setPen(Qt::white);
        painter.drawText(rect(), Qt::AlignCenter, "Waiting for trade data...");
        return;
    }
    
    drawGrid(&painter);
    drawAxes(&painter);
    drawHeatmap(&painter);
    drawTrades(&painter);
}

void TradeChartWidget::drawGrid(QPainter* painter) {
    painter->setPen(QColor(40, 40, 40)); 
    int width = this->width();
    int height = this->height();

    for (int i = 0; i < 10; ++i) {
        int y = height / 10 * i;
        painter->drawLine(0, y, width, y);
    }

    for (int i = 0; i < 15; ++i) {
        int x = width / 15 * i;
        painter->drawLine(x, 0, x, height);
    }
}

void TradeChartWidget::drawAxes(QPainter* painter) {
    if (m_minPrice < 0) return;

    painter->setPen(Qt::white);
    int width = this->width();
    int height = this->height();
    
    const int num_price_levels = 10;
    double price_range = m_maxPrice - m_minPrice;
    if (price_range <= 0) price_range = 1.0; 
    
    for (int i = 0; i <= num_price_levels; ++i) {
        double price = m_maxPrice - (price_range / num_price_levels * i);
        int y = (height * i) / num_price_levels;
        
        std::stringstream stream;
        stream << std::fixed << std::setprecision(2) << price;
        std::string price_str = stream.str();
        
        // Give the text more padding from the right edge
        painter->drawText(QRect(width - 70, y - 10, 65, 20), Qt::AlignRight | Qt::AlignVCenter, QString::fromStdString(price_str));
    }
}


void TradeChartWidget::drawTrades(QPainter* painter) {
    if (m_trades.empty()) return;

    int w = width();
    int h = height();
    double price_range = m_maxPrice - m_minPrice;
    if (price_range <= 0) price_range = 1.0;

    QPolygonF priceLine;
    
    // Handle the single trade case to avoid division by zero
    if (m_trades.size() < 2) {
        // Calculate the y position for the single trade
        double y = h - ((m_trades[0].price - m_minPrice) / price_range * h);
        // Place the single dot at the far right, where the latest trade would be
        priceLine.append(QPointF(w, y));
    } else {
        for (size_t i = 0; i < m_trades.size(); ++i) {
            double x = (double)i / (m_trades.size() - 1) * w;
            double y = h - ((m_trades[i].price - m_minPrice) / price_range * h);
            priceLine.append(QPointF(x, y));
        }
    }
    
    // Draw the price line itself
    if (m_trades.size() >= 2) {
        painter->setPen(QPen(Qt::white, 1));
        painter->drawPolyline(priceLine);
    }

    // Draw the aggressor dots
    for (size_t i = 0; i < m_trades.size(); ++i) {
        const QPointF& point = priceLine[i];
        
        if (m_trades[i].side == AggressorSide::Buy) {
            painter->setBrush(Qt::green);
        } else if (m_trades[i].side == AggressorSide::Sell) {
            painter->setBrush(Qt::red);
        } else {
            painter->setBrush(Qt::gray);
        }
        painter->setPen(Qt::NoPen);
        painter->drawEllipse(point, 2, 2);
    }
}

void TradeChartWidget::drawHeatmap(QPainter* painter) {
    if (m_currentOrderBook.bids.empty() && m_currentOrderBook.asks.empty()) {
        return;
    }

    double price_range = m_maxPrice - m_minPrice;
    if (price_range <= 0) return;

    // Find max size to scale opacity
    double max_size = 0;
    for (const auto& level : m_currentOrderBook.bids) {
        if (level.price >= m_minPrice && level.price <= m_maxPrice)
            max_size = std::max(max_size, level.size);
    }
    for (const auto& level : m_currentOrderBook.asks) {
        if (level.price >= m_minPrice && level.price <= m_maxPrice)
            max_size = std::max(max_size, level.size);
    }
    if (max_size == 0) max_size = 1.0;

    painter->setPen(Qt::NoPen);

    // Draw bids (green)
    for (const auto& level : m_currentOrderBook.bids) {
        if (level.price < m_minPrice || level.price > m_maxPrice) continue;
        
        double y = height() - ((level.price - m_minPrice) / price_range * height());
        int alpha = 5 + (level.size / max_size) * 70; // Scale opacity
        painter->setBrush(QColor(0, 255, 0, std::min(alpha, 255)));
        painter->drawRect(0, y - 1, width(), 2);
    }

    // Draw asks (red)
    for (const auto& level : m_currentOrderBook.asks) {
        if (level.price < m_minPrice || level.price > m_maxPrice) continue;

        double y = height() - ((level.price - m_minPrice) / price_range * height());
        int alpha = 5 + (level.size / max_size) * 70; // Scale opacity
        painter->setBrush(QColor(255, 0, 0, std::min(alpha, 255)));
        painter->drawRect(0, y - 1, width(), 2);
    }
}