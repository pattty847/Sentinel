#include "OrderBookDock.hpp"
#include "LeanGaugeWidget.hpp"
#include "ServiceLocator.hpp"
#include "SentinelLogging.hpp"
#include "../datasources/IGridDataSource.hpp"
#include "../datasources/RemoteGridDataSource.hpp"
#include "../render/heatmap/HeatmapPalette.hpp"
#include <QActionGroup>
#include <QApplication>
#include <QElapsedTimer>
#include <QDateTime>
#include <QHeaderView>
#include <QHideEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QShowEvent>
#include <QToolButton>
#include <QTableView>
#include <QVBoxLayout>
#include <algorithm>
#include <array>
#include <cmath>

namespace {
// Bars use the upper part of each side's palette ramp: its low end is black,
// which would vanish on the dark ladder.
constexpr double RampStart = 0.55;
const std::array<QColor, heatmap::gpu::kPaletteWidth>& paletteRamp()
{
    // The heatmap's default palette image: bids in the left half, asks in the right.
    static const auto ramp = [] {
        const auto texels = heatmap::gpu::paletteTexels(heatmap::gpu::gradientsFor(heatmap::HeatmapChartSettings{}));
        std::array<QColor, heatmap::gpu::kPaletteWidth> out;
        for (int i = 0; i < heatmap::gpu::kPaletteWidth; ++i)
            out[i] = QColor(texels[i * 4], texels[i * 4 + 1], texels[i * 4 + 2]);
        return out;
    }();
    return ramp;
}
} // namespace

DomBarDelegate::DomBarDelegate(DomModel* model, QObject* parent)
    : QStyledItemDelegate(parent), m_model(model) {}

QColor DomBarDelegate::barColor(bool bid, double fraction)
{
    constexpr int half = heatmap::gpu::kPaletteWidth / 2;
    const double position = RampStart + (1 - RampStart) * std::clamp(fraction, 0.0, 1.0);
    return paletteRamp()[(bid ? 0 : half) + int(std::lround(position * (half - 1)))];
}

QColor DomBarDelegate::sizeTextColor(bool bid, double fraction)
{
    const QColor bar = barColor(bid, fraction);
    const auto tint = [](int channel) { return channel + (255 - channel) * 7 / 10; };
    return QColor(tint(bar.red()), tint(bar.green()), tint(bar.blue()),
                  int(std::lround(255 * (0.5 + 0.5 * std::clamp(fraction, 0.0, 1.0)))));
}

QColor DomBarDelegate::bestFill(bool bid) { return barColor(bid, 0.75); }

int DomBarDelegate::dimPrefixLength(const QString& price)
{
    int digits = 0;
    for (int i = price.size() - 1; i >= 0; --i) {
        if (price[i].isDigit() && ++digits == 3) return i;
    }
    return 0;
}

QColor DomBarDelegate::eatenColor(bool bidLevel, double alpha)
{
    // Sellers eat bids (ask colour), buyers eat asks (bid colour).
    QColor color = barColor(!bidLevel, 1);
    color.setAlpha(int(std::lround(150 * std::clamp(alpha, 0.0, 1.0))));
    return color;
}

QColor DomBarDelegate::addedColor(bool bid, double alpha)
{
    QColor color = sizeTextColor(bid, 1);
    color.setAlpha(int(std::lround(235 * std::clamp(alpha, 0.0, 1.0))));
    return color;
}

QColor DomBarDelegate::pulledColor(double alpha)
{
    return QColor(170, 174, 180, int(std::lround(230 * std::clamp(alpha, 0.0, 1.0))));
}

const DomBarDelegate::TagGlyphs& DomBarDelegate::tagGlyphs(const QFont& font, qreal scale) const
{
    if (m_tags.scale == scale && m_tags.font == font) return m_tags;
    static constexpr const char* charset = DomModel::TagAlphabet;
    static_assert(sizeof DomModel::TagAlphabet - 1 <= std::tuple_size_v<decltype(TagGlyphs::offset)>);
    const QFontMetrics metrics(font);
    m_tags.font = font;
    m_tags.scale = scale;
    m_tags.height = metrics.height();
    m_tags.ascent = metrics.ascent();
    m_tags.index.fill(-1);
    int total = 0;
    for (int i = 0; charset[i]; ++i) {
        const QChar c = QLatin1Char(charset[i]);
        m_tags.index[uchar(charset[i])] = int8_t(i);
        m_tags.offset[i] = total;
        m_tags.width[i] = metrics.horizontalAdvance(c);
        total += m_tags.width[i] + 1; // a gap keeps neighbouring glyphs from bleeding
        m_tags.fallback = std::max(m_tags.fallback, m_tags.width[i]);
    }
    const std::array<QColor, TagGlyphs::Colours> colours{sizeTextColor(true, 1), sizeTextColor(false, 1),
        pulledColor(1), barColor(true, 1), barColor(false, 1)};
    for (int k = 0; k < TagGlyphs::Colours; ++k) {
        QImage strip(int(std::ceil(total * scale)), int(std::ceil(m_tags.height * scale)), QImage::Format_ARGB32_Premultiplied);
        strip.setDevicePixelRatio(scale);
        strip.fill(Qt::transparent);
        QPainter glyphs(&strip);
        glyphs.setFont(font);
        QColor colour = colours[k];
        colour.setAlpha(255);
        glyphs.setPen(colour);
        for (int i = 0; charset[i]; ++i)
            glyphs.drawText(QPointF(m_tags.offset[i], m_tags.ascent), QString(QLatin1Char(charset[i])));
        glyphs.end();
        m_tags.strips[k] = std::move(strip);
    }
    return m_tags;
}

int DomBarDelegate::tagWidth(const TagGlyphs& glyphs, const QString& text) const
{
    int width = 0;
    for (const QChar c : text) {
        const int i = c.unicode() < 256 ? glyphs.index[c.unicode()] : -1;
        width += i >= 0 ? glyphs.width[i] : glyphs.fallback;
    }
    return width;
}

void DomBarDelegate::paintPulse(QPainter* painter, const QStyleOptionViewItem& opt, int row, bool bid,
                                const QRect& area, double value, bool cumulative, const QString& label) const
{
    const DomPulseCell pulse = m_model->pulse(row, bid);
    const int span = std::max(0, area.width());
    // A bar extent [inner, outer] in fractions of the cell, drawn from the price side.
    const auto extent = [&](double inner, double outer) {
        const int a = int(std::lround(std::clamp(inner, 0.0, 1.0) * span));
        const int b = int(std::lround(std::clamp(outer, 0.0, 1.0) * span));
        return bid ? QRect(area.right() - b + 1, area.top(), std::max(1, b - a), area.height())
                   : QRect(area.left() + a, area.top(), std::max(1, b - a), area.height());
    };
    if (pulse.change > 0 && pulse.changeAlpha > 0) {
        // Added: a bright segment that grows outward from the old bar end.
        const double inner = m_model->barFraction(std::max(0.0, value - pulse.change), bid, cumulative);
        const double outer = m_model->barFraction(value, bid, cumulative);
        painter->fillRect(extent(inner, inner + (outer - inner) * pulse.grow), addedColor(bid, pulse.changeAlpha));
    } else if (pulse.change < 0 && pulse.changeAlpha > 0) {
        // Pulled: a dashed outline of the removed size beyond the bar.
        const double inner = m_model->barFraction(value, bid, cumulative);
        const double outer = value > 0 ? m_model->barFraction(value - pulse.change, bid, cumulative)
                                       : m_model->barFraction(-pulse.change, bid, false);
        const QRect box = extent(inner, std::max(outer, inner + 2.0 / std::max(1, span)));
        const QColor grey = pulledColor(pulse.changeAlpha);
        for (int x = box.left(); x <= box.right(); x += 5) {
            const int w = std::min(3, box.right() - x + 1);
            painter->fillRect(QRect(x, box.top(), w, 1), grey);
            painter->fillRect(QRect(x, box.bottom(), w, 1), grey);
        }
        painter->fillRect(QRect(bid ? box.left() : box.right(), box.top(), 1, box.height()), grey);
    }
    // One tag per cell, the newer flash; left-aligned, opposite the right-aligned size.
    const bool eaten = pulse.eatenAlpha > 0 && pulse.eatenAlpha >= pulse.changeAlpha;
    const double alpha = eaten ? pulse.eatenAlpha : pulse.changeAlpha;
    if (alpha <= 0) return;
    const QString& tag = m_model->pulseTag(row, bid, eaten);
    if (tag.isEmpty()) return;
    const auto& glyphs = tagGlyphs(opt.font, painter->device()->devicePixelRatioF());
    const int needed = tagWidth(glyphs, tag) + (label.isEmpty() ? 0 : tagWidth(glyphs, label) + 6);
    if (needed > opt.rect.width() - 6) return; // a cramped cell keeps the bar effect only
    // Eaten: the aggressor's colour (buyers eat asks: bid colour); added: the side's tint; pulled: grey.
    const int colour = eaten ? (bid ? 4 : 3) : pulse.change > 0 ? (bid ? 0 : 1) : 2;
    const QImage& strip = glyphs.strips[colour];
    const qreal scale = glyphs.scale;
    qreal x = opt.rect.left() + 3;
    const qreal y = opt.rect.top() + (opt.rect.height() - glyphs.height) / 2;
    painter->setOpacity(alpha);
    for (const QChar c : tag) {
        const int i = c.unicode() < 256 ? glyphs.index[c.unicode()] : -1;
        Q_ASSERT_X(i >= 0, "DomBarDelegate::paintPulse", "tag character missing from DomModel::TagAlphabet");
        if (i < 0) continue;
        const int w = glyphs.width[i];
        painter->drawImage(QRectF(x, y, w, glyphs.height), strip,
                           QRectF(glyphs.offset[i] * scale, 0, w * scale, glyphs.height * scale));
        x += w;
    }
    painter->setOpacity(1);
}

void DomBarDelegate::drawFitted(QPainter* painter, const QStyleOptionViewItem& opt, const QString& text, const QColor& color)
{
    if (text.isEmpty()) return;
    // Exceptional long values only: a smaller font, never a truncated number.
    QFont font = opt.font;
    const int width = std::max(1, opt.rect.width() - 6);
    const int measured = opt.fontMetrics.horizontalAdvance(text);
    if (measured > width) font.setPixelSize(std::max(1, int(opt.fontMetrics.height() * 0.8 * width / measured)));
    painter->setFont(font);
    painter->setPen(color);
    painter->drawText(opt.rect.adjusted(3, 0, -3, 0), Qt::AlignRight | Qt::AlignVCenter, text);
}

void DomBarDelegate::paintSize(QPainter* painter, const QStyleOptionViewItem& opt, const QModelIndex& index, const QString& label) const
{
    const bool bid = index.column() == DomModel::Bid;
    const bool cumulative = m_model->cumulative();
    const int row = index.row();
    const double value = m_model->size(row, bid, cumulative);
    const bool best = m_model->isBest(row, bid);
    const QRect area = opt.rect.adjusted(2, 2, -2, -2);
    const int span = std::max(0, area.width());
    QColor text = sizeTextColor(bid, 0);
    if (best) {
        painter->fillRect(area, bestFill(bid));
        text = QColor(10, 14, 18);
    } else if (value > 0) {
        const double fraction = m_model->barFraction(value, bid, cumulative);
        const int width = int(std::lround(fraction * span));
        QRect bar = area;
        if (bid) bar.setLeft(area.right() - width + 1);
        else bar.setWidth(width);
        QColor fill = barColor(bid, fraction);
        fill.setAlpha(150);
        painter->fillRect(bar, fill);
        text = sizeTextColor(bid, fraction);
    }
    paintPulse(painter, opt, row, bid, area, value, cumulative, label);
    if (!cumulative && !best) {
        // Faint depth line: cumulative size from the touch, on the cumulative scale.
        const double depth = m_model->barFraction(m_model->size(row, bid, true), bid, true);
        if (depth > 0) {
            const int offset = int(std::lround(depth * (span - 1)));
            QColor line = sizeTextColor(bid, depth);
            line.setAlpha(70);
            painter->fillRect(QRect(bid ? area.right() - offset : area.left() + offset, opt.rect.top(), 1, opt.rect.height()), line);
        }
    }
    drawFitted(painter, opt, label, text);
}

void DomBarDelegate::paintPrice(QPainter* painter, const QStyleOptionViewItem& opt, const QString& text) const
{
    // Leading digits dim, the last three bright: the eye reads the changing part.
    const int split = dimPrefixLength(text);
    const QColor bright = opt.palette.color(QPalette::Text);
    QColor dim = bright;
    dim.setAlpha(105);
    const QRect area = opt.rect.adjusted(3, 0, -3, 0);
    // One string drawn twice and clipped: no per-row substrings on the paint path.
    const int tailWidth = opt.fontMetrics.horizontalAdvance(text) - opt.fontMetrics.horizontalAdvance(text, split);
    painter->setFont(opt.font);
    painter->setClipRect(QRect(opt.rect.left(), opt.rect.top(), area.right() - tailWidth + 1 - opt.rect.left(), opt.rect.height()));
    painter->setPen(dim);
    painter->drawText(area, Qt::AlignRight | Qt::AlignVCenter, text);
    painter->setClipRect(QRect(area.right() - tailWidth + 1, opt.rect.top(), tailWidth + 3, opt.rect.height()));
    painter->setPen(bright);
    painter->drawText(area, Qt::AlignRight | Qt::AlignVCenter, text);
}

void DomBarDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                           const QModelIndex& index) const
{
    // Paint native background/selection, then the column's own content.
    QStyleOptionViewItem opt(option);
    initStyleOption(&opt, index);
    const QString text = opt.text;
    opt.text.clear();
    const auto* style = opt.widget ? opt.widget->style() : QApplication::style();
    style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);
    painter->save();
    // Eaten: the whole row flashes in the aggressor's colour while it fades.
    for (const bool bidLevel : {true, false}) {
        const double eaten = m_model->pulse(index.row(), bidLevel).eatenAlpha;
        if (eaten > 0) painter->fillRect(opt.rect, eatenColor(bidLevel, eaten));
    }
    switch (index.column()) {
    case DomModel::Bid:
    case DomModel::Ask: paintSize(painter, opt, index, text); break;
    case DomModel::Price: paintPrice(painter, opt, text); break;
    default: {
        const QVariant foreground = index.data(Qt::ForegroundRole);
        drawFitted(painter, opt, text, foreground.isValid() ? foreground.value<QColor>() : opt.palette.color(QPalette::Text));
    }
    }
    painter->restore();
}

OrderBookDock::OrderBookDock(QWidget* parent, IGridDataSource* source)
    : DockablePanel("orderbook", "Order Book", parent), m_source(source ? source : ServiceLocator::dataSource())
{
    m_pulseTrades.reserve(PulseTradeCapacity);
    m_leanBids.reserve(1);
    m_leanAsks.reserve(1);
    m_groupBids.reserve(1);
    m_groupAsks.reserve(1);
    m_pulseElapsed.start();
    buildUi();
    m_timer.setParent(this);
    m_timer.setObjectName("domPublishTimer");
    m_timer.setInterval(67); // ~15 Hz; events only update ingestion state/dirty flags.
    m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, &QTimer::timeout, this, &OrderBookDock::refreshDisplay);
    connect(&m_timer, &QTimer::timeout, this, &OrderBookDock::refreshLean);
    connect(this, &QDockWidget::visibilityChanged, this, &OrderBookDock::setDisplayActive);
    installEventFilter(this);
    connect(this, &QDockWidget::topLevelChanged, this, [this] { watchWindow(); updateDisplayTimer(); });
    if (!m_source) return;
    if (auto* remote = qobject_cast<RemoteGridDataSource*>(m_source.data())) m_health = remote->marketHealth();
    m_freshness.connected = m_source->connectionState();
    connect(m_source, &IGridDataSource::liveOrderBookUpdated, this, &OrderBookDock::onOrderBookUpdated, Qt::QueuedConnection);
    connect(m_source, &IGridDataSource::tradeReceived, this, &OrderBookDock::onTradeReceived, Qt::QueuedConnection);
    connect(m_source, &IGridDataSource::connectionStatusChanged, this, [this](bool connected) {
        m_freshness.connected = connected;
        m_freshness.awaitingBook = true; // A reconnect alone does not freshen retained data.
        m_dirty = true;
    }, Qt::QueuedConnection);
    connect(m_source, &IGridDataSource::bookSnapshotStaleChanged, this, [this](const QString& symbol, bool stale) {
        if (symbol != m_symbol) return;
        m_freshness.snapshotStale = stale;
        m_dirty = true;
    }, Qt::QueuedConnection);
    connect(m_source, &QObject::destroyed, this, [this] {
        m_freshness.connected = false;
        m_dirty = true;
    });
}

QHBoxLayout* OrderBookDock::buildHeading()
{
    auto* heading = new QHBoxLayout;
    m_symbolLabel = new QLabel("No symbol", m_contentWidget);
    m_symbolLabel->setObjectName("domSymbol");
    QFont font = m_symbolLabel->font();
    font.setBold(true);
    m_symbolLabel->setFont(font);
    m_symbolLabel->setMinimumWidth(0);
    // The symbol yields width so the symbol, Σ and Recenter fit the 180 px minimum dock.
    m_symbolLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_recenter = new QPushButton("Recenter", m_contentWidget);
    m_recenter->setObjectName("domRecenter");
    m_recenter->setAccessibleName("Recenter and follow the market");
    m_recenter->setCheckable(true);
    m_recenter->setToolTip("Follow the market. Scrolling pauses follow and pins the price ladder.");
    connect(m_recenter, &QPushButton::clicked, this, &OrderBookDock::recenter);
    m_cumulative = new QToolButton(m_contentWidget);
    m_cumulative->setText("Σ");
    m_cumulative->setObjectName("domCumulative");
    m_cumulative->setAccessibleName("Cumulative size");
    m_cumulative->setCheckable(true);
    m_cumulative->setToolTip("Cumulative size: Bid and Ask show size summed from the touch outward.\n"
                             "Off: per-row size, with a faint cumulative depth line inside the bars.");
    connect(m_cumulative, &QToolButton::toggled, this, &OrderBookDock::setCumulative);
    m_pulse = new QToolButton(m_contentWidget);
    m_pulse->setText("Pulse");
    m_pulse->setObjectName("domPulse");
    m_pulse->setAccessibleName("Liquidity Pulse");
    m_pulse->setCheckable(true);
    m_pulse->setToolTip("Liquidity Pulse: each book change flashes as what it was.\n"
                        "Bright segment +size: added. Grey dashed outline -size: pulled (cancelled).\n"
                        "Row flash ×size in the aggressor's colour: eaten by trades.\n"
                        "Changes under 5% of the side's largest visible level are not flashed.");
    heading->addWidget(m_symbolLabel, 1);
    heading->addWidget(m_pulse);
    heading->addWidget(m_cumulative);
    heading->addWidget(m_recenter);
    return heading;
}

void OrderBookDock::buildLadder(QVBoxLayout* layout)
{
    m_model = new DomModel(this);
    m_table = new QTableView(m_contentWidget);
    m_table->setObjectName("domLadder");
    m_table->setModel(m_model);
    m_table->setAccessibleName("Order book price ladder");
    m_table->setToolTip("2,001 contiguous price buckets. Scroll to inspect; Recenter / follow moves the window to the market.");
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_table->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_table->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    m_table->setTextElideMode(Qt::ElideNone);
    m_table->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_table->verticalHeader()->hide();
    m_table->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    m_table->verticalHeader()->setDefaultSectionSize(22);
    m_table->verticalHeader()->setMinimumSectionSize(22);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    m_table->horizontalHeader()->setDefaultAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_table->horizontalHeader()->setMinimumSectionSize(20);
    m_table->setItemDelegate(new DomBarDelegate(m_model, m_table));
    m_table->viewport()->installEventFilter(this);
    m_table->installEventFilter(this);
    m_table->verticalScrollBar()->installEventFilter(this);
    layout->addWidget(m_table, 1);
}

void OrderBookDock::buildUi()
{
    if (m_table) return;
    auto* layout = new QVBoxLayout(m_contentWidget);
    layout->setContentsMargins(4, 4, 4, 4);
    setMinimumWidth(180);
    layout->setSpacing(2);
    auto* heading = buildHeading();
    auto* headingWidget = new QWidget(m_contentWidget);
    headingWidget->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    heading->setContentsMargins(0, 0, 0, 0);
    heading->setSizeConstraint(QLayout::SetNoConstraint);
    headingWidget->setLayout(heading);
    layout->addWidget(headingWidget);
    auto label = [&](const char* name) {
        auto* result = new QLabel(m_contentWidget);
        result->setObjectName(name);
        result->setWordWrap(false);
        result->setMinimumWidth(0);
        result->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        result->setTextFormat(Qt::PlainText);
        return result;
    };
    m_aggregation = label("domAggregation");
    m_summary = label("domBookSummary");
    m_status = label("domFreshness");
    layout->addWidget(m_aggregation);
    auto* summaryLine = new QHBoxLayout;
    summaryLine->setSpacing(4);
    summaryLine->addWidget(m_summary, 1);
    summaryLine->addWidget(m_status, 2);
    m_status->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    layout->addLayout(summaryLine);
    m_status->setText("Waiting for book");
    buildLean(heading, layout);
    buildLadder(layout);
    buildGrouping(heading);
    m_pulseFooter = label("domPulseFooter");
    layout->addWidget(m_pulseFooter);
    // Same settings store and format as the other docks (AGENTS 4b: never a native-format QSettings).
    QSettings settings(QSettings::defaultFormat(), QSettings::UserScope, "Sentinel", "SentinelTerminal");
    m_cumulative->setChecked(settings.value("OrderBookDock/cumulative", false).toBool());
    m_pulseOn = settings.value("OrderBookDock/pulse", true).toBool();
    m_pulse->setChecked(m_pulseOn);
    m_pulseFooter->setVisible(m_pulseOn);
    connect(m_pulse, &QToolButton::toggled, this, &OrderBookDock::setPulse);
}

void OrderBookDock::buildLean(QHBoxLayout* heading, QVBoxLayout* layout)
{
    m_lean = new QToolButton(m_contentWidget);
    m_lean->setObjectName("domLean");
    m_lean->setText("Lean");
    m_lean->setAccessibleName("Lean Gauge");
    m_lean->setCheckable(true);
    m_lean->setToolTip("Book imbalance, depth and two minute history; expands at 280 px dock width.");
    heading->insertWidget(2, m_lean);
    m_heading = heading;
    m_leanStrip = new LeanGaugeWidget(m_leanGauge, m_contentWidget);
    layout->addWidget(m_leanStrip);
    QSettings settings(QSettings::defaultFormat(), QSettings::UserScope, "Sentinel", "SentinelTerminal");
    m_lean->setChecked(settings.value("OrderBookDock/lean", true).toBool());
    connect(m_lean, &QToolButton::toggled, this, [this](bool on) {
        QSettings settings(QSettings::defaultFormat(), QSettings::UserScope, "Sentinel", "SentinelTerminal");
        settings.setValue("OrderBookDock/lean", on);
        m_leanGauge.reset();
        updateLeanVisibility();
    });
    updateLeanVisibility();
}

namespace {
// The grouping button opens its menu on click; its arrow would cost heading width.
const char* const GroupButtonStyle = "QToolButton::menu-indicator { image: none; width: 0px; }";
const char* const GroupButtonCompactStyle =
    "QToolButton { padding: 1px; } QToolButton::menu-indicator { image: none; width: 0px; }";
} // namespace

void OrderBookDock::applyHeadingStyle(bool compact)
{
    m_heading->setSpacing(compact ? 0 : -1);
    for (auto* button : {m_pulse, m_lean, m_cumulative})
        button->setStyleSheet(compact ? QStringLiteral("QToolButton { padding: 1px; }") : QString());
    if (m_grouping) m_grouping->setStyleSheet(compact ? GroupButtonCompactStyle : GroupButtonStyle);
    m_recenter->setStyleSheet(compact ? QStringLiteral("QPushButton { padding: 1px; }") : QString());
}

void OrderBookDock::fitHeading()
{
    // Restyling repolishes every button, so it happens only when compact mode flips. The
    // unmodified width is measured again only after a font, style or button-text change.
    if (m_headingNatural < 0) {
        if (m_headingCompact) applyHeadingStyle(false);
        m_headingCompact = false;
        int needed = 5 * m_heading->spacing();
        for (QWidget* button : {static_cast<QWidget*>(m_pulse), static_cast<QWidget*>(m_lean),
                                static_cast<QWidget*>(m_cumulative), static_cast<QWidget*>(m_recenter),
                                static_cast<QWidget*>(m_grouping)}) {
            if (!button) continue;
            button->ensurePolished();
            needed += button->sizeHint().width();
        }
        m_headingNatural = needed;
    }
    const bool compact = m_headingNatural > width() - 8;
    if (compact == m_headingCompact) return;
    m_headingCompact = compact;
    applyHeadingStyle(compact);
}

void OrderBookDock::changeEvent(QEvent* event)
{
    DockablePanel::changeEvent(event);
    if ((event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) && m_heading) {
        m_headingNatural = -1;
        fitHeading();
    }
}

void OrderBookDock::buildGrouping(QHBoxLayout* heading)
{
    m_grouping = new QToolButton(m_contentWidget);
    m_grouping->setObjectName("domGrouping");
    m_grouping->setAccessibleName("Row grouping");
    m_grouping->setPopupMode(QToolButton::InstantPopup);
    m_grouping->setStyleSheet(GroupButtonStyle);
    m_groupMenu = new QMenu(m_grouping);
    m_groupMenu->setObjectName("domGroupingMenu");
    m_grouping->setMenu(m_groupMenu);
    connect(m_groupMenu, &QMenu::aboutToShow, this, &OrderBookDock::fillGroupMenu);
    heading->insertWidget(1, m_grouping);
    QSettings settings(QSettings::defaultFormat(), QSettings::UserScope, "Sentinel", "SentinelTerminal");
    const QString mode = settings.value("OrderBookDock/grouping", "auto").toString();
    m_groupMode = mode == "fixed" ? Grouping::Fixed : mode == "link" ? Grouping::Link : Grouping::Auto;
    m_fixedTick = settings.value("OrderBookDock/groupTick", 0.0).toDouble();
    updateGroupButton();
    m_headingNatural = -1;
    fitHeading();
}

void OrderBookDock::setGrouping(Grouping mode, double fixedTick)
{
    m_groupMode = mode;
    if (mode == Grouping::Fixed) m_fixedTick = fixedTick;
    m_autoHold.reset();
    m_linkHold.reset();
    QSettings settings(QSettings::defaultFormat(), QSettings::UserScope, "Sentinel", "SentinelTerminal");
    settings.setValue("OrderBookDock/grouping", mode == Grouping::Fixed ? "fixed" : mode == Grouping::Link ? "link" : "auto");
    if (mode == Grouping::Fixed) settings.setValue("OrderBookDock/groupTick", fixedTick);
    m_dirty = true;
    if (m_displayActive) refreshDisplay();
    updateGroupButton();
}

void OrderBookDock::setHeatmapTick(double tick, bool manual)
{
    if (tick == m_linkTick && manual == m_linkManual) return;
    m_linkTick = tick;
    m_linkManual = manual;
    if (m_groupMode == Grouping::Link) m_dirty = true;
}

void OrderBookDock::fillGroupMenu()
{
    // Built when the menu opens, never on the publish path.
    m_groupMenu->clear();
    auto* group = new QActionGroup(m_groupMenu);
    const auto add = [&](const QString& text, bool checked, std::function<void()> apply) {
        QAction* action = m_groupMenu->addAction(text);
        action->setCheckable(true);
        action->setChecked(checked);
        group->addAction(action);
        connect(action, &QAction::triggered, this, std::move(apply));
        return action;
    };
    add("Auto", m_groupMode == Grouping::Auto, [this] { setGrouping(Grouping::Auto); })
        ->setToolTip(QStringLiteral("The smallest row size at which %1% of the %2 rows each side of the touch hold size; "
                                    "a new size applies after %3 s.")
                         .arg(int(dom::AutoGroupRule::TargetFill * 100)).arg(dom::AutoGroupRule::RowsPerSide)
                         .arg(dom::GroupHold::HoldMs / 1000.0));
    add("Link to heatmap tick", m_groupMode == Grouping::Link, [this] { setGrouping(Grouping::Link); });
    m_groupMenu->addSeparator();
    const double native = m_groupNative;
    for (const int64_t g : m_choices.list()) {
        const double tick = double(g) * native;
        const QString text = DomModel::priceText(tick, tick) + (g == 1 ? QStringLiteral(" (server tick)") : QString());
        add(text, m_groupMode == Grouping::Fixed && m_model->requestedGroup() == g, [this, tick] { setGrouping(Grouping::Fixed, tick); });
    }
}

void OrderBookDock::updateGrouping(const LiveOrderBook& book)
{
    const double native = book.getTickSize();
    if (!(native > 0)) return;
    const qint64 now = m_pulseClockSource ? m_pulseClockSource() : m_pulseElapsed.elapsed();
    const auto top = book.captureDenseNonZero(m_groupBids, m_groupAsks, 1);
    const double bid = m_groupBids.empty() ? 0 : top.minPrice + m_groupBids.front().first * top.tickSize;
    const double ask = m_groupAsks.empty() ? 0 : top.minPrice + m_groupAsks.front().first * top.tickSize;
    if (native != m_groupNative) { // a new product or server grid: no held size carries over
        m_groupNative = native;
        m_autoHold.reset();
        m_linkHold.reset();
    }
    m_choices = dom::groupChoices(native, bid > 0 && ask > 0 ? (bid + ask) / 2 : bid + ask);
    int64_t group = 1;
    if (m_groupMode == Grouping::Fixed) {
        group = m_choices.nearest(m_fixedTick > 0 ? m_fixedTick / native : 1);
    } else if (m_groupMode == Grouping::Link) {
        const auto link = dom::linkGroup(m_linkTick, native, m_choices);
        m_linkExact = link.exact;
        if (m_linkManual) m_linkHold.reset(); // a locked heatmap tick applies at once
        group = m_linkHold.step(link.group, now);
    } else {
        const dom::AutoBook view{book.getBids(), book.getAsks(), std::llround(top.minPrice / native),
            m_groupBids.empty() ? -1 : std::ptrdiff_t(m_groupBids.front().first),
            m_groupAsks.empty() ? -1 : std::ptrdiff_t(m_groupAsks.front().first)};
        const int64_t target = dom::AutoGroupRule::target(view, m_choices);
        group = m_autoHold.step(target, now);
        sLog_Probe("dom.group", "mode=auto target=" << target << " group=" << group << " changes=" << m_autoHold.changes()
                   << " native=" << native);
    }
    if (group < 1 || !m_choices.contains(group)) group = m_choices.nearest(double(std::max<int64_t>(group, 1)));
    m_model->setGroup(group);
    updateGroupButton();
}

void OrderBookDock::updateGroupButton()
{
    // Text and tooltip change only with the size or mode (no per-tick string work).
    const int64_t group = m_model->requestedGroup();
    if (group == m_shownGroup && m_groupMode == m_shownMode && m_linkExact == m_shownExact && m_groupNative > 0) return;
    m_shownGroup = group;
    m_shownMode = m_groupMode;
    m_shownExact = m_linkExact;
    const double tick = double(group) * m_groupNative;
    const QString size = m_groupNative > 0 ? DomModel::priceText(tick, tick) : QStringLiteral("—");
    const QString prefix = m_groupMode == Grouping::Auto ? QStringLiteral("A ") : m_groupMode == Grouping::Link ? QStringLiteral("L ") : QString();
    m_grouping->setText(prefix + size);
    QString tip = QStringLiteral("Row size %1 (%2 server ticks). ").arg(size).arg(group);
    if (m_groupMode == Grouping::Auto)
        tip += QStringLiteral("Auto: the smallest size at which %1% of the %2 rows each side of the touch hold size; "
                              "changes after %3 s.").arg(int(dom::AutoGroupRule::TargetFill * 100))
                   .arg(dom::AutoGroupRule::RowsPerSide).arg(dom::GroupHold::HoldMs / 1000.0);
    else if (m_groupMode == Grouping::Link)
        tip += m_linkExact ? QStringLiteral("Linked to the heatmap tick.")
                           : QStringLiteral("Linked: the heatmap tick %1 is not a whole multiple of the server tick "
                                            "(heatmap layer off?); nearest size used.")
                                 .arg(m_linkTick > 0 ? QString::number(m_linkTick, 'g', 12) : QStringLiteral("(none)"));
    else tip += QStringLiteral("Fixed.");
    m_grouping->setToolTip(tip + QStringLiteral("\nClick to choose Auto, Link to heatmap, or a fixed size. "
                                                "The spread above is the server's, whatever the row size."));
    if (group > 1 || m_groupNative > 0) sLog_App("DOM grouping mode=" << int(m_groupMode) << " group=" << group << " size=" << size);
    m_headingNatural = -1;
    fitHeading();
}

void OrderBookDock::updateLeanVisibility()
{
    fitHeading();
    const bool visible = m_lean->isChecked() && width() >= 280;
    m_leanStrip->setVisible(visible);
    if (!visible) m_leanGauge.reset();
}

void OrderBookDock::refreshLean()
{
    if (!m_displayActive || minimized() || !isVisible()) return;
    if (!m_source || m_symbol.isEmpty()) return;
    if (m_lean->isChecked() && !m_leanStrip->isHidden())
        stepLean(m_source->getDirectLiveOrderBook(m_symbolId));
}

void OrderBookDock::stepLean(const LiveOrderBook& book)
{
    const auto top = book.captureDenseNonZero(m_leanBids, m_leanAsks, 1);
    const sentinel::book::LeanBook view{book.getBids(), book.getAsks(), top.minPrice, top.tickSize,
        m_leanBids.empty() ? -1 : std::ptrdiff_t(m_leanBids.front().first),
        m_leanAsks.empty() ? -1 : std::ptrdiff_t(m_leanAsks.front().first)};
    m_leanGauge.update(view, m_leanGeneration, m_pulseElapsed.elapsed());
    ++m_leanSteps;
    m_leanStrip->update();
}

void OrderBookDock::setPulse(bool on)
{
    m_pulseOn = on;
    m_pulseFooter->setVisible(on);
    QSettings settings(QSettings::defaultFormat(), QSettings::UserScope, "Sentinel", "SentinelTerminal");
    settings.setValue("OrderBookDock/pulse", on);
    updatePulseActive();
}

void OrderBookDock::updatePulseActive()
{
    // Hidden or off: no classification; any return starts from a fresh baseline.
    const bool active = m_displayActive && m_pulseOn;
    if (active == m_pulseActive) return;
    m_pulseActive = active;
    m_pulseTrades.clear();
    m_model->resetPulse();
    m_footerVersion = UINT64_MAX;
    m_dirty = true;
}

qint64 OrderBookDock::pulseClock(bool snapshot)
{
    // Strictly after the last snapshot: a trade received in the same millisecond
    // as a snapshot belongs to the next one, and snapshots strictly increase.
    const qint64 now = std::max(m_pulseClockSource ? m_pulseClockSource() : m_pulseElapsed.elapsed(), m_lastPulseMs + 1);
    if (snapshot) m_lastPulseMs = now;
    return now;
}

void OrderBookDock::setCumulative(bool on)
{
    m_model->setCumulative(on);
    QSettings settings(QSettings::defaultFormat(), QSettings::UserScope, "Sentinel", "SentinelTerminal");
    settings.setValue("OrderBookDock/cumulative", on);
}

void OrderBookDock::updateVisibleRows()
{
    // Bars scale to what the viewport shows, never to the whole 2,001-row window.
    const int first = m_table->rowAt(0);
    const int last = m_table->rowAt(m_table->viewport()->height() - 1);
    m_model->setVisibleRows(first, last < 0 && first >= 0 ? m_model->rowCount() - 1 : last);
}

void OrderBookDock::onSymbolChanged(const QString& symbol)
{
    if (symbol == m_symbol) return;
    m_symbol = symbol;
    m_symbolId = symbol.toStdString();
    ++m_leanGeneration;
    m_leanGauge.reset();
    m_leanStrip->update();
    m_trades.clear();
    m_pulseTrades.clear();
    const auto connected = m_freshness.connected;
    m_freshness = {};
    m_freshness.connected = m_source ? m_source->connectionState() : connected;
    m_freshness.snapshotStale = m_source && m_source->isBookSnapshotStale(symbol);
    m_dirty = m_symbolDirty = m_bookDirty = true;
    m_follow = true;
    // Including symbol/model/header changes: no display work until visible.
}

void OrderBookDock::onOrderBookUpdated(const QString& symbol, const std::vector<BookDelta>& deltas)
{
    if (symbol != m_symbol) return;
    // The datasource already applied ALL deltas. Do not rescan it per event.
    m_freshness.awaitingBook = deltas.empty();
    if (deltas.empty()) { m_leanGauge.reset(); m_leanStrip->update(); }
    m_dirty = m_bookDirty = true;
}

void OrderBookDock::onTradeReceived(const Trade& trade)
{
    if (m_symbol.isEmpty() || trade.product_id != m_symbolId) return;
    m_trades.ingest(trade); // Also before the first book, and while hidden.
    if (m_pulseActive) {
        if (m_pulseTrades.size() < m_pulseTrades.capacity()) {
            const auto aggressor = trade.side == AggressorSide::Buy ? sentinel::book::Aggressor::Buy
                : trade.side == AggressorSide::Sell ? sentinel::book::Aggressor::Sell : sentinel::book::Aggressor::Unknown;
            m_pulseTrades.push_back({trade.price, trade.size, aggressor, pulseClock(false)});
        } else if (!m_pulseOverflowWarned) {
            m_pulseOverflowWarned = true;
            sLog_Warning("DOM pulse trade batch full capacity=" << PulseTradeCapacity << " symbol=" << m_symbol
                         << ": later trades in this tick are not matched");
        }
    }
    m_dirty = true;
}

void OrderBookDock::setDisplayActive(bool active)
{
    m_exposed = active;
    updateDisplayTimer();
}

bool OrderBookDock::minimized() const
{
    return window()->isMinimized() || (m_hostWindow && m_hostWindow->isMinimized());
}

void OrderBookDock::watchWindow()
{
    QWidget* host = parentWidget() ? parentWidget()->window() : window();
    if (host == m_hostWindow) return;
    if (m_hostWindow && m_hostWindow != this) m_hostWindow->removeEventFilter(this);
    m_hostWindow = host;
    if (host && host != this) host->installEventFilter(this);
}

void OrderBookDock::updateDisplayTimer()
{
    m_displayActive = m_exposed && isVisible() && !minimized();
    if (m_displayActive) {
        m_dirty = true;
        m_timer.start();
    } else m_timer.stop();
    updatePulseActive();
    if (!m_displayActive) m_leanGauge.reset();
}

void OrderBookDock::showEvent(QShowEvent* event)
{
    DockablePanel::showEvent(event);
    watchWindow();
    setDisplayActive(true);
}
void OrderBookDock::hideEvent(QHideEvent* event)
{
    setDisplayActive(false);
    DockablePanel::hideEvent(event);
}

void OrderBookDock::resizeEvent(QResizeEvent* event)
{
    DockablePanel::resizeEvent(event);
    if (m_leanStrip) updateLeanVisibility();
}

void OrderBookDock::stopFollowing()
{
    if (m_programmaticScroll || !m_follow) return;
    m_follow = false;
    m_recenter->setText("Recenter");
    m_recenter->setChecked(false);
}

bool OrderBookDock::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == this || watched == m_hostWindow) {
        if (event->type() == QEvent::WindowStateChange) updateDisplayTimer();
        return DockablePanel::eventFilter(watched, event);
    }
    if (watched == m_table->viewport() && event->type() == QEvent::Resize && m_displayActive) {
        fitColumns();
        updateSummary();
    }
    const bool ladderInput = watched == m_table || watched == m_table->viewport() || watched == m_table->verticalScrollBar();
    if (!ladderInput) return DockablePanel::eventFilter(watched, event);
    if (event->type() == QEvent::MouseMove && (static_cast<QMouseEvent*>(event)->buttons() & Qt::LeftButton)) stopFollowing();
    if (event->type() == QEvent::Wheel ||
        (watched == m_table->verticalScrollBar() && event->type() == QEvent::MouseButtonPress)) stopFollowing();
    if (event->type() == QEvent::KeyPress) {
        const int key = static_cast<QKeyEvent*>(event)->key();
        if (key == Qt::Key_Up || key == Qt::Key_Down || key == Qt::Key_PageUp || key == Qt::Key_PageDown ||
            key == Qt::Key_Home || key == Qt::Key_End) stopFollowing();
    }
    return DockablePanel::eventFilter(watched, event);
}

void OrderBookDock::recenter()
{
    m_follow = true;
    m_dirty = true;
    refreshDisplay();
}

void OrderBookDock::refreshBook()
{
    if (!m_source || m_symbol.isEmpty()) return;
    const auto& book = m_source->getDirectLiveOrderBook(m_symbolId);
    // Capture the replica timestamp only after a change notification (or
    // initial symbol hydration). Unchanged L2 messages also touch that
    // timestamp, but must not keep resetting the displayed last-change age.
    if (book.getTickSize() > 0 && m_bookDirty) {
        m_freshness.lastChangeMs = std::chrono::duration_cast<std::chrono::milliseconds>(book.getLastUpdate().time_since_epoch()).count();
    }
    if (book.getTickSize() <= 0) m_freshness.awaitingBook = true;
    m_bookDirty = false;
    updateVisibleRows();
    updateGrouping(book);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    // Scale decay, visible-range changes and trades leaving the window need a publish too.
    if (m_model->needsRepublish(now)) m_dirty = true;
    if (m_dirty) {
        QElapsedTimer cost;
        cost.start();
        m_model->publish(book, m_trades, m_follow, now);
        sLog_Probe("dom.publish", "us=" << double(cost.nsecsElapsed()) / 1000.0 << " group=" << m_model->group()
                   << " levels=" << m_model->lastGroupLevels() << " rows=" << m_model->filledFirst() << ".."
                   << m_model->filledLast());
    }
    // Every exposed tick, changed or not, so the 10 s totals keep moving on a quiet book.
    if (m_pulseActive) {
        m_model->pulseStep(pulseClock(true), m_pulseTrades);
        m_pulseTrades.clear();
    }
}

void OrderBookDock::refreshDisplay()
{
    if (!m_displayActive || !isVisible() || minimized()) return;
    m_programmaticScroll = true;
    const bool needsLayout = m_symbolDirty || m_model->rowCount() == 0;
    if (m_symbolDirty) {
        m_model->clear(m_symbol);
        m_symbolLabel->setText(m_symbol.isEmpty() ? "No symbol" : m_symbol);
        m_symbolDirty = false;
    }
    refreshBook();
    if (m_dirty) {
        m_recenter->setText(m_follow ? "Following" : "Recenter");
        m_recenter->setChecked(m_follow);
        m_table->horizontalHeader()->setToolTip(m_model->executionSummary());
        fitColumns();
        // New rows must establish the scroll range before the first follow
        // operation; regular ticks reuse that layout.
        if (needsLayout) {
            m_contentWidget->layout()->activate();
            m_table->doItemsLayout();
        }
        if (m_follow && m_model->centerRow() >= 0) m_table->scrollTo(m_model->index(m_model->centerRow(), DomModel::Price), QAbstractItemView::PositionAtCenter);
        m_dirty = false;
    }
    updateSummary();
    m_programmaticScroll = false;
}

void OrderBookDock::fitColumns()
{
    const int width = m_table->viewport()->width();
    const QFontMetrics fm(m_table->font());
    const int priceWidth = std::max(54, fm.horizontalAdvance(m_model->index(0, DomModel::Price).data().toString()) + 8);
    const int sizeWidth = fm.horizontalAdvance("0.0353") + 8;
    const int countWidth = std::max(fm.horizontalAdvance("Sell vol"), fm.horizontalAdvance("+88.8k")) + 10;
    const bool counts = width >= priceWidth + sizeWidth * 2 + countWidth * 3;
    const bool delta = width >= priceWidth + sizeWidth * 2 + countWidth;
    for (int col = DomModel::Buys; col < DomModel::Columns; ++col) {
        const bool visible = col == DomModel::Delta ? delta : counts;
        m_table->setColumnHidden(col, !visible);
        if (visible) m_table->setColumnWidth(col, countWidth);
    }
    const int core = width - (counts ? countWidth * 3 : delta ? countWidth : 0);
    const int price = std::min(priceWidth, core / 2);
    const int side = (core - price) / 2;
    m_table->setColumnWidth(DomModel::Bid, side);
    m_table->setColumnWidth(DomModel::Price, price);
    m_table->setColumnWidth(DomModel::Ask, core - price - side);
    m_table->horizontalScrollBar()->setValue(0);
}

void OrderBookDock::updateSummary()
{
    const bool wide = m_contentWidget->width() >= 720;
    QString fullStatus;
    if (m_health) {
        const auto health = m_health->snapshot(m_symbol);
        fullStatus = health.compact();
        m_status->setToolTip(health.reason + "\nBook age is time since an accepted book message; quiet is not stale.");
    } else {
        // Non-remote sources retain their readiness facts with the shared words.
        const auto state = m_freshness.connected == false ? MarketHealth::State::Disconnected
            : m_freshness.snapshotStale ? MarketHealth::State::Stale
            : m_freshness.awaitingBook || m_freshness.lastChangeMs <= 0 ? MarketHealth::State::WaitingForBook
            : m_freshness.connected == true ? MarketHealth::State::Live : MarketHealth::State::Initializing;
        fullStatus = MarketHealth::stateText(state);
        if (m_freshness.lastChangeMs > 0) {
            const double seconds = std::max<qint64>(0, QDateTime::currentMSecsSinceEpoch() - m_freshness.lastChangeMs) / 1000.0;
            fullStatus += " · last change " + QString::number(seconds, 'f', 1) + " s";
        }
        m_status->setToolTip(fullStatus);
    }
    m_status->setText(fullStatus);
    m_status->setMaximumWidth(m_status->fontMetrics().horizontalAdvance(m_status->text()) + 2);
    m_aggregation->setText(m_model->aggregation(true));
    m_aggregation->setToolTip(m_model->aggregation() + "\n" + m_model->executionSummary());
    m_summary->setText(m_model->summary(!wide));
    m_summary->setToolTip(m_model->summary() + "\n" + m_model->executionSummary());
    updatePulseFooter();
}

void OrderBookDock::updatePulseFooter()
{
    if (!m_pulseOn) return;
    const int width = m_pulseFooter->width();
    if (m_model->pulseTotalsVersion() == m_footerVersion && width == m_footerWidth) return;
    m_footerVersion = m_model->pulseTotalsVersion();
    m_footerWidth = width;
    const QString full = m_model->pulseSummary();
    // The longest form that fits; below that, whole " · " segments drop: never a cut number.
    const auto fits = [&](const QString& text) { return m_pulseFooter->fontMetrics().horizontalAdvance(text) <= width; };
    QString text = full;
    for (const auto form : {DomModel::PulseText::Compact, DomModel::PulseText::NoMax, DomModel::PulseText::Tiny}) {
        if (fits(text)) break;
        text = m_model->pulseSummary(form);
    }
    while (!fits(text) && text.contains(QStringLiteral(" · ")))
        text = text.left(text.lastIndexOf(QStringLiteral(" · "))) + QStringLiteral(" …");
    m_pulseFooter->setText(text);
    const auto& totals = m_model->pulseTotals();
    m_pulseFooter->setToolTip(QStringLiteral("Book changes over the last %1 s in the rows on screen (plus %2 rows each way), all sizes:\n%3\n"
                                             "Added: asks %4 · bids %5 %6. Pulled = cancelled before price got there; eaten = removed by trades.")
        .arg(DomModel::PulseWindowMs / 1000).arg(DomModel::PulseMarginRows).arg(full)
        .arg(DomModel::sizeText(totals.side[1].added), DomModel::sizeText(totals.side[0].added),
             m_symbol.section('-', 0, 0)));
}
