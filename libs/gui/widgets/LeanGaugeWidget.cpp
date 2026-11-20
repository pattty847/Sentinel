#include "LeanGaugeWidget.hpp"
#include "OrderBookDock.hpp"
#include "../config/TooltipHelp.hpp"
#include "../themes/ThemePalette.hpp"
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
    // The strip draws every value; the formula and band meanings need learning mode.
    TooltipHelp::instance().setToolTip(this, {}, "Lean: (bid size − ask size) / (bid size + ask size). Positive = bids (left).\n"
               "Bands are quote-price distances from the native book midpoint, independent of ladder rows:\n"
               "near ±5 and ±15 on the left, far ±50 and ±100 on the right.\n"
               "Depth: cumulative size from mid to ±100. History: ±15 over the last two minutes.\n"
               "Near/far disagree compares ±5 and ±100. One-sided books use the remaining touch.\n"
               "Locked native buckets are valid. Crossed books hold the last values and history, marked stale.\n"
               "Empty books clear history. Hidden below 280 px dock width.");
}

void LeanGaugeWidget::setUnit(const QString& unit)
{
    if (unit == m_unit) return;
    m_unit = unit;
    m_scale = 0;
    updateMetrics();
    update();
}

QString LeanGaugeWidget::caption() const
{
    // The history caption keeps base's width so the history chart keeps its size.
    return QStringLiteral("±15 · 2m");
}

QString LeanGaugeWidget::depthCaption() const
{
    // The distances' unit, stated once, in the depth label's row.
    return m_unit.isEmpty() ? QStringLiteral("Depth ±100") : QStringLiteral("Depth ±100 %1").arg(m_unit);
}

void LeanGaugeWidget::updateMetrics()
{
    const QFontMetrics metrics(font());
    m_rowHeight = std::max(14, metrics.height());
    m_labelWidth = metrics.horizontalAdvance("±100") + 4;
    m_valueWidth = metrics.horizontalAdvance("-100%") + 4;
    m_historyLeft = metrics.horizontalAdvance(caption()) + 6;
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
    // Labels and states in the dim text token (4.5:1 and up, brief DOM-6), not the disabled grey.
    const QColor label = theme::text3();
    for (size_t i = 0; i < m_labels.size(); ++i)
        m_labels[i] = textImage("±" + QString::number(sentinel::book::LeanGauge::Distances[i]), textFont, m_scale, label);
    const std::array<QString, 7> states{"Waiting for book", "", "Bid only", "Ask only", "Invalid book", "Crossed · stale", "Near / far disagree"};
    for (size_t i = 0; i < states.size(); ++i) m_status[i] = textImage(states[i], textFont, m_scale, label);
    m_depthLabel = textImage(depthCaption(), textFont, m_scale, label);
    m_historyLabel = textImage(caption(), textFont, m_scale, label);
}

void LeanGaugeWidget::paintBands(QPainter& p)
{
    const int half = width() / 2;
    for (size_t i = 0; i < 4; ++i) {
        // Near bands in the left column, far in the right: ±5 over ±15 | ±50 over ±100.
        const int x = int(i / 2) * half;
        const int y = int(i % 2) * m_rowHeight;
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
        const QRect area = depthRect();
        const double base = area.bottom(), scale = area.height() - 1;
        QPointF previous(center, base - depth[0] / max * scale);
        for (size_t i = 1; i < depth.size(); ++i) {
            const double x = center + (bid ? -1 : 1) * reach * i / 100;
            const QPointF next(x, base - depth[i] / max * scale);
            p.drawLine(previous, QPointF(x, previous.y()));
            p.drawLine(QPointF(x, previous.y()), next);
            previous = next;
        }
    }
}

void LeanGaugeWidget::paintHistory(QPainter& p)
{
    const QRect area = historyRect();
    const int top = area.top();
    const double center = top + area.height() / 2.0;
    p.drawImage(QPoint(2, top), m_historyLabel);
    p.setPen(m_dim);
    p.drawLine(QPointF(area.left(), center), QPointF(area.left() + area.width(), center));
    if (m_gauge.historySize() < 2) return;
    const auto now = m_gauge.history(m_gauge.historySize() - 1).time;
    const auto point = [&](size_t i) {
        const auto& sample = m_gauge.history(i);
        return QPointF(area.left() + area.width() * (1 - double(now - sample.time) / 120000), center - sample.value * 8);
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
