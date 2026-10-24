#include "LeanGaugeWidget.hpp"
#include "OrderBookDock.hpp"
#include <QPainter>
#include <QEvent>
#include <QtMath>
#include <cmath>

namespace {
QImage textImage(const QString& text, const QFont& font, qreal scale, const QColor& colour)
{
    const QFontMetrics fm(font);
    QImage image(QSize(qCeil((fm.horizontalAdvance(text) + 2) * scale), qCeil(fm.height() * scale)),
                 QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(scale);
    image.fill(Qt::transparent);
    QPainter p(&image);
    p.setFont(font);
    p.setPen(colour);
    p.drawText(QPoint(0, fm.ascent()), text);
    return image;
}
}

LeanGaugeWidget::LeanGaugeWidget(const sentinel::book::LeanGauge& gauge, QWidget* parent)
    : QWidget(parent), m_gauge(gauge), m_bid(DomBarDelegate::barColor(true, 1)),
      m_ask(DomBarDelegate::barColor(false, 1)), m_dim(QColor(110, 117, 128))
{
    setObjectName("domLeanStrip");
    setAccessibleName("Book imbalance, cumulative depth and two minute history");
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    setFixedHeight(78);
    setToolTip("Lean: (bid size − ask size) / (bid size + ask size). Positive = bids (left).\n"
               "Bands are quote-price distances from the native book midpoint, independent of ladder rows.\n"
               "Depth: cumulative size from mid to ±100. History: ±15 over the last two minutes.\n"
               "Near/far disagree compares ±5 and ±100. One-sided books use the remaining touch.\n"
               "Empty, locked or crossed books clear the history. Hidden below 280 px dock width.");
}

void LeanGaugeWidget::prepareText()
{
    if (m_scale > 0) return;
    m_scale = devicePixelRatioF();
    QFont small = font();
    small.setPixelSize(10);
    for (int i = -100; i <= 100; ++i) {
        const QString text = (i > 0 ? "+" : "") + QString::number(i) + "%";
        m_percent[i + 100] = textImage(text, small, m_scale, DomBarDelegate::sizeTextColor(i >= 0, 1));
    }
    for (size_t i = 0; i < m_labels.size(); ++i)
        m_labels[i] = textImage("±" + QString::number(sentinel::book::LeanGauge::Distances[i]), small, m_scale, m_dim.color());
    const std::array<QString, 6> states{"Waiting for book", "", "Bid only", "Ask only", "Locked / crossed book", "Near / far disagree"};
    for (size_t i = 0; i < states.size(); ++i) m_status[i] = textImage(states[i], small, m_scale, m_dim.color());
    m_depthLabel = textImage("Depth ±100", small, m_scale, m_dim.color());
    m_historyLabel = textImage("±15 · 2m", small, m_scale, m_dim.color());
}

void LeanGaugeWidget::paintBands(QPainter& p)
{
    const int half = width() / 2;
    for (size_t i = 0; i < 4; ++i) {
        const int x = int(i % 2) * half;
        const int y = int(i / 2) * 15;
        const double value = m_gauge.bands()[i].imbalance;
        const int center = x + 34 + (half - 78) / 2;
        const int reach = std::max(0, (half - 78) / 2);
        const int length = int(std::lround(std::abs(value) * reach));
        p.drawImage(QPoint(x + 2, y), m_labels[i]);
        p.fillRect(QRect(center - reach, y + 5, reach * 2, 4), m_track);
        p.fillRect(QRect(value > 0 ? center - length : center, y + 4, length, 6),
                   value >= 0 ? m_bid.brush() : m_ask.brush());
        p.fillRect(QRect(center, y + 2, 1, 10), m_dim.brush());
        p.drawImage(QPoint(x + half - 37, y), m_percent[int(std::lround(value * 100)) + 100]);
    }
}

void LeanGaugeWidget::paintDepth(QPainter& p)
{
    const double center = width() / 2.0;
    const double reach = center - 3;
    const double max = std::max(m_gauge.bidDepth().back(), m_gauge.askDepth().back());
    if (max <= 0) return;
    for (bool bid : {true, false}) {
        p.setPen(bid ? m_bid : m_ask);
        const auto& depth = bid ? m_gauge.bidDepth() : m_gauge.askDepth();
        QPointF previous(center, 47 - depth[0] / max * 14);
        for (size_t i = 1; i < depth.size(); ++i) {
            const double x = center + (bid ? -1 : 1) * reach * i / 100;
            const QPointF next(x, 47 - depth[i] / max * 14);
            p.drawLine(previous, QPointF(x, previous.y()));
            p.drawLine(QPointF(x, previous.y()), next);
            previous = next;
        }
    }
}

void LeanGaugeWidget::paintHistory(QPainter& p)
{
    p.drawImage(QPoint(2, 62), m_historyLabel);
    p.setPen(m_dim);
    p.drawLine(QPointF(55, 66), QPointF(width() - 3, 66));
    if (m_gauge.historySize() < 2) return;
    const auto now = m_gauge.history(m_gauge.historySize() - 1).time;
    const auto point = [&](size_t i) {
        const auto& sample = m_gauge.history(i);
        return QPointF(55 + (width() - 58) * (1 - double(now - sample.time) / 120000), 66 - sample.value * 8);
    };
    for (size_t i = 1; i < m_gauge.historySize(); ++i) {
        p.setPen(m_gauge.history(i).value >= 0 ? m_bid : m_ask);
        p.drawLine(point(i - 1), point(i));
    }
}

void LeanGaugeWidget::paint(QPainter& painter)
{
    prepareText();
    paintBands(painter);
    paintDepth(painter);
    const size_t state = m_gauge.disagreement() ? 5 : size_t(m_gauge.state());
    painter.drawImage(QPoint(2, 48), m_status[state]);
    painter.drawImage(QPointF(width() - m_depthLabel.width() / m_scale - 2, 48), m_depthLabel);
    paintHistory(painter);
}
void LeanGaugeWidget::paintEvent(QPaintEvent*) { QPainter painter(this); paint(painter); }

bool LeanGaugeWidget::event(QEvent* event)
{
    if (event->type() == QEvent::FontChange || event->type() == QEvent::PaletteChange ||
        event->type() == QEvent::DevicePixelRatioChange) {
        m_scale = 0;
        update();
    }
    return QWidget::event(event);
}
