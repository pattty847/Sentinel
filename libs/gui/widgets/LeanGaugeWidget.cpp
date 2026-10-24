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
    updateMetrics();
    setToolTip("Lean: (bid size − ask size) / (bid size + ask size). Positive = bids (left).\n"
               "Bands are quote-price distances from the native book midpoint, independent of ladder rows.\n"
               "Depth: cumulative size from mid to ±100. History: ±15 over the last two minutes.\n"
               "Near/far disagree compares ±5 and ±100. One-sided books use the remaining touch.\n"
               "Locked native buckets are valid. Crossed books hold the last values and history, marked stale.\n"
               "Empty books clear history. Hidden below 280 px dock width.");
}

void LeanGaugeWidget::updateMetrics()
{
    const QFontMetrics metrics(font());
    m_rowHeight = std::max(14, metrics.height());
    m_labelWidth = metrics.horizontalAdvance("±100") + 4;
    m_valueWidth = metrics.horizontalAdvance("-100%") + 4;
    m_historyLeft = metrics.horizontalAdvance("±15 · 2m") + 6;
    setFixedHeight(3 * m_rowHeight + 14 + std::max(16, m_rowHeight));
}

void LeanGaugeWidget::prepareText()
{
    if (m_scale > 0) return;
    m_scale = devicePixelRatioF();
    const QFont textFont = font();
    for (int i = -100; i <= 100; ++i) {
        const QString text = (i > 0 ? "+" : "") + QString::number(i) + "%";
        m_percent[i + 100] = textImage(text, textFont, m_scale, DomBarDelegate::sizeTextColor(i >= 0, 1));
    }
    for (size_t i = 0; i < m_labels.size(); ++i)
        m_labels[i] = textImage("±" + QString::number(sentinel::book::LeanGauge::Distances[i]), textFont, m_scale, m_dim.color());
    const std::array<QString, 7> states{"Waiting for book", "", "Bid only", "Ask only", "Invalid book", "Crossed · stale", "Near / far disagree"};
    for (size_t i = 0; i < states.size(); ++i) m_status[i] = textImage(states[i], textFont, m_scale, m_dim.color());
    m_depthLabel = textImage("Depth ±100", textFont, m_scale, m_dim.color());
    m_historyLabel = textImage("±15 · 2m", textFont, m_scale, m_dim.color());
}

void LeanGaugeWidget::paintBands(QPainter& p)
{
    const int half = width() / 2;
    for (size_t i = 0; i < 4; ++i) {
        const int x = int(i % 2) * half;
        const int y = int(i / 2) * m_rowHeight;
        const double value = m_gauge.bands()[i].imbalance;
        const int center = x + m_labelWidth + (half - m_labelWidth - m_valueWidth) / 2;
        const int reach = std::max(0, (half - m_labelWidth - m_valueWidth) / 2);
        const int length = int(std::lround(std::abs(value) * reach));
        p.drawImage(QPoint(x + 2, y), m_labels[i]);
        p.fillRect(QRect(center - reach, y + m_rowHeight / 2 - 2, reach * 2, 4), m_track);
        p.fillRect(QRect(value > 0 ? center - length : center, y + m_rowHeight / 2 - 3, length, 6),
                   value >= 0 ? m_bid.brush() : m_ask.brush());
        p.fillRect(QRect(center, y + m_rowHeight / 2 - 5, 1, 10), m_dim.brush());
        p.drawImage(QPoint(x + half - m_valueWidth, y), m_percent[int(std::lround(value * 100)) + 100]);
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
        QPointF previous(center, (2 * m_rowHeight + 13) - depth[0] / max * 14);
        for (size_t i = 1; i < depth.size(); ++i) {
            const double x = center + (bid ? -1 : 1) * reach * i / 100;
            const QPointF next(x, (2 * m_rowHeight + 13) - depth[i] / max * 14);
            p.drawLine(previous, QPointF(x, previous.y()));
            p.drawLine(QPointF(x, previous.y()), next);
            previous = next;
        }
    }
}

void LeanGaugeWidget::paintHistory(QPainter& p)
{
    const int top = 3 * m_rowHeight + 14;
    const double center = top + std::max(16, m_rowHeight) / 2.0;
    p.drawImage(QPoint(2, top), m_historyLabel);
    p.setPen(m_dim);
    p.drawLine(QPointF(m_historyLeft, center), QPointF(width() - 3, center));
    if (m_gauge.historySize() < 2) return;
    const auto now = m_gauge.history(m_gauge.historySize() - 1).time;
    const auto point = [&](size_t i) {
        const auto& sample = m_gauge.history(i);
        return QPointF(m_historyLeft + (width() - m_historyLeft - 3) * (1 - double(now - sample.time) / 120000), center - sample.value * 8);
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
    const bool stale = m_gauge.state() == sentinel::book::LeanGauge::State::Crossed;
    const size_t state = !stale && m_gauge.disagreement() ? 6 : size_t(m_gauge.state());
    painter.drawImage(QPoint(2, 2 * m_rowHeight + 14), m_status[state]);
    if ((m_status[state].width() + m_depthLabel.width()) / m_scale + 8 < width())
        painter.drawImage(QPointF(width() - m_depthLabel.width() / m_scale - 2, 2 * m_rowHeight + 14), m_depthLabel);
    paintHistory(painter);
}
void LeanGaugeWidget::paintEvent(QPaintEvent*) { QPainter painter(this); paint(painter); }

bool LeanGaugeWidget::event(QEvent* event)
{
    if (event->type() == QEvent::FontChange || event->type() == QEvent::PaletteChange ||
        event->type() == QEvent::DevicePixelRatioChange) {
        m_scale = 0;
        updateMetrics();
        update();
    }
    return QWidget::event(event);
}
