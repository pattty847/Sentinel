#include "TopToolbar.hpp"
#include "SentinelLogging.hpp"
#include <QAction>
#include <QToolButton>
#include <QWidget>
#include <QHBoxLayout>
#include <QLabel>
#include <QIcon>
#include <QSlider>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <algorithm>

namespace {
const char* labelForTimeframeMs(int64_t ms) {
    switch (ms) {
        case 1000: return "1s";
        case 60000: return "1m";
        case 300000: return "5m";
        case 900000: return "15m";
        case 3600000: return "1h";
        case 14400000: return "4h";
        case 86400000: return "1D";
        default: return nullptr;
    }
}
}

TopToolbar::TopToolbar(QWidget* parent)
    : QToolBar(parent)
{
    setMovable(false);
    setFloatable(false);
    setIconSize(QSize(18, 18));
    setToolButtonStyle(Qt::ToolButtonIconOnly);

    QLabel* chartLabel = new QLabel("Charts", this);
    chartLabel->setStyleSheet("QLabel { color: #B6C2CF; font-weight: 600; padding-right: 6px; }");
    addWidget(chartLabel);

    m_symbolSearch = new QLineEdit(this);
    m_symbolSearch->setPlaceholderText("Search Symbol");
    m_symbolSearch->setText("BTC-USD");
    m_symbolSearch->setFixedWidth(170);
    addWidget(m_symbolSearch);

    m_subscribeButton = new QToolButton(this);
    m_subscribeButton->setIcon(QIcon(":/svg/search.svg"));
    m_subscribeButton->setToolTip("Subscribe");
    addWidget(m_subscribeButton);
    connect(m_subscribeButton, &QToolButton::clicked, this, &TopToolbar::subscribeRequested);

    addSeparator();

    // Layer toggles: one primary field + independent overlays.
    auto* candleAction = addAction(QIcon(":/svg/candlestick_chart.svg"), "Candles");
    candleAction->setCheckable(true);
    candleAction->setChecked(true);

    m_heatmapButton = addIconButton(":/svg/grid_view.svg", "Heatmap");
    m_heatmapButton->setCheckable(true);
    m_heatmapButton->setChecked(true);
    m_heatmapButton->setAutoExclusive(false);

    m_footprintButton = addIconButton(":/svg/footprint.svg", "Footprint");
    m_footprintButton->setCheckable(true);
    m_footprintButton->setAutoExclusive(false);

    m_tpoButton = addIconButton(":/svg/tpo_chart.svg", "TPO");
    m_tpoButton->setCheckable(true);
    m_tpoButton->setAutoExclusive(false);

    // No dedicated volume-profile icon exists under resources (searched for
    // *profile*/*volume*.svg); reuse the TPO icon until one is added.
    m_volumeProfileButton = addIconButton(":/svg/tpo_chart.svg", "Volume profile");
    m_volumeProfileButton->setCheckable(true);
    m_volumeProfileButton->setAutoExclusive(false);

    connect(candleAction, &QAction::toggled, this, [this](bool enabled) { emit candlesToggled(enabled); });
    connect(m_heatmapButton, &QToolButton::toggled, this, [this](bool enabled) {
        sLog_App("ui: toolbar heatmap button checked=" << enabled);
        emit heatmapToggled(enabled);
        if (enabled) {
            emit primaryFieldRequested(0);
        }
    });
    connect(m_footprintButton, &QToolButton::toggled, this, [this](bool enabled) {
        sLog_App("ui: toolbar footprint button checked=" << enabled);
        emit footprintToggled(enabled);
        if (enabled) {
            emit primaryFieldRequested(1);
        }
    });
    connect(m_tpoButton, &QToolButton::toggled, this, [this](bool enabled) {
        sLog_App("ui: toolbar tpo button checked=" << enabled);
        emit tpoToggled(enabled);
        if (enabled) {
            emit primaryFieldRequested(2);
        }
    });
    connect(m_volumeProfileButton, &QToolButton::toggled, this, [this](bool enabled) {
        sLog_App("ui: toolbar volumeProfile button checked=" << enabled);
        emit volumeProfileToggled(enabled);
        if (enabled) {
            emit primaryFieldRequested(3);
        }
    });

    addSeparator();

    m_timeframeCombo = new QComboBox(this);
    m_timeframeCombo->addItems({"1s", "1m", "5m", "15m", "1h", "4h", "1D"});
    const int64_t timeframesMs[] = {1000, 60000, 300000, 900000, 3600000, 14400000, 86400000};
    for (int i = 0; i < m_timeframeCombo->count(); ++i) {
        m_timeframeCombo->setItemData(i, static_cast<qlonglong>(timeframesMs[i]), Qt::UserRole);
    }
    m_timeframeCombo->setFixedWidth(70);
    addWidget(m_timeframeCombo);
    connect(m_timeframeCombo, &QComboBox::currentTextChanged, this, &TopToolbar::timeframeSelected);

    // Heatmap tick: Auto/Manual and the preset (S6c). Driven by setTickSelectorState.
    m_tickModeCombo = new QComboBox(this);
    m_tickModeCombo->setObjectName("tickModeCombo");
    m_tickModeCombo->addItems({"Auto", "Manual"});
    m_tickModeCombo->setFixedWidth(84);
    addWidget(m_tickModeCombo);
    connect(m_tickModeCombo, QOverload<int>::of(&QComboBox::activated), this, [this](int index) {
        sLog_App("ui: toolbar tick mode=" << (index == 1 ? "manual" : "auto"));
        emit tickModeRequested(index == 1);
    });
    m_tickPresetCombo = new QComboBox(this);
    m_tickPresetCombo->setObjectName("tickPresetCombo");
    m_tickPresetCombo->setFixedWidth(84);
    addWidget(m_tickPresetCombo);
    connect(m_tickPresetCombo, QOverload<int>::of(&QComboBox::activated), this, [this](int index) {
        const qint64 units = m_tickPresetCombo->itemData(index).toLongLong();
        if (units <= 0) return;
        sLog_App("ui: toolbar tick preset units=" << units);
        emit tickPresetRequested(units);
    });
    m_tickVeilLabel = new QLabel("veiled", this);
    m_tickVeilLabel->setObjectName("tickVeilLabel");
    m_tickVeilLabel->setStyleSheet("QLabel { color: #F0B46A; padding-left: 4px; }");
    m_tickVeilLabel->setVisible(false);
    addWidget(m_tickVeilLabel);
    setTickSelectorState({});

    m_chartTypeCombo = new QComboBox(this);
    m_chartTypeCombo->addItems({"Candle", "Hollow", "Line"});
    m_chartTypeCombo->setFixedWidth(90);
    addWidget(m_chartTypeCombo);
    connect(m_chartTypeCombo, &QComboBox::currentTextChanged, this, &TopToolbar::chartTypeSelected);

    m_colorPresetCombo = new QComboBox(this);
    m_colorPresetCombo->addItems({"Electric", "Fire", "Ocean", "Monochrome", "Matrix"});
    m_colorPresetCombo->setFixedWidth(100);
    m_colorPresetCombo->setToolTip("Heatmap color palette");
    addWidget(m_colorPresetCombo);
    connect(m_colorPresetCombo, &QComboBox::currentTextChanged, this, &TopToolbar::colorPresetSelected);

    addSeparator();

    QLabel* liqLabel = new QLabel("Liq", this);
    liqLabel->setStyleSheet("QLabel { color: #B6C2CF; padding-left: 4px; }");
    addWidget(liqLabel);

    QLabel* modeLabel = new QLabel("Mode", this);
    modeLabel->setStyleSheet("QLabel { color: #B6C2CF; padding-left: 6px; }");
    addWidget(modeLabel);

    m_liquidityModeCombo = new QComboBox(this);
    m_liquidityModeCombo->addItems({"Asset", "USD"});
    m_liquidityModeCombo->setFixedWidth(80);
    m_liquidityModeCombo->setToolTip("Liquidity label mode");
    addWidget(m_liquidityModeCombo);
    connect(m_liquidityModeCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this,
            &TopToolbar::liquidityLabelModeChanged);

    m_liquiditySlider = new QSlider(Qt::Horizontal, this);
    m_liquiditySlider->setRange(0, 1000);
    m_liquiditySlider->setFixedWidth(120);
    m_liquiditySlider->setToolTip("Liquidity threshold");
    addWidget(m_liquiditySlider);
    connect(m_liquiditySlider, &QSlider::valueChanged, this, [this](int value) {
        emit liquidityThresholdChanged(static_cast<double>(value));
    });

    addSeparator();

    QAction* indicatorsAction = addIconAction(":/svg/indicators.svg", "Indicators", "Indicators");
    QAction* layoutsAction = addIconAction(":/svg/layout.svg", "Layouts", "Layouts");

    addSeparator();

    QAction* quickSearchAction = addIconAction(":/svg/search.svg", "Quick Search", "Quick Search");
    QAction* settingsAction = addIconAction(":/svg/settings.svg", "Settings", "Settings");
    QAction* fullscreenAction = addIconAction(":/svg/full_screen.svg", "Fullscreen", "Toggle Fullscreen");
    QAction* screenshotAction = addIconAction(":/svg/camera.svg", "Screenshot", "Screenshot");

    connect(indicatorsAction, &QAction::triggered, this, &TopToolbar::indicatorsRequested);
    connect(layoutsAction, &QAction::triggered, this, &TopToolbar::layoutsRequested);
    connect(quickSearchAction, &QAction::triggered, this, &TopToolbar::quickSearchRequested);
    connect(settingsAction, &QAction::triggered, this, &TopToolbar::settingsRequested);
    connect(fullscreenAction, &QAction::triggered, this, &TopToolbar::fullscreenToggled);
    connect(screenshotAction, &QAction::triggered, this, &TopToolbar::screenshotRequested);
}

QAction* TopToolbar::addIconAction(const QString& iconPath, const QString& text, const QString& tooltip) {
    QAction* action = addAction(QIcon(iconPath), text);
    action->setToolTip(tooltip);
    return action;
}

QToolButton* TopToolbar::addIconButton(const QString& iconPath, const QString& tooltip) {
    QToolButton* button = new QToolButton(this);
    button->setIcon(QIcon(iconPath));
    button->setToolTip(tooltip);
    addWidget(button);
    return button;
}

void TopToolbar::setTimeframeMs(int64_t ms) {
    if (!m_timeframeCombo) {
        return;
    }
    if (const char* label = labelForTimeframeMs(ms)) {
        const int idx = m_timeframeCombo->findText(label);
        if (idx >= 0 && idx != m_timeframeCombo->currentIndex()) {
            const QSignalBlocker blocker(*m_timeframeCombo);
            m_timeframeCombo->setCurrentIndex(idx);
        }
    }
}

void TopToolbar::setAvailableTimeframes(const std::vector<int64_t>& servedTimeframesMs) {
    if (!m_timeframeCombo) {
        return;
    }
    const QSignalBlocker blocker(*m_timeframeCombo);
    auto* model = qobject_cast<QStandardItemModel*>(m_timeframeCombo->model());
    int firstAvailable = -1;
    for (int i = 0; i < m_timeframeCombo->count(); ++i) {
        const int64_t tf = m_timeframeCombo->itemData(i, Qt::UserRole).toLongLong();
        const bool available = servedTimeframesMs.empty() ||
            std::find(servedTimeframesMs.begin(), servedTimeframesMs.end(), tf) != servedTimeframesMs.end();
        model->item(i)->setEnabled(available);
        m_timeframeCombo->setItemData(i,
            available ? QString() : QStringLiteral("Not built yet: larger timeframes will be rolled up from 1m"),
            Qt::ToolTipRole);
        if (available && firstAvailable < 0) {
            firstAvailable = i;
        }
    }
    if (firstAvailable >= 0 && !model->item(m_timeframeCombo->currentIndex())->isEnabled()) {
        m_timeframeCombo->setCurrentIndex(firstAvailable);
    }
}

void TopToolbar::setLayerToggleStates(bool heatmapEnabled,
                                      bool footprintEnabled,
                                      bool tpoEnabled,
                                      bool volumeProfileEnabled) {
    sLog_Probe("ui.layers",
               "toolbar sync heatmap=" << heatmapEnabled << " footprint=" << footprintEnabled
               << " tpo=" << tpoEnabled << " volumeProfile=" << volumeProfileEnabled);
    if (m_heatmapButton) {
        const QSignalBlocker blocker(*m_heatmapButton);
        m_heatmapButton->setChecked(heatmapEnabled);
    }
    if (m_footprintButton) {
        const QSignalBlocker blocker(*m_footprintButton);
        m_footprintButton->setChecked(footprintEnabled);
    }
    if (m_tpoButton) {
        const QSignalBlocker blocker(*m_tpoButton);
        m_tpoButton->setChecked(tpoEnabled);
    }
    if (m_volumeProfileButton) {
        const QSignalBlocker blocker(*m_volumeProfileButton);
        m_volumeProfileButton->setChecked(volumeProfileEnabled);
    }
}

QString TopToolbar::tickText(int64_t units, double priceScale) {
    if (units <= 0 || !(priceScale > 0)) return QStringLiteral("-");
    return QStringLiteral("$") + QString::number(double(units) / priceScale, 'g', 12);
}

void TopToolbar::fillTickPresetCombo(QComboBox* combo, const TickSelectorState& state, const QString& emptyText) {
    const QSignalBlocker block(combo);
    combo->clear();
    const int64_t shown = state.manual ? state.manualUnits : state.drawnUnits;
    const bool shownOffered =
        shown > 0 && std::find(state.offeredUnits.begin(), state.offeredUnits.end(), shown) != state.offeredUnits.end();
    std::vector<int64_t> units = state.offeredUnits;
    if (shown > 0 && !shownOffered) {
        units.push_back(shown);
        std::sort(units.begin(), units.end());
    }
    auto* model = qobject_cast<QStandardItemModel*>(combo->model());
    for (const int64_t u : units) {
        const bool offered = u != shown || shownOffered;
        combo->addItem(tickText(u, state.priceScale) + (offered ? QString() : QStringLiteral(" (unavailable)")),
                       qlonglong(u));
        if (!offered && model) {
            auto* item = model->item(combo->count() - 1);
            item->setEnabled(false);
            item->setToolTip(QStringLiteral("No loaded data builds this tick; its columns are veiled."));
        }
    }
    combo->setPlaceholderText(emptyText);
    combo->setCurrentIndex(shown > 0 ? combo->findData(qlonglong(shown)) : -1);
}

void TopToolbar::setTickSelectorState(const TickSelectorState& state) {
    if (m_tickStateSet && state == m_tickState) return;
    m_tickStateSet = true;
    m_tickState = state;
    {
        const QSignalBlocker block(m_tickModeCombo);
        m_tickModeCombo->setCurrentIndex(state.manual ? 1 : 0);
    }
    fillTickPresetCombo(m_tickPresetCombo, state, QStringLiteral("tick"));

    m_tickModeCombo->setEnabled(state.enabled);
    m_tickPresetCombo->setEnabled(state.enabled && !state.offeredUnits.empty());
    if (!state.enabled) {
        m_tickModeCombo->setToolTip(state.disabledReason);
        m_tickPresetCombo->setToolTip(state.disabledReason);
        m_tickVeilLabel->setVisible(false);
        return;
    }
    m_tickModeCombo->setToolTip(state.manual
        ? QStringLiteral("Manual: the tick stays locked; zoom only scales rows. Remembered per symbol and timeframe.")
        : QStringLiteral("Auto: the smallest preset whose rows are at least the minimum row height."));
    QString tip = state.manual ? QStringLiteral("Locked tick %1. Presets: what some loaded data can build.")
                                     .arg(tickText(state.manualUnits, state.priceScale))
                               : QStringLiteral("Drawn tick %1 (Auto). Pick a preset to lock it (Manual).")
                                     .arg(tickText(state.drawnUnits, state.priceScale));
    if (!state.indicator.isEmpty()) tip += QStringLiteral("\n") + state.indicator;
    m_tickPresetCombo->setToolTip(tip);
    m_tickVeilLabel->setToolTip(state.indicator);
    m_tickVeilLabel->setVisible(!state.indicator.isEmpty());
}

void TopToolbar::setColorPreset(const QString& preset) {
    if (!m_colorPresetCombo) return;
    const int index = m_colorPresetCombo->findText(preset);
    if (index < 0) return; // Custom: no combo entry
    const QSignalBlocker block(m_colorPresetCombo);
    m_colorPresetCombo->setCurrentIndex(index);
}
