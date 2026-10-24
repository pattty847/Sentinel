#pragma once
#include "marketdata/analytics/LeanGauge.hpp"
#include <QImage>
#include <QPen>
#include <QWidget>

class LeanGaugeWidget : public QWidget {
public:
    explicit LeanGaugeWidget(const sentinel::book::LeanGauge& gauge, QWidget* parent = nullptr);
    // Public paint entry for the allocation/pixel test; no data access or computation here.
    void paint(QPainter& painter);
    QSize sizeHint() const override { return {280, 78}; }
protected:
    void paintEvent(QPaintEvent*) override;
    bool event(QEvent* event) override;
private:
    void prepareText();
    void paintBands(QPainter& painter);
    void paintDepth(QPainter& painter);
    void paintHistory(QPainter& painter);
    const sentinel::book::LeanGauge& m_gauge;
    qreal m_scale = 0;
    std::array<QImage, 201> m_percent;
    std::array<QImage, 4> m_labels;
    std::array<QImage, 6> m_status;
    QImage m_historyLabel, m_depthLabel;
    QPen m_bid, m_ask, m_dim;
    QBrush m_track{QColor(38, 43, 51)};
};
