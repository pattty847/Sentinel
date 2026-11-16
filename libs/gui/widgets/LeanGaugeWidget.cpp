#include "LeanGaugeWidget.hpp"
#include "OrderBookDock.hpp"
#include "../themes/ThemePalette.hpp"
#include <QPainter>
#include <QEvent>
#include <QtMath>
#include <cmath>

// Layout (44 px at the 10 px chrome font): two band rows, near bands (±5, ±15) in the left
// column and far bands (±50, ±100) in the right, then one row holding the caption (the unit,
// stated once) or a state, the two minute ±15 history and the mirrored depth to ±100.
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
      m_ask(DomBarDelegate::barColor(false, 1)), m_dim(theme::textOff())
{
    setObjectName("domLeanStrip");
    setAccessibleName("Book imbalance, cumulative depth and two minute history");
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    updateMetrics();
    setToolTip("Lean: (bid size − ask size) / (bid size + ask size). Positive = bids (left).\n"
               "Bands are quote-price distances from the native book midpoint: near ±5 ±15 left, far ±50 ±100 right.\n"
               "Bottom row: ±15 lean over the last two minutes, then cumulative depth from mid to ±100.\n"
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
    // The distances' unit, stated once.
    return m_unit.isEmpty() ? QStringLiteral("±15 · 2m") : QStringLiteral("±15 %1 · 2m").arg(m_unit);
}

void LeanGaugeWidget::updateMetrics()
{
    const QFontMetrics metrics(font());
    m_rowHeight = std::max(14, metrics.height());
    m_labelWidth = metrics.horizontalAdvance("±100") + 4;
    m_valueWidth = metrics.horizontalAdvance("-100%") + 4;
    m_captionWidth = metrics.horizontalAdvance(caption());
    for (const char* state : States) m_captionWidth = std::max(m_captionWidth, metrics.horizontalAdvance(QString::fromUtf8(state)));
    m_captionWidth += 6;
    setFixedHeight(2 * m_rowHeight + bottomHeight());
}

QRect LeanGaugeWidget::captionRect() const { return {0, 2 * m_rowHeight, m_captionWidth, bottomHeight()}; }

QRect LeanGaugeWidget::historyRect() const
{
    const int right = m_captionWidth + (width() - m_captionWidth) * 3 / 5;
    return {m_captionWidth, 2 * m_rowHeight, std::max(0, right - m_captionWidth - 4), bottomHeight()};
}

QRect LeanGaugeWidget::depthRect() const
{
    const QRect history = historyRect();
    const int left = history.right() + 6;
    return {left, 2 * m_rowHeight, std::max(0, width() - 3 - left), bottomHeight()};
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
    for (size_t i = 0; i < States.size(); ++i) m_status[i] = textImage(QString::fromUtf8(States[i]), textFont, m_scale, label);
    m_historyLabel = textImage(caption(), textFont, m_scale, label);
}

void LeanGaugeWidget::paintBands(QPainter& p)
{
    const int half = width() / 2;
    for (size_t i = 0; i < 4; ++i) {
        // Near bands in the left column, far bands in the right: ±5 ±15 | ±50 ±100.
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
    const QRect area = depthRect();
    const double center = area.left() + area.width() / 2.0;
    const double reach = area.width() / 2.0;
    const double max = std::max(m_gauge.bidDepth().back(), m_gauge.askDepth().back());
    if (max <= 0 || reach <= 0) return;
    const double base = area.bottom() - 1, height = area.height() - 3;
    for (bool bid : {true, false}) {
        p.setPen(bid ? m_bid : m_ask);
        const auto& depth = bid ? m_gauge.bidDepth() : m_gauge.askDepth();
        QPointF previous(center, base - depth[0] / max * height);
        for (size_t i = 1; i < depth.size(); ++i) {
            const double x = center + (bid ? -1 : 1) * reach * i / 100;
            const QPointF next(x, base - depth[i] / max * height);
            p.drawLine(previous, QPointF(x, previous.y()));
            p.drawLine(QPointF(x, previous.y()), next);
            previous = next;
        }
    }
}

void LeanGaugeWidget::paintHistory(QPainter& p)
{
    const QRect area = historyRect();
    const double center = area.top() + area.height() / 2.0;
    p.setPen(m_dim);
    p.drawLine(QPointF(area.left(), center), QPointF(area.right(), center));
    if (m_gauge.historySize() < 2 || area.width() <= 0) return;
    const auto now = m_gauge.history(m_gauge.historySize() - 1).time;
    const double amplitude = area.height() / 2.0 - 1;
    const auto point = [&](size_t i) {
        const auto& sample = m_gauge.history(i);
        return QPointF(area.left() + area.width() * (1 - double(now - sample.time) / 120000), center - sample.value * amplitude);
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
    paintHistory(painter);
    // The caption names the history and the unit; an abnormal state takes its place.
    const bool stale = m_gauge.state() == sentinel::book::LeanGauge::State::Crossed;
    const size_t state = !stale && m_gauge.disagreement() ? 6 : size_t(m_gauge.state());
    const QImage& text = state == 1 ? m_historyLabel : m_status[state];
    const QRect caption = captionRect();
    painter.drawImage(QPointF(2, caption.top() + (caption.height() - text.height() / m_scale) / 2), text);
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
