#include "HeatmapTelemetryDock.hpp"
#include <QCloseEvent>
#include <QGridLayout>
#include <QLabel>
#include <QPainter>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>

// The lab's frame-time graph: the last 90 samples (one per poll), 0..33.3 ms.
class HeatmapFrameGraph : public QWidget {
public:
    explicit HeatmapFrameGraph(const std::vector<double> &history, QWidget *parent)
        : QWidget(parent), history_(history) {
        setObjectName("frameGraph");
        setMinimumHeight(70);
    }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.fillRect(rect(), QColor("#0A1118"));
        p.setPen(QColor("#263B48"));
        p.drawLine(0, height() / 2, width(), height() / 2);
        if (history_.size() < 2) return;
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(QColor("#5DD4D8"), 1.5));
        QPolygonF line;
        for (size_t i = 0; i < history_.size(); ++i)
            line << QPointF(double(i) * width() / 89.0, height() - std::min(history_[i], 33.3) / 33.3 * height());
        p.drawPolyline(line);
    }
private:
    const std::vector<double> &history_;
};

namespace {
double num(const QVariantMap &m, const char *key) { return m.value(key).toDouble(); }
QString money(double v) { return v > 0 ? QStringLiteral("$") + QString::number(v, 'g', 12) : QStringLiteral("-"); }
QString mb(const QVariantMap &m, const char *key) { return QString::number(num(m, key) / 1048576.0, 'f', 1) + " MB"; }
QString integer(const QVariantMap &m, const char *key) { return QString::number(m.value(key).toLongLong()); }
QString ms(const QVariantMap &m, const char *key, int decimals) {
    return QString::number(num(m, key), 'f', decimals) + " ms";
}
QString text(const QVariantMap &m, const char *key) {
    const QString v = m.value(key).toString();
    return v.isEmpty() ? QStringLiteral("-") : v;
}
} // namespace

HeatmapTelemetryDock::HeatmapTelemetryDock(QWidget *parent)
    : DockablePanel("heatmapTelemetryDock", "Heatmap Telemetry", parent) {
    m_timer = new QTimer(this);
    m_timer->setInterval(kRefreshMs);
    connect(m_timer, &QTimer::timeout, this, &HeatmapTelemetryDock::refresh);
    buildUi();
    setMinimumWidth(kMinimumWidth); // labels and values side by side, as the lab panel
}

void HeatmapTelemetryDock::addSection(const QString &title) {
    auto *label = new QLabel(title, m_table);
    label->setStyleSheet("QLabel { color: #A6E7E9; font-weight: 700; font-size: 12px; padding-top: 6px; }");
    m_grid->addWidget(label, m_grid->rowCount(), 0, 1, 2);
}

void HeatmapTelemetryDock::addRow(const QString &name, const QString &key,
                                  std::function<QString(const QVariantMap &)> format) {
    const int row = m_grid->rowCount();
    auto *label = new QLabel(name, m_table);
    label->setStyleSheet("QLabel { color: #9BAFBA; font-size: 12px; }");
    auto *value = new QLabel("-", m_table);
    value->setObjectName("telemetry." + key);
    value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    value->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); // a long value never widens the dock
    value->setStyleSheet("QLabel { color: #E8F0F2; font-family: Menlo, monospace; font-size: 12px; }");
    m_grid->addWidget(label, row, 0);
    m_grid->addWidget(value, row, 1);
    m_rows.push_back({key, value, std::move(format)});
}

void HeatmapTelemetryDock::buildUi() {
    if (m_grid) return;
    auto *outer = new QVBoxLayout(m_contentWidget);
    outer->setContentsMargins(8, 8, 8, 8);
    m_contentWidget->setStyleSheet("QWidget { background-color: #070B10; }");
    m_disabled = new QLabel(m_contentWidget);
    m_disabled->setObjectName("telemetryDisabled");
    m_disabled->setWordWrap(true);
    m_disabled->setAlignment(Qt::AlignCenter);
    m_disabled->setStyleSheet("QLabel { color: #8198A6; font-size: 12px; padding: 16px; }");
    m_disabled->setText("The chart draws with the legacy renderer. Telemetry covers the GPU heatmap renderer: "
                        "Settings > Debug > Renderer: GPU.");
    outer->addWidget(m_disabled);

    auto *scroll = new QScrollArea(m_contentWidget);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_table = new QWidget(scroll);
    m_grid = new QGridLayout(m_table);
    m_grid->setVerticalSpacing(2);
    m_grid->setColumnStretch(1, 1); // labels at their width, values take the rest
    scroll->setWidget(m_table);
    outer->addWidget(scroll);

    auto *title = new QLabel("RENDER TELEMETRY", m_table);
    title->setStyleSheet("QLabel { color: #A6E7E9; font-weight: 700; font-size: 14px; }");
    m_grid->addWidget(title, 0, 0, 1, 2);
    auto *graphLabel = new QLabel("Frame time p50 per poll · last 90 samples", m_table);
    graphLabel->setStyleSheet("QLabel { color: #AAB7C0; font-size: 12px; }");
    m_grid->addWidget(graphLabel, 1, 0, 1, 2);
    m_graph = new HeatmapFrameGraph(m_frameHistory, m_table);
    m_grid->addWidget(m_graph, 2, 0, 1, 2);

    addSection("TICK");
    addRow("Mode", "mode", [](const QVariantMap &m) { return text(m, "mode"); });
    addRow("Tick", "tick", [](const QVariantMap &m) { return money(num(m, "tick")); });
    addRow("Manual tick", "manualTick", [](const QVariantMap &m) { return money(num(m, "manualTick")); });
    addRow("h", "hysteresis", [](const QVariantMap &m) { return QString::number(num(m, "hysteresis")); });
    addRow("Min row px", "minRowPx", [](const QVariantMap &m) { return QString::number(num(m, "minRowPx")); });
    addRow("Row height", "rowPx", [](const QVariantMap &m) { return QString::number(num(m, "rowPx"), 'f', 2) + " px"; });
    addRow("Finest tick in view", "commonTick", [](const QVariantMap &m) { return money(num(m, "commonTick")); });
    addRow("Last re-bin (CPU submit)", "lastBinMs", [](const QVariantMap &m) { return ms(m, "lastBinMs", 3); });
    addRow("Tick changes", "tickChanges", [](const QVariantMap &m) { return integer(m, "tickChanges"); });
    addRow("Crossfade", "crossfadeMs", [](const QVariantMap &m) {
        return integer(m, "crossfadeMs") + " ms" + (m.value("crossfading").toBool() ? " (fading)" : "");
    });
    addRow("Holding old picture", "holding", [](const QVariantMap &m) { return m.value("holding").toBool() ? "yes" : "no"; });
    addRow("Settled", "settled", [](const QVariantMap &m) { return m.value("settled").toBool() ? "yes" : "no"; });
    m_indicator = new QLabel(m_table);
    m_indicator->setObjectName("telemetry.indicator");
    m_indicator->setWordWrap(true);
    m_indicator->setStyleSheet("QLabel { color: #F0B46A; font-size: 11px; }");
    m_indicator->setVisible(false);
    m_grid->addWidget(m_indicator, m_grid->rowCount(), 0, 1, 2);

    addSection("GPU (NODE)");
    addRow("Resident / cap", "residentBytes", [](const QVariantMap &m) {
        return mb(m, "residentBytes") + " / " + QString::number(num(m, "gpuCapBytes") / 1048576.0, 'f', 0) + " MB";
    });
    addRow("Sources / bins", "sourceBytes", [](const QVariantMap &m) { return mb(m, "sourceBytes") + " / " + mb(m, "binBytes"); });
    addRow("Resident sources", "residentSources", [](const QVariantMap &m) { return integer(m, "residentSources"); });
    addRow("Uploads / passes / fills", "sourcesUploaded", [](const QVariantMap &m) {
        return integer(m, "sourcesUploaded") + " / " + integer(m, "binPasses") + " / " + integer(m, "fillPasses");
    });
    addRow("Evictions / missing", "evictions", [](const QVariantMap &m) {
        return integer(m, "evictions") + " / " + integer(m, "missingReports");
    });
    addRow("Slots ready/fallback/partial", "readySlots", [](const QVariantMap &m) {
        return integer(m, "readySlots") + "/" + integer(m, "fallbackSlots") + "/" + integer(m, "partialSlots");
    });
    addRow("Loading slots", "loadingSlots", [](const QVariantMap &m) { return integer(m, "loadingSlots"); });
    addRow("Refused spans (CPU ceiling)", "refusedSpans", [](const QVariantMap &m) {
        return integer(m, "refusedSpans") + " · " + mb(m, "refusedBytes");
    });

    addSection("LIVE EDGE");
    addRow("Connection", "connection", [](const QVariantMap &m) { return text(m, "connection"); });
    addRow("Version drawn / published", "liveVersion", [](const QVariantMap &m) {
        return integer(m, "liveVersion") + " / " + integer(m, "livePublished");
    });
    addRow("Publish to draw", "livePublishToDrawMs", [](const QVariantMap &m) { return ms(m, "livePublishToDrawMs", 1); });
    addRow("  p50 / p95", "livePublishP95", [](const QVariantMap &m) {
        return QString::number(num(m, "livePublishP50"), 'f', 1) + " / " + ms(m, "livePublishP95", 1);
    });
    addRow("Data age at draw", "liveDataAgeMs", [](const QVariantMap &m) { return ms(m, "liveDataAgeMs", 0); });
    addRow("  p50 / p95", "liveAgeP95", [](const QVariantMap &m) {
        return QString::number(num(m, "liveAgeP50"), 'f', 0) + " / " + ms(m, "liveAgeP95", 0);
    });
    addRow("Window L .. end", "liveL", [](const QVariantMap &m) { return text(m, "liveL") + " .. " + text(m, "liveEnd"); });
    addRow("Span (min)", "liveSpanMin", [](const QVariantMap &m) { return QString::number(num(m, "liveSpanMin"), 'f', 1); });
    addRow("Draws from max(L,E) / E", "liveFrom", [](const QVariantMap &m) { return text(m, "liveFrom") + " / " + text(m, "liveE"); });
    addRow("Uploads / passes", "liveUploads", [](const QVariantMap &m) {
        return integer(m, "liveUploads") + " / " + integer(m, "liveBinPasses");
    });
    addRow("Buffers created / sets", "liveBufferCreations", [](const QVariantMap &m) {
        return integer(m, "liveBufferCreations") + " / " + integer(m, "liveSets");
    });
    addRow("Compose / interval (min)", "liveComposeMs", [](const QVariantMap &m) {
        return QString::number(num(m, "liveComposeMs"), 'f', 2) + " ms / " + integer(m, "liveIntervalMs") + " ms (" +
               integer(m, "liveMinIntervalMs") + ")";
    });

    addSection("DATA (CONTROLLER)");
    addRow("Decoded chunks", "chunkBytes", [](const QVariantMap &m) { return mb(m, "chunkBytes") + " · " + integer(m, "chunkEntries"); });
    addRow("Chunk decodes / fetched", "chunkLoads", [](const QVariantMap &m) {
        return integer(m, "chunkLoads") + " / " + integer(m, "fetchedChunks");
    });
    addRow("Span builds / cache hits", "spanBuilds", [](const QVariantMap &m) {
        return integer(m, "spanBuilds") + " / " + integer(m, "spanCacheHits");
    });
    addRow("Span images alive / claimed", "spanLiveBytes", [](const QVariantMap &m) {
        return mb(m, "spanLiveBytes") + " / " + mb(m, "spanClaimedBytes");
    });
    addRow("Span LRU / building", "spanCacheBytes", [](const QVariantMap &m) {
        return mb(m, "spanCacheBytes") + " / " + mb(m, "spanReservedBytes");
    });
    addRow("Chunks wanted (pinned)", "chunkWantedBytes", [](const QVariantMap &m) { return mb(m, "chunkWantedBytes"); });
    addRow("CPU committed", "cpuCommittedBytes", [](const QVariantMap &m) { return mb(m, "cpuCommittedBytes"); });
    addRow("Snapshots published", "publications", [](const QVariantMap &m) { return integer(m, "publications"); });
    addRow("Process footprint", "footprintBytes", [](const QVariantMap &m) { return mb(m, "footprintBytes"); });

    addSection("RENDER");
    addRow("Frames/s (chart window)", "fps", [](const QVariantMap &m) { return QString::number(num(m, "fps"), 'f', 1); });
    addRow("Frame p50 / p95", "frameMs", [](const QVariantMap &m) {
        return QString::number(num(m, "frameMs"), 'f', 2) + " / " + ms(m, "frameP95Ms", 2);
    });
    addRow("GPU frame (all passes)", "gpuFrameMs", [](const QVariantMap &m) { return ms(m, "gpuFrameMs", 2); });
    addRow("Node prepare", "prepareMs", [](const QVariantMap &m) { return ms(m, "prepareMs", 2); });
    addRow("Kernel", "kernel", [](const QVariantMap &m) { return text(m, "kernel"); });
    m_grid->setRowStretch(m_grid->rowCount(), 1);
    m_table->setVisible(false);
}

void HeatmapTelemetryDock::setProvider(Provider provider) {
    m_provider = std::move(provider);
    refresh();
}

bool HeatmapTelemetryDock::disabledShown() const { return !m_disabled->isHidden(); }

QString HeatmapTelemetryDock::valueText(const QString &key) const {
    if (key == QLatin1String("indicator")) return m_indicator->text();
    for (const auto &row : m_rows)
        if (row.key == key) return row.value->text();
    return {};
}

// GUI thread, 4 Hz while visible: one provider read (atomics, shared pointers and
// the service's stats copy) and label texts. No per-frame hook.
void HeatmapTelemetryDock::refresh() {
    ++m_refreshes;
    const auto metrics = m_provider ? m_provider() : std::nullopt;
    m_disabled->setVisible(!metrics);
    m_table->setVisible(metrics.has_value());
    if (!metrics) return;
    for (const auto &row : m_rows) row.value->setText(row.format(*metrics));
    const QString indicator = metrics->value("indicator").toString();
    m_indicator->setText(indicator);
    m_indicator->setVisible(!indicator.isEmpty());
    m_frameHistory.push_back(metrics->value("frameMs").toDouble());
    if (m_frameHistory.size() > 90) m_frameHistory.erase(m_frameHistory.begin());
    m_graph->update();
}

void HeatmapTelemetryDock::showEvent(QShowEvent *event) {
    DockablePanel::showEvent(event);
    refresh();
    m_timer->start();
}

void HeatmapTelemetryDock::hideEvent(QHideEvent *event) {
    DockablePanel::hideEvent(event);
    m_timer->stop();
}

void HeatmapTelemetryDock::closeEvent(QCloseEvent *event) {
    DockablePanel::closeEvent(event);
    if (event->isAccepted()) emit closedByUser();
}
