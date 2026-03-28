#include "AxisTextService.hpp"

#include "models/AxisModel.hpp"
#include "SentinelLogging.hpp"
#include "ChartTextLayout.hpp"

#include <QQuickWindow>
#include <QScreen>
#include <QElapsedTimer>
#include <cmath>

namespace {
bool envIntValue(const char *name, int &out) {
    const QByteArray value = qgetenv(name);
    if (value.isEmpty()) {
        return false;
    }
    bool ok = false;
    const int parsed = value.toInt(&ok);
    if (!ok) {
        return false;
    }
    out = parsed;
    return true;
}
} // namespace

AxisTextService::AxisTextService(const ChartTextAtlas& atlas, QObject* parent)
    : QObject(parent)
    , m_atlas(atlas)
{
}

void AxisTextService::setPriceAxisSource(QObject *source) {
    syncAxisTicks(m_priceAxisSource, m_priceAxisConnections, m_priceAxisTicks,
                  m_priceAxisSource, source);
}

void AxisTextService::setTimeAxisSource(QObject *source) {
    syncAxisTicks(m_timeAxisSource, m_timeAxisConnections, m_timeAxisTicks,
                  m_timeAxisSource, source);
}

void AxisTextService::setAxisLabelPxOverride(int px) {
    m_axisLabelPxOverride = px;
}

void AxisTextService::syncAxisTicks(
    AxisModel *currentModel, std::vector<QMetaObject::Connection> &connections,
    std::vector<AxisTickSnapshot> &storage, AxisModel *&target,
    QObject *source) {
    AxisModel *nextModel = qobject_cast<AxisModel *>(source);
    if (currentModel == nextModel) {
        return;
    }

    for (const auto &connection : connections) {
        disconnect(connection);
    }
    connections.clear();
    target = nextModel;

    if (target) {
        auto refresh = [this, &storage, model = target]() {
            refreshAxisTickSnapshot(model, storage);
            emit needsUpdate();
        };
        connections.push_back(connect(
            target, &QAbstractItemModel::dataChanged, this,
            [refresh](const auto &, const auto &, const auto &) { refresh(); }));
        connections.push_back(connect(target, &QAbstractItemModel::modelReset, this,
                                      [refresh]() { refresh(); }));
        refresh();
    } else {
        {
            std::lock_guard<std::mutex> lock(m_axisSnapshotMutex);
            storage.clear();
        }
        refreshAxisLayout();
        emit needsUpdate();
    }

    emit axisSourcesChanged();
}

void AxisTextService::refreshAxisTickSnapshot(
    AxisModel *model, std::vector<AxisTickSnapshot> &storage) {
    {
        std::lock_guard<std::mutex> lock(m_axisSnapshotMutex);
        storage.clear();
        if (model) {
            std::vector<AxisModel::TickSnapshot> ticks;
            model->copyTicks(ticks);
            storage.reserve(ticks.size());
            for (const auto &tick : ticks) {
                storage.push_back({tick.position, tick.label, tick.isMajorTick});
            }
        }
    }
    refreshAxisLayout();
}

void AxisTextService::bindAxisLayoutWindow(QQuickWindow *window) {
    if (m_axisLayoutWindowConnection) {
        disconnect(m_axisLayoutWindowConnection);
        m_axisLayoutWindowConnection = {};
    }
    if (m_axisLayoutScreenConnection) {
        disconnect(m_axisLayoutScreenConnection);
        m_axisLayoutScreenConnection = {};
    }
    m_axisLayoutWindowScreen.clear();

    auto bindScreen = [this](QScreen *screen) {
        if (m_axisLayoutScreenConnection) {
            disconnect(m_axisLayoutScreenConnection);
            m_axisLayoutScreenConnection = {};
        }
        m_axisLayoutWindowScreen = screen;
        if (screen) {
            m_axisLayoutScreenConnection =
                connect(screen, &QScreen::logicalDotsPerInchChanged, this,
                        [this](qreal) { refreshAxisLayout(); });
        }
        refreshAxisLayout();
    };

    if (window) {
        m_axisLayoutWindowConnection =
            connect(window, &QQuickWindow::screenChanged, this, bindScreen);
        bindScreen(window->screen());
    } else {
        refreshAxisLayout();
    }
}

void AxisTextService::refreshAxisLayout() {
    int envAxisLabelPx = 0;
    envIntValue("SENTINEL_AXIS_LABEL_PX", envAxisLabelPx);

    double logicalDpiY = 96.0;
    // Try the parent QQuickItem's window first, fall back to cached screen.
    if (auto *item = qobject_cast<QQuickItem *>(parent())) {
        if (QQuickWindow *itemWindow = item->window();
            itemWindow && itemWindow->screen()) {
            logicalDpiY = itemWindow->screen()->logicalDotsPerInchY();
        } else if (m_axisLayoutWindowScreen) {
            logicalDpiY = m_axisLayoutWindowScreen->logicalDotsPerInchY();
        }
    } else if (m_axisLayoutWindowScreen) {
        logicalDpiY = m_axisLayoutWindowScreen->logicalDotsPerInchY();
    }

    const int resolvedLabelPx = AxisLayout::resolveEffectiveAxisLabelPx(
        logicalDpiY, m_axisLabelPxOverride, envAxisLabelPx);
    const float axisScale =
        (m_atlas.fontPx() > 0)
            ? (static_cast<float>(resolvedLabelPx) /
               static_cast<float>(m_atlas.fontPx()))
            : 1.0f;

    std::vector<QString> priceLabels;
    {
        std::lock_guard<std::mutex> lock(m_axisSnapshotMutex);
        priceLabels.reserve(m_priceAxisTicks.size());
        for (const auto &tick : m_priceAxisTicks) {
            if (!tick.label.isEmpty()) {
                priceLabels.push_back(tick.label);
            }
        }
    }

    const int nextPriceAxisWidth = AxisLayout::measurePriceAxisWidthPx(
        m_atlas, axisScale, priceLabels, m_priceAxisWidthPx);
    const int nextTimeAxisHeight = AxisLayout::measureTimeAxisHeightPx(
        m_atlas, axisScale, m_timeAxisHeightPx);

    {
        std::lock_guard<std::mutex> lock(m_axisLayoutMutex);
        m_axisLayoutSnapshot = AxisLayoutSnapshot{
            static_cast<float>(resolvedLabelPx), axisScale, nextPriceAxisWidth,
            nextTimeAxisHeight};
    }

    const bool changed =
        !qFuzzyCompare(m_effectiveAxisLabelPx + 1.0,
                       static_cast<double>(resolvedLabelPx) + 1.0) ||
        m_priceAxisWidthPx != nextPriceAxisWidth ||
        m_timeAxisHeightPx != nextTimeAxisHeight;
    m_effectiveAxisLabelPx = static_cast<double>(resolvedLabelPx);
    m_priceAxisWidthPx = nextPriceAxisWidth;
    m_timeAxisHeightPx = nextTimeAxisHeight;
    if (changed) {
        emit layoutChanged();
    }
    emit needsUpdate();
}

void AxisTextService::submitAxisText(ChartTextRenderer& renderer,
                                     const ChartTextAtlas& atlas,
                                     qreal canvasWidth,
                                     qreal canvasHeight) {
    if (atlas.fontPx() <= 0) {
        return;
    }
    const bool axisPixelSnap =
        !qEnvironmentVariableIsSet("SENTINEL_CHART_TEXT_NO_PIXEL_SNAP");

    std::vector<AxisTickSnapshot> priceTicks;
    std::vector<AxisTickSnapshot> timeTicks;
    AxisLayoutSnapshot axisLayout;
    {
        std::lock_guard<std::mutex> lock(m_axisSnapshotMutex);
        priceTicks = m_priceAxisTicks;
        timeTicks = m_timeAxisTicks;
    }
    {
        std::lock_guard<std::mutex> lock(m_axisLayoutMutex);
        axisLayout = m_axisLayoutSnapshot;
    }

    if (qEnvironmentVariableIsSet("SENTINEL_CHART_TEXT_DEBUG")) {
        static QElapsedTimer chartTextDebugTimer;
        static bool chartTextDebugStarted = false;
        if (!chartTextDebugStarted) {
            chartTextDebugTimer.start();
            chartTextDebugStarted = true;
        }
        if (chartTextDebugTimer.elapsed() > 1000) {
            const QString firstPrice = priceTicks.empty() ? QStringLiteral("<none>")
                                                          : priceTicks.front().label;
            const QString firstTime = timeTicks.empty() ? QStringLiteral("<none>")
                                                        : timeTicks.front().label;
            sLog_Debug(
                QString("Chart text axis snapshot: price=%1 firstPrice=%2 time=%3 "
                        "firstTime=%4 atlasFontPx=%5 pxRange=%6 padding=%7 "
                        "lineHeight=%8 scale=%9 snap=%10")
                    .arg(static_cast<int>(priceTicks.size()))
                    .arg(firstPrice)
                    .arg(static_cast<int>(timeTicks.size()))
                    .arg(firstTime)
                    .arg(atlas.fontPx())
                    .arg(atlas.pxRange(), 0, 'f', 2)
                    .arg(atlas.paddingPx())
                    .arg(atlas.lineHeightPx(), 0, 'f', 2)
                    .arg(axisLayout.axisScale *
                             static_cast<float>(atlas.fontPx()),
                         0,
                         'f', 2)
                    .arg(axisPixelSnap ? 1 : 0));
            auto logSample = [&](const char *label, const QString &text,
                                 const QPointF &anchor) {
                ChartTextRun sampleRun;
                sampleRun.text = text;
                sampleRun.anchor = anchor;
                sampleRun.color = Qt::white;
                sampleRun.scale = axisLayout.axisScale;
                sampleRun.hAlign =
                    (QString::fromLatin1(label) == QStringLiteral("price"))
                        ? ChartTextRun::HorizontalAlign::Left
                        : ChartTextRun::HorizontalAlign::Center;
                sampleRun.vAlign = ChartTextRun::VerticalAlign::Center;
                sampleRun.useStableMetrics = true;
                sampleRun.pixelSnap = axisPixelSnap;
                ChartGlyphInstance sampleGlyph;
                float renderPx = 0.0f;
                if (ChartTextLayout::buildDebugSample(atlas, sampleRun,
                                                      sampleGlyph, renderPx)) {
                    const float fracX = static_cast<float>(
                        sampleGlyph.rect.left() - std::floor(sampleGlyph.rect.left()));
                    const float fracY = static_cast<float>(
                        sampleGlyph.rect.top() - std::floor(sampleGlyph.rect.top()));
                    sLog_Debug(
                        QString("Chart text sample[%1]: text=%2 renderPx=%3 rect=[%4,%5 "
                                "%6x%7] frac=[%8,%9] uv=[%10,%11 %12x%13]")
                            .arg(QString::fromLatin1(label))
                            .arg(text)
                            .arg(renderPx, 0, 'f', 2)
                            .arg(sampleGlyph.rect.left(), 0, 'f', 2)
                            .arg(sampleGlyph.rect.top(), 0, 'f', 2)
                            .arg(sampleGlyph.rect.width(), 0, 'f', 2)
                            .arg(sampleGlyph.rect.height(), 0, 'f', 2)
                            .arg(fracX, 0, 'f', 2)
                            .arg(fracY, 0, 'f', 2)
                            .arg(sampleGlyph.uv.left(), 0, 'f', 4)
                            .arg(sampleGlyph.uv.top(), 0, 'f', 4)
                            .arg(sampleGlyph.uv.width(), 0, 'f', 4)
                            .arg(sampleGlyph.uv.height(), 0, 'f', 4));
                }
            };
            if (!priceTicks.empty()) {
                logSample("price", priceTicks.front().label,
                          QPointF(canvasWidth + 6.0, priceTicks.front().position));
            }
            if (!timeTicks.empty()) {
                logSample("time", timeTicks.front().label,
                          QPointF(timeTicks.front().position,
                                  canvasHeight + axisLayout.timeAxisHeightPx * 0.5));
            }
            chartTextDebugTimer.restart();
        }
    }

    const qreal priceInsetLeft = 4.0;
    const qreal priceInsetRight = 4.0;
    const qreal timeInsetTop = 2.0;
    const qreal timeInsetBottom = 2.0;
    const QRectF priceSafeRect(
        canvasWidth + priceInsetLeft, 0.0,
        std::max(0.0, static_cast<double>(axisLayout.priceAxisWidthPx) -
                          (priceInsetLeft + priceInsetRight)),
        canvasHeight);
    const QRectF timeSafeRect(
        0.0, canvasHeight + timeInsetTop, canvasWidth,
        std::max(0.0, static_cast<double>(axisLayout.timeAxisHeightPx) -
                          (timeInsetTop + timeInsetBottom)));
    const qreal priceAnchorX = priceSafeRect.left();
    const qreal stableMetricCenterOffset =
        0.5 * static_cast<qreal>(atlas.glyphTopPx() +
                                 atlas.glyphBottomPx()) *
        static_cast<qreal>(axisLayout.axisScale);
    const qreal timeAnchorY = timeSafeRect.center().y();
    const QColor axisColor(255, 255, 255, 255);
    for (const auto &tick : priceTicks) {
        ChartTextRun run;
        run.text = tick.label;
        run.anchor = QPointF(priceAnchorX, tick.position + stableMetricCenterOffset);
        run.color = axisColor;
        run.scale = axisLayout.axisScale;
        run.hAlign = ChartTextRun::HorizontalAlign::Left;
        run.vAlign = ChartTextRun::VerticalAlign::Center;
        run.useStableMetrics = true;
        run.pixelSnap = axisPixelSnap;
        QRectF runRect;
        if (!ChartTextLayout::measureRunRect(atlas, run, runRect)) {
            continue;
        }
        if (runRect.top() < priceSafeRect.top() ||
            runRect.bottom() > priceSafeRect.bottom() ||
            runRect.right() > priceSafeRect.right()) {
            continue;
        }
        renderer.submitRun(run, ChartTextRenderer::Priority::High);
    }
    for (const auto &tick : timeTicks) {
        ChartTextRun run;
        run.text = tick.label;
        run.anchor = QPointF(tick.position, timeAnchorY);
        run.color = axisColor;
        run.scale = axisLayout.axisScale;
        run.hAlign = ChartTextRun::HorizontalAlign::Center;
        run.vAlign = ChartTextRun::VerticalAlign::Center;
        run.useStableMetrics = true;
        run.pixelSnap = axisPixelSnap;
        QRectF runRect;
        if (!ChartTextLayout::measureRunRect(atlas, run, runRect)) {
            continue;
        }
        if (runRect.left() < timeSafeRect.left() ||
            runRect.right() > timeSafeRect.right()) {
            continue;
        }
        renderer.submitRun(run, ChartTextRenderer::Priority::High);
    }
}
