#pragma once
#include "marketdata/analytics/LeanGauge.hpp"
#include <QImage>
#include <QPen>
#include <QWidget>
#include <algorithm>

class LeanGaugeWidget : public QWidget {
public:
    explicit LeanGaugeWidget(const sentinel::book::LeanGauge& gauge, QWidget* parent = nullptr);
    // Public paint entry for the allocation/pixel test; no data access or computation here.
    void paint(QPainter& painter);
    QSize sizeHint() const override { return {280, minimumHeight()}; }
    // The quote unit of the band distances ("USD"), shown once in the depth label.
    void setUnit(const QString& unit);
    QString caption() const;
    QString depthCaption() const;
    // Chart areas, which paint uses (test hooks; the strip's layout is held for an owner lab):
    // the mirrored depth polyline (baseline at its bottom, 14 px scale) and the history line.
    QRect depthRect() const { return {0, 2 * m_rowHeight - 1, width(), 15}; }
    QRect historyRect() const { return {m_historyLeft, 3 * m_rowHeight + 14, width() - 3 - m_historyLeft, std::max(16, m_rowHeight)}; }
protected:
    void paintEvent(QPaintEvent*) override;
    bool event(QEvent* event) override;
private:
    void updateMetrics();
    void prepareText();
    void paintBands(QPainter& painter);
    void paintDepth(QPainter& painter);
    void paintHistory(QPainter& painter);
    const sentinel::book::LeanGauge& m_gauge;
    QString m_unit;
    qreal m_scale = 0;
    int m_rowHeight = 16, m_labelWidth = 34, m_valueWidth = 37, m_historyLeft = 55;
    std::array<QImage, 201> m_percent;
    std::array<QImage, 4> m_labels;
    std::array<QImage, 7> m_status;
    QImage m_historyLabel, m_depthLabel;
    QPen m_bid, m_ask, m_dim;
    QBrush m_track{QColor(38, 43, 51)};
};
