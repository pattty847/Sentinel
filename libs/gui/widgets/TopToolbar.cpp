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
    m_candleAction = candleAction;

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

    connect(candleAction, &QAction::toggled, this, [this](bool enabled) {
        m_mode.candles = enabled;
        applyVisibility();
        emit candlesToggled(enabled);
    });
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
    m_tickModeAction = addWidget(m_tickModeCombo);
    connect(m_tickModeCombo, QOverload<int>::of(&QComboBox::activated), this, [this](int index) {
        sLog_App("ui: toolbar tick mode=" << (index == 1 ? "manual" : "auto"));
        emit tickModeRequested(index == 1);
    });
    m_tickPresetCombo = new QComboBox(this);
    m_tickPresetCombo->setObjectName("tickPresetCombo");
    m_tickPresetCombo->setFixedWidth(84);
    m_tickPresetAction = addWidget(m_tickPresetCombo);
    connect(m_tickPresetCombo, QOverload<int>::of(&QComboBox::activated), this, [this](int index) {
        const qint64 units = m_tickPresetCombo->itemData(index).toLongLong();
        if (units <= 0) return;
        sLog_App("ui: toolbar tick preset units=" << units);
        emit tickPresetRequested(units);
    });
    m_tickVeilLabel = new QLabel("veiled", this);
    m_tickVeilLabel->setObjectName("tickVeilLabel");
    m_tickVeilLabel->setStyleSheet("QLabel { color: #F0B46A; padding-left: 4px; }");
    m_tickVeilAction = addWidget(m_tickVeilLabel);
    setTickSelectorState({});

    m_chartTypeCombo = new QComboBox(this);
    m_chartTypeCombo->addItems({"Candle", "Hollow", "Line"});
    m_chartTypeCombo->setObjectName("chartTypeCombo");
    m_chartTypeCombo->setFixedWidth(90);
    m_chartTypeAction = addWidget(m_chartTypeCombo);
    connect(m_chartTypeCombo, &QComboBox::currentTextChanged, this, &TopToolbar::chartTypeSelected);

    m_colorPresetCombo = new QComboBox(this);
    m_colorPresetCombo->addItems({"Electric", "Fire", "Ocean", "Monochrome", "Matrix"});
    m_colorPresetCombo->setFixedWidth(100);
    m_colorPresetCombo->setObjectName("colorPresetCombo");
    m_colorPresetCombo->setToolTip("Heatmap color palette");
    m_paletteAction = addWidget(m_colorPresetCombo);
    connect(m_colorPresetCombo, &QComboBox::currentTextChanged, this, &TopToolbar::colorPresetSelected);

    // TPO-only controls (renderer state; HeatmapChartControls keeps them in sync).
    m_tpoSessionCombo = new QComboBox(this);
    m_tpoSessionCombo->setObjectName("tpoSessionCombo");
    for (const auto &[name, id] : std::initializer_list<std::pair<const char *, int>>{
             {"New York", 0}, {"London", 1}, {"Asia", 2}, {"Australia", 3}, {"24H", 4}, {"1W", 5}, {"1M", 6}})
        m_tpoSessionCombo->addItem(name, id);
    m_tpoSessionCombo->setFixedWidth(96);
    m_tpoSessionCombo->setToolTip("TPO session (the volume profile follows it)");
    m_tpoSessionAction = addWidget(m_tpoSessionCombo);
    connect(m_tpoSessionCombo, QOverload<int>::of(&QComboBox::activated), this, [this](int index) {
        sLog_App("ui: toolbar tpo session=" << m_tpoSessionCombo->itemData(index).toInt());
        emit tpoSessionSelected(m_tpoSessionCombo->itemData(index).toInt());
    });
    m_tpoLayoutCombo = new QComboBox(this);
    m_tpoLayoutCombo->setObjectName("tpoLayoutCombo");
    m_tpoLayoutCombo->addItem("Collapsed", "collapsed");
    m_tpoLayoutCombo->addItem("Split", "split");
    m_tpoLayoutCombo->setFixedWidth(96);
    m_tpoLayoutCombo->setToolTip("TPO layout");
    m_tpoLayoutAction = addWidget(m_tpoLayoutCombo);
    connect(m_tpoLayoutCombo, QOverload<int>::of(&QComboBox::activated), this, [this](int index) {
        sLog_App("ui: toolbar tpo layout=" << m_tpoLayoutCombo->itemData(index).toString());
        emit tpoLayoutSelected(m_tpoLayoutCombo->itemData(index).toString());
    });

    addSeparator();

    QLabel* liqLabel = new QLabel("Liq", this);
    liqLabel->setStyleSheet("QLabel { color: #B6C2CF; padding-left: 4px; }");
    m_liqLabelAction = addWidget(liqLabel);

    // Liquidity labels on/off (S7b) and their currency.
    m_labelsButton = new QToolButton(this);
    m_labelsButton->setObjectName("labelsButton");
    m_labelsButton->setText("Labels");
    m_labelsButton->setCheckable(true);
    m_labelsButton->setChecked(true);
    m_labelsButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_labelsButton->setToolTip("Liquidity labels on the heatmap cells");
    m_labelsButton->setStyleSheet(
        "QToolButton#labelsButton { color: #8FA3B8; padding: 2px 8px; border-radius: 3px; }"
        "QToolButton#labelsButton:checked { background-color: #2B5A7A; color: #FFFFFF; }");
    m_labelsAction = addWidget(m_labelsButton);
    connect(m_labelsButton, &QToolButton::toggled, this, [this](bool on) {
        sLog_App("ui: toolbar labels=" << on);
        emit labelsToggled(on);
    });

    QLabel* modeLabel = new QLabel("Mode", this);
    modeLabel->setStyleSheet("QLabel { color: #B6C2CF; padding-left: 6px; }");
    m_modeLabelAction = addWidget(modeLabel);

    m_liquidityModeCombo = new QComboBox(this);
    m_liquidityModeCombo->setObjectName("liquidityModeCombo");
    m_liquidityModeCombo->addItems({"Asset", "USD"});
    m_liquidityModeCombo->setFixedWidth(80);
    m_liquidityModeCombo->setToolTip("Liquidity label currency");
    m_modeComboAction = addWidget(m_liquidityModeCombo);
    connect(m_liquidityModeCombo,
            QOverload<int>::of(&QComboBox::activated),
            this,
            &TopToolbar::liquidityLabelModeChanged);

    // GPU renderer: the two-handle range filter (replaces threshold/sensitivity).
    m_rangeSlider = new LiquidityRangeSlider(this);
    m_rangeSlider->setFixedSize(170, 22);
    m_rangeAction = addWidget(m_rangeSlider);
    connect(m_rangeSlider, &LiquidityRangeSlider::rangeEdited, this, [this](double low, double high, bool final) {
        m_rangeLabel->setText(QString::number(low, 'g', 3) + QStringLiteral(" - ") + QString::number(high, 'g', 3));
        emit liquidityRangeEdited(low, high, final);
    });
    m_rangeLabel = new QLabel(this);
    m_rangeLabel->setObjectName("liquidityRangeLabel");
    m_rangeLabel->setStyleSheet("QLabel { color: #8FA3B8; padding-left: 4px; }");
    m_rangeLabel->setMinimumWidth(72);
    m_rangeLabelAction = addWidget(m_rangeLabel);

    // Legacy renderer: the threshold slider.
    m_liquiditySlider = new QSlider(Qt::Horizontal, this);
    m_liquiditySlider->setObjectName("liquidityThresholdSlider");
    m_liquiditySlider->setRange(0, 1000);
    m_liquiditySlider->setFixedWidth(120);
    m_liquiditySlider->setToolTip("Liquidity threshold");
    m_thresholdAction = addWidget(m_liquiditySlider);
    connect(m_liquiditySlider, &QSlider::valueChanged, this, [this](int value) {
        emit liquidityThresholdChanged(static_cast<double>(value));
    });

    addSeparator();

    QAction* indicatorsAction = addIconAction(":/svg/indicators.svg", "Indicators", "Indicators");
    QAction* layoutsAction = addIconAction(":/svg/layout.svg", "Layouts", "Layouts");

    addSeparator();

    QAction* quickSearchAction = addIconAction(":/svg/search.svg", "Quick Search", "Quick Search");
    // Chart settings menu (gear): HeatmapChartControls fills it (one entry point).
    m_chartMenuButton = new QToolButton(this);
    m_chartMenuButton->setObjectName("chartMenuButton");
    m_chartMenuButton->setIcon(QIcon(":/svg/settings.svg"));
    m_chartMenuButton->setToolTip("Chart settings");
    m_chartMenuButton->setPopupMode(QToolButton::InstantPopup);
    m_chartMenu = new QMenu(m_chartMenuButton);
    m_chartMenu->setObjectName("chartMenu");
    m_chartMenuButton->setMenu(m_chartMenu);
    addWidget(m_chartMenuButton);
    QAction* fullscreenAction = addIconAction(":/svg/full_screen.svg", "Fullscreen", "Toggle Fullscreen");
    QAction* screenshotAction = addIconAction(":/svg/camera.svg", "Screenshot", "Screenshot");

    connect(indicatorsAction, &QAction::triggered, this, &TopToolbar::indicatorsRequested);
    connect(layoutsAction, &QAction::triggered, this, &TopToolbar::layoutsRequested);
    connect(quickSearchAction, &QAction::triggered, this, &TopToolbar::quickSearchRequested);
    connect(fullscreenAction, &QAction::triggered, this, &TopToolbar::fullscreenToggled);
    connect(screenshotAction, &QAction::triggered, this, &TopToolbar::screenshotRequested);
    setLiquidityRange(0, 0, m_rangeSlider->low(), m_rangeSlider->high());
    applyVisibility();
}

TopToolbar::ControlVisibility TopToolbar::controlVisibility(const ModeState &mode) {
    ControlVisibility v;
    v.tickSelector = mode.heatmap;
    v.palette = mode.heatmap;
    v.liquidity = mode.heatmap;
    v.rangeSlider = mode.heatmap && mode.gpu;
    v.thresholdSlider = mode.heatmap && !mode.gpu;
    v.candleStyle = mode.candles;
    v.tpoSession = mode.tpo || mode.volumeProfile;
    v.tpoLayout = mode.tpo;
    v.labelsToggle = mode.heatmap && mode.gpu;
    return v;
}

void TopToolbar::setModeState(const ModeState &mode) {
    m_mode = mode;
    if (m_candleAction && m_candleAction->isChecked() != mode.candles) {
        const QSignalBlocker block(m_candleAction);
        m_candleAction->setChecked(mode.candles);
    }
    applyVisibility();
}

void TopToolbar::applyVisibility() {
    const auto v = controlVisibility(m_mode);
    // A toolbar widget hides with its action; its own flag follows too, so
    // isVisibleTo() agrees before the toolbar lays out (or while it is hidden).
    auto show = [this](QAction *action, bool visible) {
        if (!action) return;
        action->setVisible(visible);
        if (QWidget *w = widgetForAction(action)) w->setVisible(visible);
    };
    show(m_tickModeAction, v.tickSelector);
    show(m_tickPresetAction, v.tickSelector);
    show(m_tickVeilAction, v.tickSelector && m_tickState.enabled && !m_tickState.indicator.isEmpty());
    show(m_paletteAction, v.palette);
    for (auto *a : {m_liqLabelAction, m_modeLabelAction, m_modeComboAction}) show(a, v.liquidity);
    show(m_labelsAction, v.labelsToggle);
    show(m_rangeAction, v.rangeSlider);
    show(m_rangeLabelAction, v.rangeSlider);
    show(m_thresholdAction, v.thresholdSlider);
    show(m_chartTypeAction, v.candleStyle);
    show(m_tpoSessionAction, v.tpoSession);
    show(m_tpoLayoutAction, v.tpoLayout);
}

TopToolbar::ControlVisibility TopToolbar::shownControls() const {
    auto on = [](const QAction *a) { return a && a->isVisible(); };
    ControlVisibility v;
    v.tickSelector = on(m_tickModeAction) && on(m_tickPresetAction);
    v.palette = on(m_paletteAction);
    v.liquidity = on(m_modeComboAction);
    v.labelsToggle = on(m_labelsAction);
    v.rangeSlider = on(m_rangeAction);
    v.thresholdSlider = on(m_thresholdAction);
    v.candleStyle = on(m_chartTypeAction);
    v.tpoSession = on(m_tpoSessionAction);
    v.tpoLayout = on(m_tpoLayoutAction);
    return v;
}

bool TopToolbar::candlesChecked() const { return m_candleAction && m_candleAction->isChecked(); }

void TopToolbar::setCandlesChecked(bool checked) {
    auto mode = m_mode;
    mode.candles = checked;
    setModeState(mode);
}

void TopToolbar::setLiquidityRange(double domainLo, double domainHi, double low, double high) {
    if (m_rangeSlider->dragging()) return; // the user's drag owns the handles
    m_rangeSlider->setDomain(domainLo, domainHi);
    m_rangeSlider->setValues(low, high);
    m_rangeLabel->setText(QString::number(m_rangeSlider->low(), 'g', 3) + QStringLiteral(" - ") +
                          QString::number(m_rangeSlider->high(), 'g', 3));
}

void TopToolbar::setLabelOptions(bool show, bool usd) {
    if (m_labelsButton->isChecked() != show) {
        const QSignalBlocker block(m_labelsButton);
        m_labelsButton->setChecked(show);
    }
    const int index = usd ? 1 : 0;
    if (m_liquidityModeCombo->currentIndex() != index) {
        const QSignalBlocker block(m_liquidityModeCombo);
        m_liquidityModeCombo->setCurrentIndex(index);
    }
}

void TopToolbar::setTpoState(int sessionType, const QString &layout) {
    const QSignalBlocker a(m_tpoSessionCombo), b(m_tpoLayoutCombo);
    if (const int i = m_tpoSessionCombo->findData(sessionType); i >= 0) m_tpoSessionCombo->setCurrentIndex(i);
    if (const int i = m_tpoLayoutCombo->findData(layout); i >= 0) m_tpoLayoutCombo->setCurrentIndex(i);
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
    applyVisibility(); // the veil label follows the indicator
    if (!state.enabled) {
        m_tickModeCombo->setToolTip(state.disabledReason);
        m_tickPresetCombo->setToolTip(state.disabledReason);
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
}

void TopToolbar::setColorPreset(const QString& preset) {
    if (!m_colorPresetCombo) return;
    const int index = m_colorPresetCombo->findText(preset);
    if (index < 0) return; // Custom: no combo entry
    const QSignalBlocker block(m_colorPresetCombo);
    m_colorPresetCombo->setCurrentIndex(index);
}
