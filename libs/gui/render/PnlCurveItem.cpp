#include "PnlCurveItem.hpp"

#include <QPainter>
#include <QPaintEvent>
#include <QFontMetrics>
#include <chrono>
#include <cmath>
#include <algorithm>

PnlCurveItem::PnlCurveItem(QWidget* parent)
    : QWidget(parent) {
    setMinimumHeight(80);
    setBackgroundRole(QPalette::Base);
    setAutoFillBackground(false);
}

void PnlCurveItem::appendPoint(int64_t timestampMs, double totalPnl, const QString& algoId) {
    m_allPoints.push_back({timestampMs, totalPnl, ""});
    if (!algoId.isEmpty()) {
        m_algoPoints.push_back({timestampMs, totalPnl, algoId});
    }
    // Trim to 10k points
    if (m_allPoints.size() > 10000) m_allPoints.removeFirst();
    if (m_algoPoints.size() > 10000) m_algoPoints.removeFirst();
    update();
}

void PnlCurveItem::setWindow(Window w) {
    m_window = w;
    update();
}

void PnlCurveItem::clear() {
    m_allPoints.clear();
    m_algoPoints.clear();
    update();
}

QVector<PnlCurveItem::PnlPoint> PnlCurveItem::filteredPoints(const QVector<PnlPoint>& src) const {
    if (m_window == Window::All || src.isEmpty()) {
        return src;
    }
    int64_t nowMs = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    int64_t cutoff = nowMs;
    switch (m_window) {
        case Window::Min1: cutoff = nowMs - 60'000; break;
        case Window::Min5: cutoff = nowMs - 5 * 60'000; break;
        case Window::Hour1: cutoff = nowMs - 60 * 60'000; break;
        default: break;
    }
    QVector<PnlPoint> out;
    for (auto& p : src) {
        if (p.timestampMs >= cutoff) out.push_back(p);
    }
    return out;
}

void PnlCurveItem::drawSeries(QPainter& painter, const QVector<PnlPoint>& pts, const QRect& area, const QColor& color) {
    if (pts.size() < 2) return;

    double minP = pts.first().pnl, maxP = pts.first().pnl;
    for (auto& p : pts) {
        minP = std::min(minP, p.pnl);
        maxP = std::max(maxP, p.pnl);
    }
    // Ensure the zero line is always visible
    minP = std::min(minP, 0.0);
    maxP = std::max(maxP, 0.0);
    const double range = maxP - minP;
    if (range < 1e-9) return;

    const int64_t t0 = pts.first().timestampMs;
    const int64_t t1 = pts.last().timestampMs;
    const double dt = static_cast<double>(t1 - t0);
    if (dt <= 0) return;

    auto ptToScreen = [&](const PnlPoint& p) -> QPointF {
        const double fx = (p.timestampMs - t0) / dt;
        const double fy = 1.0 - (p.pnl - minP) / range;
        return {area.x() + fx * area.width(), area.y() + fy * area.height()};
    };

    // Draw zero line
    const double zeroY = area.y() + (1.0 - (0.0 - minP) / range) * area.height();
    painter.setPen(QPen(QColor(80, 80, 80), 1, Qt::DashLine));
    painter.drawLine(QPointF(area.left(), zeroY), QPointF(area.right(), zeroY));

    // Draw series
    painter.setPen(QPen(color, 1.5));
    QPointF prev = ptToScreen(pts.first());
    for (int i = 1; i < pts.size(); ++i) {
        QPointF cur = ptToScreen(pts[i]);
        painter.drawLine(prev, cur);
        prev = cur;
    }

    // Draw latest value label
    const QPointF last = ptToScreen(pts.last());
    const QString label = QString("%1%2").arg(pts.last().pnl >= 0 ? "+" : "").arg(pts.last().pnl, 0, 'f', 2);
    painter.setPen(color);
    painter.setFont(QFont("Roboto Mono", 8));
    painter.drawText(static_cast<int>(last.x()) + 4, static_cast<int>(last.y()), label);
}

void PnlCurveItem::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), QColor(18, 20, 24));

    const int margin = 4;
    const QRect area = rect().adjusted(margin + 30, margin, -margin, -margin);

    auto allPts = filteredPoints(m_allPoints);
    auto algoPts = filteredPoints(m_algoPoints);

    drawSeries(painter, allPts, area, QColor(80, 200, 120));    // green = total (manual+algo)
    drawSeries(painter, algoPts, area, QColor(0, 180, 255));    // cyan = algo only

    // Y axis labels
    if (!allPts.isEmpty()) {
        double minP = allPts.first().pnl, maxP = allPts.first().pnl;
        for (auto& p : allPts) { minP = std::min(minP, p.pnl); maxP = std::max(maxP, p.pnl); }
        minP = std::min(minP, 0.0); maxP = std::max(maxP, 0.0);
        painter.setPen(QColor(160, 160, 160));
        painter.setFont(QFont("Roboto Mono", 7));
        painter.drawText(margin, area.top() + 10, QString("$%1").arg(maxP, 0, 'f', 2));
        painter.drawText(margin, area.bottom(), QString("$%1").arg(minP, 0, 'f', 2));
    }
}
