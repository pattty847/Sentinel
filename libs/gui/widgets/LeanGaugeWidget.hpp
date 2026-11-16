#pragma once
#include "marketdata/analytics/LeanGauge.hpp"
#include <QImage>
#include <QPen>
#include <QWidget>
#include <algorithm>
#include <array>

class LeanGaugeWidget : public QWidget {
public:
    explicit LeanGaugeWidget(const sentinel::book::LeanGauge& gauge, QWidget* parent = nullptr);
    // Public paint entry for the allocation/pixel test; no data access or computation here.
    void paint(QPainter& painter);
    QSize sizeHint() const override { return {280, minimumHeight()}; }
    // The quote unit of the band distances ("USD"), shown once in the history caption.
    void setUnit(const QString& unit);
    QString caption() const;
    // Layout of the bottom row (test hooks): the caption or state text, the two minute
    // history and the mirrored depth.
    QRect captionRect() const;
    QRect historyRect() const;
    QRect depthRect() const;
    int rowHeight() const { return m_rowHeight; }
    // State text by LeanGauge::State, then "Near / far disagree"; TwoSided shows the caption.
    static constexpr std::array<const char*, 7> States{"Waiting for book", "", "Bid only", "Ask only", "Invalid book",
                                                       "Crossed · stale", "Near / far disagree"};
protected:
    void paintEvent(QPaintEvent*) override;
    bool event(QEvent* event) override;
private:
    int bottomHeight() const { return std::max(16, m_rowHeight); }
    void updateMetrics();
    void prepareText();
    void paintBands(QPainter& painter);
    void paintDepth(QPainter& painter);
    void paintHistory(QPainter& painter);
    const sentinel::book::LeanGauge& m_gauge;
    QString m_unit;
    qreal m_scale = 0;
    int m_rowHeight = 14, m_labelWidth = 34, m_valueWidth = 37, m_captionWidth = 55;
    std::array<QImage, 201> m_percent;
    std::array<QImage, 4> m_labels;
    std::array<QImage, 7> m_status;
    QImage m_historyLabel;
    QPen m_bid, m_ask, m_dim;
    QBrush m_track{QColor(38, 43, 51)};
};
