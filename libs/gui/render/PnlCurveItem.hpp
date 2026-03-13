/*
Sentinel — PnlCurveItem
Role: Lightweight QWidget that paints a PnL-over-time line chart.
Threading: Data appended on GUI thread; painted on GUI thread.
*/
#pragma once

#include <QWidget>
#include <QVector>
#include <QPair>
#include <QString>
#include <cstdint>

class PnlCurveItem : public QWidget {
    Q_OBJECT
public:
    enum class Window { All, Hour1, Min5, Min1 };

    explicit PnlCurveItem(QWidget* parent = nullptr);

    void appendPoint(int64_t timestampMs, double totalPnl, const QString& algoId);
    void setWindow(Window w);
    void clear();

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    struct PnlPoint {
        int64_t timestampMs = 0;
        double pnl = 0.0;
        QString algoId;
    };

    QVector<PnlPoint> m_allPoints;
    QVector<PnlPoint> m_algoPoints; // separate series for algo-only
    Window m_window = Window::All;

    QVector<PnlPoint> filteredPoints(const QVector<PnlPoint>& src) const;
    void drawSeries(QPainter& painter, const QVector<PnlPoint>& pts, const QRect& area, const QColor& color);
};
