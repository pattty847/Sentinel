#include "TopToolbar.hpp"
#include "SentinelLogging.hpp"
#include <QAction>
#include <QActionEvent>
#include <QToolButton>
#include <QWidget>
#include <QHBoxLayout>
#include <QLabel>
#include <QIcon>
#include <QSlider>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <algorithm>
#include <tuple>
#include <QEvent>
#include <QTimer>
#include <QStyle>
#include <QWidgetAction>
#include <QSpinBox>

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

    m_symbolSearch = new QLineEdit(this);
    m_symbolSearch->setObjectName("chartSymbolSearch");
    m_symbolSearch->setAccessibleName("Chart symbol");
    m_symbolSearch->setPlaceholderText("Search Symbol");
    m_symbolSearch->setText("BTC-USD");
    m_symbolSearch->setFixedWidth(145);
    addWidget(m_symbolSearch);

    m_subscribeButton = new QToolButton(this);
    m_subscribeButton->setIcon(QIcon(":/svg/search.svg"));
    m_subscribeButton->setObjectName("chartSubscribeButton");
    m_subscribeButton->setToolTip("Show symbol chart");
    m_subscribeButton->setAccessibleName("Show symbol chart");
    addWidget(m_subscribeButton);
    connect(m_subscribeButton, &QToolButton::clicked, this, &TopToolbar::subscribeRequested);

    // A public Qt extension button opens the complete semantic chart menu.
    // QToolBar's embedded native extension cannot represent addWidget controls.
    m_controlsButton = new QToolButton(this);
    m_controlsButton->setObjectName("chartOverflowButton");
    m_controlsButton->setIcon(style()->standardIcon(QStyle::SP_ToolBarHorizontalExtensionButton));
    m_controlsButton->setToolTip("All chart controls (Shift+F10)");
    m_controlsButton->setAccessibleName("All chart controls");
    m_controlsButton->setAccessibleDescription("Chart layers, timeframe, tick, liquidity and workspace commands. Shift+F10.");
    m_controlsButton->setPopupMode(QToolButton::InstantPopup);
    m_controlsButton->setStyleSheet("QToolButton#chartOverflowButton::menu-indicator { image: none; width: 0px; }");
    m_controlsMenu = new QMenu(m_controlsButton);
    m_controlsMenu->setObjectName("chartControlsMenu");
    m_controlsMenu->setToolTipsVisible(true);
    // Only this menu tree: preserve the gear menu's existing theme policy.
    m_controlsMenu->setStyleSheet("QMenu::item:disabled { color: #8FA3B8; }");
    m_controlsButton->setMenu(m_controlsMenu);

    m_chartMenuButton = new QToolButton(this);
    m_chartMenuButton->setObjectName("chartMenuButton");
    m_chartMenuButton->setIcon(QIcon(":/svg/settings.svg"));
    m_chartMenuButton->setToolTip("Chart settings and appearance");
    m_chartMenuButton->setAccessibleName("Chart settings and appearance");
    m_chartMenuButton->setPopupMode(QToolButton::InstantPopup);
    m_chartMenu = new QMenu(m_chartMenuButton);
    m_chartMenu->setObjectName("chartMenu");
    m_chartMenu->setToolTipsVisible(true);
    m_chartMenu->installEventFilter(this);
    m_chartMenuButton->setMenu(m_chartMenu);
    addWidget(m_chartMenuButton);

    addSeparator();

    // Layer toggles: one primary field + independent overlays.
    auto* candleAction = addIconAction(":/svg/candlestick_chart.svg", "Candles", "Candles");
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

    // A text mark distinguishes volume profile from the TPO glyph at a glance.
    m_volumeProfileButton = addIconButton(QString(), "Volume profile");
    m_volumeProfileButton->setText("VP");
    m_volumeProfileButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_volumeProfileButton->setAccessibleName("Volume profile layer");
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
    m_timeframeCombo->setObjectName("chartTimeframeCombo");
    m_timeframeCombo->setAccessibleName("Chart timeframe");
    m_timeframeCombo->addItems({"1s", "1m", "5m", "15m", "1h", "4h", "1D"});
    const int64_t timeframesMs[] = {1000, 60000, 300000, 900000, 3600000, 14400000, 86400000};
    for (int i = 0; i < m_timeframeCombo->count(); ++i) {
        m_timeframeCombo->setItemData(i, static_cast<qlonglong>(timeframesMs[i]), Qt::UserRole);
    }
    m_timeframeCombo->setFixedWidth(70);
    addWidget(m_timeframeCombo);
    connect(m_timeframeCombo, &QComboBox::currentTextChanged, this, &TopToolbar::timeframeSelected);

    // Heatmap tick: Auto/Manual and the preset (S6c). Driven by setTickSelectorState.
    auto *tickLabel = new QLabel("Tick", this);
    tickLabel->setObjectName("chartTickLabel");
    m_tickLabelAction = addWidget(tickLabel);
    m_tickModeCombo = new QComboBox(this);
    m_tickModeCombo->setObjectName("tickModeCombo");
    m_tickModeCombo->addItems({"Auto", "Manual"});
    m_tickModeCombo->setFixedWidth(84);
    m_tickModeCombo->setAccessibleName("Heatmap tick mode");
    m_tickModeAction = addWidget(m_tickModeCombo);
    connect(m_tickModeCombo, QOverload<int>::of(&QComboBox::activated), this, [this](int index) {
        sLog_App("ui: toolbar tick mode=" << (index == 1 ? "manual" : "auto"));
        emit tickModeRequested(index == 1);
    });
    m_tickPresetCombo = new QComboBox(this);
    m_tickPresetCombo->setObjectName("tickPresetCombo");
    m_tickPresetCombo->setAccessibleName("Heatmap tick size");
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
    m_chartTypeCombo->setAccessibleName("Candle style");
    m_chartTypeCombo->setToolTip("Candle style");
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
    m_tpoSessionCombo->setAccessibleName("TPO and volume profile session");
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
    m_tpoLayoutCombo->setAccessibleName("TPO layout");
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
        refreshRangeLabel();
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

    m_indicatorsAction = addIconAction(":/svg/indicators.svg", "Indicators", "Indicators are not available on this chart yet");
    m_indicatorsAction->setObjectName("chartIndicatorsAction");
    m_indicatorsAction->setEnabled(false);
    if (auto *button = findChild<QToolButton *>("chartIndicatorsButton")) {
        button->setAccessibleDescription(m_indicatorsAction->toolTip());
        button->setAttribute(Qt::WA_AlwaysShowToolTips);
    }
    m_layoutsAction = addIconAction(":/svg/layout.svg", "Layouts", "Save, restore or reset a workspace layout");
    m_layoutsAction->setObjectName("chartLayoutsAction");

    addSeparator();

    QAction* quickSearchAction = addIconAction(":/svg/search.svg", "Quick Search", "Focus symbol search");
    quickSearchAction->setObjectName("controlsQuickSearch");
    m_quickSearchAction = quickSearchAction;
    m_fullscreenAction = addIconAction(":/svg/full_screen.svg", "Fullscreen", "Toggle fullscreen (F11)");
    m_fullscreenAction->setObjectName("chartFullscreenAction");
    m_fullscreenAction->setCheckable(true);
    // F11 is already registered by ShortcutBinder; adding it here would make
    // both shortcuts ambiguous. Window-state changes update this action.
    QAction* screenshotAction = addIconAction(":/svg/camera.svg", "Screenshot", "Screenshot");
    screenshotAction->setObjectName("chartScreenshotAction");
    m_screenshotAction = screenshotAction;

    connect(m_layoutsAction, &QAction::triggered, this, &TopToolbar::layoutsRequested);
    connect(quickSearchAction, &QAction::triggered, this, &TopToolbar::quickSearchRequested);
    connect(m_fullscreenAction, &QAction::triggered, this, &TopToolbar::fullscreenToggled);
    connect(screenshotAction, &QAction::triggered, this, &TopToolbar::screenshotRequested);
    connect(m_controlsMenu, &QMenu::aboutToShow, this, &TopToolbar::refreshControlsMenu);
    buildControlsMenu();
    m_inlineActions = actions();
    // Collapse labels together with their inputs, and preserve the original order.
    for (int i = 0; i < m_inlineActions.size(); ++i) {
        QList<QAction*> group{m_inlineActions[i]};
        QAction *last = m_inlineActions[i] == m_tickLabelAction ? m_tickVeilAction
                      : m_inlineActions[i] == m_liqLabelAction ? m_thresholdAction : nullptr;
        if (last) while (group.back() != last && i + 1 < m_inlineActions.size()) group << m_inlineActions[++i];
        m_inlineGroups << group;
        for (auto *action : group) m_modeVisibility.insert(action, true);
    }
    auto *spacer = new QWidget(this);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_spacerAction = addWidget(spacer);
    m_overflowAction = addWidget(m_controlsButton);
    m_spacerAction->setVisible(false);
    m_overflowAction->setVisible(false);
    setLiquidityRange(0, 0, m_rangeSlider->low(), m_rangeSlider->high());
    applyVisibility();
}

void TopToolbar::buildControlsMenu() {
    auto layer = [this](const char *name, const QString &text, QAbstractButton *button) {
        QAction *action = m_controlsMenu->addAction(text);
        action->setObjectName(name);
        action->setCheckable(true);
        connect(action, &QAction::triggered, this, [button] { button->click(); });
    };
    QAction *candles = m_controlsMenu->addAction("Candles");
    candles->setObjectName("controlsCandles");
    candles->setCheckable(true);
    connect(candles, &QAction::triggered, m_candleAction, &QAction::trigger);
    layer("controlsHeatmap", "Heatmap", m_heatmapButton);
    layer("controlsFootprint", "Footprint", m_footprintButton);
    layer("controlsTpo", "TPO", m_tpoButton);
    layer("controlsVolumeProfile", "Volume profile", m_volumeProfileButton);
    m_controlsMenu->addSeparator();

    QMenu *timeframes = m_controlsMenu->addMenu("Timeframe");
    timeframes->setObjectName("controlsTimeframes");
    for (int i = 0; i < m_timeframeCombo->count(); ++i) {
        QAction *action = timeframes->addAction(m_timeframeCombo->itemText(i));
        action->setObjectName("controlsTimeframe");
        action->setData(i);
        action->setCheckable(true);
        connect(action, &QAction::triggered, this, [this, i] { m_timeframeCombo->setCurrentIndex(i); });
    }
    QMenu *styles = m_controlsMenu->addMenu("Candle style");
    styles->setObjectName("controlsCandleStyles");
    for (int i = 0; i < m_chartTypeCombo->count(); ++i) {
        QAction *action = styles->addAction(m_chartTypeCombo->itemText(i));
        action->setObjectName("controlsCandleStyle");
        action->setData(i);
        action->setCheckable(true);
        connect(action, &QAction::triggered, this, [this, i] { m_chartTypeCombo->setCurrentIndex(i); });
    }
    auto comboMenu = [this](const QString &title, const char *name, QComboBox *combo) {
        auto *menu = m_controlsMenu->addMenu(title);
        menu->setObjectName(name);
        for (int i = 0; i < combo->count(); ++i) {
            auto *action = menu->addAction(combo->itemText(i));
            action->setData(i);
            action->setCheckable(true);
            connect(action, &QAction::triggered, this, [combo, i] {
                combo->setCurrentIndex(i);
                emit combo->activated(i);
            });
        }
    };
    comboMenu("TPO / volume profile session", "controlsTpoSession", m_tpoSessionCombo);
    comboMenu("TPO layout", "controlsTpoLayout", m_tpoLayoutCombo);
    m_tickMenu = m_controlsMenu->addMenu("Heatmap tick");
    m_tickMenu->setObjectName("controlsTick");
    m_rangeMenuAction = m_controlsMenu->addAction("Edit liquidity range...");
    m_rangeMenuAction->setObjectName("controlsLiquidityRange");
    connect(m_rangeMenuAction, &QAction::triggered, this, &TopToolbar::liquidityRangeSettingsRequested);
    m_legacyThresholdMenu = m_controlsMenu->addMenu("Liquidity threshold");
    m_legacyThresholdMenu->setObjectName("controlsLegacyThreshold");
    auto *row = new QWidget(m_legacyThresholdMenu);
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->addWidget(new QLabel("Filter strength", row));
    auto *strength = new QSpinBox(row);
    strength->setObjectName("controlsThresholdStrength");
    strength->setAccessibleName("Liquidity filter strength");
    strength->setRange(m_liquiditySlider->minimum(), m_liquiditySlider->maximum());
    strength->setToolTip("0 disables the filter; higher values hide smaller liquidity. Uses the same scale as the toolbar slider.");
    rowLayout->addWidget(strength);
    auto *editor = new QWidgetAction(m_legacyThresholdMenu);
    editor->setDefaultWidget(row);
    m_legacyThresholdMenu->addAction(editor);
    connect(strength, QOverload<int>::of(&QSpinBox::valueChanged), m_liquiditySlider, &QSlider::setValue);
    connect(m_liquiditySlider, &QSlider::valueChanged, strength, &QSpinBox::setValue);
    m_labelsMenu = m_controlsMenu->addMenu("Liquidity labels");
    m_labelsMenu->setObjectName("controlsLabels");
    m_controlsMenu->addSeparator();
    m_controlsMenu->addAction(m_indicatorsAction);
    m_layoutsMenu = m_controlsMenu->addMenu("Layouts");
    m_layoutsMenu->setObjectName("controlsLayouts");
    m_controlsMenu->addAction(m_quickSearchAction);
    m_controlsMenu->addAction(m_fullscreenAction);
    m_controlsMenu->addAction(m_screenshotAction);
    refreshControlsMenu();
}

void TopToolbar::refreshControlsMenu() {
    auto action = [this](const char *name) { return m_controlsMenu->findChild<QAction *>(name); };
    // The gear owns these actions and their persisted settings. Refresh them
    // before sharing them; clearing this menu does not delete externally-owned
    // actions, including after the host rebuilds its hooks.
    emit m_chartMenu->aboutToShow();
    m_labelsMenu->clear();
    if (m_mode.gpu)
        for (auto *entry : m_chartMenu->actions())
            if (entry->objectName() == QLatin1String("chartMenuLabels")) m_labelsMenu->addAction(entry);
    for (const auto *name : {"chartMenuCurrency", "chartMenuLabelSize"}) {
        if (!m_mode.gpu && QString::fromLatin1(name) == QLatin1String("chartMenuLabelSize")) continue;
        if (auto *menu = m_chartMenu->findChild<QMenu *>(name)) m_labelsMenu->addMenu(menu);
    }
    m_labelsMenu->menuAction()->setEnabled(m_mode.heatmap && !m_labelsMenu->isEmpty());
    m_labelsMenu->menuAction()->setToolTip("Liquidity label currency, with visibility and size on the GPU heatmap");
    m_layoutsMenu->clear();
    if (auto *layouts = m_chartMenu->findChild<QMenu *>("chartMenuLayouts"))
        for (auto *entry : layouts->actions()) m_layoutsMenu->addAction(entry);
    m_layoutsMenu->menuAction()->setEnabled(!m_layoutsMenu->isEmpty());
    m_legacyThresholdMenu->menuAction()->setEnabled(m_mode.heatmap && !m_mode.gpu);
    m_legacyThresholdMenu->menuAction()->setVisible(!m_mode.gpu);
    action("controlsCandles")->setChecked(candlesChecked());
    action("controlsHeatmap")->setChecked(m_heatmapButton->isChecked());
    action("controlsFootprint")->setChecked(m_footprintButton->isChecked());
    action("controlsTpo")->setChecked(m_tpoButton->isChecked());
    action("controlsVolumeProfile")->setChecked(m_volumeProfileButton->isChecked());
    for (QAction *a : m_controlsMenu->findChild<QMenu *>("controlsTimeframes")->actions()) {
        const int i = a->data().toInt();
        auto *model = qobject_cast<QStandardItemModel *>(m_timeframeCombo->model());
        const bool enabled = !model || model->item(i)->isEnabled();
        a->setEnabled(enabled);
        a->setChecked(i == m_timeframeCombo->currentIndex());
        a->setToolTip(m_timeframeCombo->itemData(i, Qt::ToolTipRole).toString());
    }
    m_controlsMenu->findChild<QMenu *>("controlsCandleStyles")->menuAction()->setEnabled(
        m_mode.candles && m_chartTypeCombo->isEnabled());
    for (QAction *a : m_controlsMenu->findChild<QMenu *>("controlsCandleStyles")->actions())
        a->setChecked(a->data().toInt() == m_chartTypeCombo->currentIndex());
    const auto visibility = controlVisibility(m_mode);
    for (const auto &[name, combo, active] : {
             std::tuple{"controlsTpoSession", m_tpoSessionCombo, visibility.tpoSession},
             std::tuple{"controlsTpoLayout", m_tpoLayoutCombo, visibility.tpoLayout}}) {
        auto *menu = m_controlsMenu->findChild<QMenu *>(name);
        menu->menuAction()->setEnabled(active && combo->isEnabled());
        menu->menuAction()->setToolTip(!active ? "Enable the corresponding chart layer to use this control"
                                              : !combo->isEnabled() ? "No chart renderer is attached" : combo->toolTip());
        for (auto *entry : menu->actions()) {
            entry->setChecked(entry->data().toInt() == combo->currentIndex());
            entry->setEnabled(active && combo->isEnabled());
        }
    }
    m_tickMenu->menuAction()->setEnabled(m_mode.heatmap && m_mode.gpu && m_tickState.enabled);
    m_tickMenu->menuAction()->setToolTip(!m_mode.heatmap ? QStringLiteral("Enable the heatmap layer to change its tick")
        : !m_mode.gpu ? QStringLiteral("Tick selection requires the GPU heatmap")
        : m_tickState.enabled ? m_tickState.indicator : m_tickState.disabledReason);
    m_tickMenu->clear();
    QAction *autoTick = m_tickMenu->addAction("Auto tick");
    autoTick->setObjectName("controlsTickAuto");
    autoTick->setCheckable(true);
    autoTick->setChecked(!m_tickState.manual);
    connect(autoTick, &QAction::triggered, this, [this] { emit tickModeRequested(false); });
    QAction *manualTick = m_tickMenu->addAction("Manual tick");
    manualTick->setObjectName("controlsTickManual");
    manualTick->setCheckable(true);
    manualTick->setChecked(m_tickState.manual);
    connect(manualTick, &QAction::triggered, this, [this] { emit tickModeRequested(true); });
    m_tickMenu->addSeparator();
    // Mirror the wide selector, including a retained current tick that loaded
    // data cannot build. It remains visible and checked, but cannot be selected.
    for (int i = 0; i < m_tickPresetCombo->count(); ++i) {
        const int64_t units = m_tickPresetCombo->itemData(i).toLongLong();
        QAction *preset = m_tickMenu->addAction(m_tickPresetCombo->itemText(i));
        preset->setObjectName("controlsTickPreset");
        preset->setData(qlonglong(units));
        preset->setCheckable(true);
        preset->setChecked(i == m_tickPresetCombo->currentIndex());
        preset->setEnabled(m_tickPresetCombo->model()->flags(m_tickPresetCombo->model()->index(i, 0)) & Qt::ItemIsEnabled);
        preset->setToolTip(m_tickPresetCombo->itemData(i, Qt::ToolTipRole).toString());
        connect(preset, &QAction::triggered, this, [this, units] { emit tickPresetRequested(units); });
    }
    if (!m_tickState.indicator.isEmpty()) {
        m_tickMenu->addSeparator();
        QAction *indicator = m_tickMenu->addAction(m_tickState.indicator);
        indicator->setObjectName("controlsTickIndicator");
        indicator->setToolTip(m_tickState.indicator);
        indicator->setEnabled(false);
    }
    m_rangeMenuAction->setEnabled(m_mode.heatmap && m_mode.gpu);
    m_rangeMenuAction->setVisible(m_mode.gpu);
    m_rangeMenuAction->setToolTip(m_mode.gpu ? QStringLiteral("Edit base-asset cell sizes in Chart settings")
                                              : QStringLiteral("Liquidity range is available with the GPU heatmap"));
}

void TopToolbar::refreshRangeLabel() {
    if (!m_rangeLabel || !m_rangeSlider) return;
    const QString range = QString::number(m_rangeSlider->low(), 'g', 3) + QStringLiteral(" - ") +
                          QString::number(m_rangeSlider->high(), 'g', 3) + QStringLiteral(" ") + m_baseAssetSymbol;
    m_rangeLabel->setText(range);
    m_rangeLabel->setToolTip(QStringLiteral("Base-asset size per heatmap cell: %1").arg(range));
    if (m_rangeMenuAction) m_rangeMenuAction->setText(QStringLiteral("Liquidity range: %1...").arg(range));
}

void TopToolbar::setBaseAssetSymbol(const QString &symbol) {
    const QString base = symbol.section('-', 0, 0).trimmed().toUpper();
    if (base.isEmpty() || base == m_baseAssetSymbol) return;
    m_baseAssetSymbol = base;
    m_rangeSlider->setBaseUnit(base);
    refreshRangeLabel();
}

void TopToolbar::showLayoutsMenu() {
    QMenu *layouts = m_chartMenu->findChild<QMenu *>("chartMenuLayouts");
    if (!layouts) return;
    layouts->popup(mapToGlobal(QPoint(width() - layouts->sizeHint().width(), height())));
}

void TopToolbar::showControlsMenu() {
    m_controlsMenu->popup(mapToGlobal(QPoint(width() - m_controlsMenu->sizeHint().width(), height())));
    for (QAction *action : m_controlsMenu->actions()) {
        if (action->isVisible() && action->isEnabled() && !action->isSeparator()) {
            m_controlsMenu->setActiveAction(action);
            break;
        }
    }
}

bool TopToolbar::eventFilter(QObject *watched, QEvent *event) {
    if (watched == m_chartMenu && event->type() == QEvent::ActionRemoved && m_labelsMenu) {
        // When host hooks rebuild the gear menu, release shared direct actions
        // before QMenu::clear decides ownership/deletion. Otherwise old labels
        // actions remain associated with overflow and survive the rebuild.
        m_labelsMenu->removeAction(static_cast<QActionEvent *>(event)->action());
    }
    return QToolBar::eventFilter(watched, event);
}

bool TopToolbar::event(QEvent *event) {
    const bool result = QToolBar::event(event);
    switch (event->type()) {
    case QEvent::Resize:
    case QEvent::Show:
    case QEvent::LayoutRequest:
    case QEvent::FontChange:
    case QEvent::ApplicationFontChange:
    case QEvent::StyleChange:
        scheduleFit();
        break;
    default: break;
    }
    return result;
}

void TopToolbar::scheduleFit() {
    if (!m_overflowAction || m_fitPending || m_fitting) return;
    m_fitPending = true;
    QTimer::singleShot(0, this, [this] {
        m_fitPending = false;
        fitControls();
    });
}

void TopToolbar::setInlineVisible(QAction *action, bool visible) {
    // QWidgetAction propagates its hidden/disabled state to its default widget.
    // Preserve that adapter's actual availability: overflow commands still use
    // it, and the chart controller owns enabled state independently of width.
    QWidget *widget = widgetForAction(action);
    const bool enabled = widget && widget->isEnabled();
    action->setVisible(visible);
    if (widget) widget->setEnabled(enabled);
}

void TopToolbar::fitControls() {
    if (!isVisible() || m_fitting) return;
    // Hidden widgets retain their size hints. Compare those and intended mode
    // visibility, never the result of the previous responsive layout. Our own
    // action changes post LayoutRequest too; unchanged inputs end that cycle.
    QVector<int> inputs{width(), layout()->contentsMargins().left(), layout()->contentsMargins().right(),
                        style()->pixelMetric(QStyle::PM_ToolBarItemSpacing), m_controlsButton->sizeHint().width()};
    for (QAction *action : m_inlineActions) {
        const auto *widget = widgetForAction(action);
        const QSize hint = widget ? widget->sizeHint().expandedTo(widget->minimumSize()).boundedTo(widget->maximumSize()) : QSize();
        inputs << int(m_modeVisibility.value(action, true)) << hint.width() << hint.height();
    }
    if (inputs == m_fitInputs) return;
    m_fitInputs = inputs;
    m_fitting = true;
    m_spacerAction->setVisible(false);
    m_overflowAction->setVisible(false);
    for (auto *action : m_inlineActions) setInlineVisible(action, m_modeVisibility.value(action, true));
    layout()->invalidate();
    if (QToolBar::sizeHint().width() > width()) {
        m_spacerAction->setVisible(true);
        m_overflowAction->setVisible(true);
        // The first three groups are symbol, submit and gear. All other groups
        // move as a suffix; the complete menu remains available regardless.
        for (int i = m_inlineGroups.size() - 1; i >= 3 && QToolBar::sizeHint().width() > width(); --i) {
            for (auto *action : m_inlineGroups[i]) setInlineVisible(action, false);
            // Avoid an orphan trailing separator before the extension.
            for (int j = i - 1; j >= 3; --j) {
                auto *last = m_inlineGroups[j].back();
                if (!last->isVisible()) continue;
                if (last->isSeparator()) setInlineVisible(last, false);
                break;
            }
            layout()->invalidate();
        }
    }
    layout()->activate();
    m_fitting = false;
}

void TopToolbar::setFullscreen(bool fullscreen) {
    const QSignalBlocker block(m_fullscreenAction);
    m_fullscreenAction->setChecked(fullscreen);
    m_fullscreenAction->setToolTip(fullscreen ? "Exit fullscreen (F11)" : "Enter fullscreen (F11)");
}

TopToolbar::ControlVisibility TopToolbar::controlVisibility(const ModeState &mode) {
    ControlVisibility v;
    v.tickSelector = mode.heatmap;
    // Appearance lives in the gear menu. Retain the selectors as model adapters
    // for existing callers, without consuming primary toolbar width.
    v.rangeSlider = mode.heatmap && mode.gpu;
    v.thresholdSlider = mode.heatmap && !mode.gpu;
    v.candleStyle = mode.candles;
    v.tpoSession = mode.tpo || mode.volumeProfile;
    v.tpoLayout = mode.tpo;
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
        m_modeVisibility.insert(action, visible);
        if (!isVisible()) {
            setInlineVisible(action, visible);
            if (QWidget *w = widgetForAction(action)) w->setVisible(visible);
        }
    };
    show(m_tickModeAction, v.tickSelector);
    show(m_tickLabelAction, v.tickSelector);
    show(m_tickPresetAction, v.tickSelector);
    show(m_tickVeilAction, v.tickSelector && m_tickState.enabled && !m_tickState.indicator.isEmpty());
    show(m_paletteAction, v.palette);
    show(m_liqLabelAction, v.rangeSlider || v.thresholdSlider);
    for (auto *a : {m_modeLabelAction, m_modeComboAction}) show(a, v.liquidity);
    show(m_labelsAction, v.labelsToggle);
    show(m_rangeAction, v.rangeSlider);
    show(m_rangeLabelAction, v.rangeSlider);
    show(m_thresholdAction, v.thresholdSlider);
    show(m_chartTypeAction, v.candleStyle);
    show(m_tpoSessionAction, v.tpoSession);
    show(m_tpoLayoutAction, v.tpoLayout);
    scheduleFit();
}

TopToolbar::ControlVisibility TopToolbar::shownControls() const {
    auto on = [this](QAction *a) { return a && m_modeVisibility.value(a, a->isVisible()); };
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
    refreshRangeLabel();
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
    // The command can be shared with menus. Its independent widget action may
    // hide for overflow without changing that command's visibility or state.
    auto *action = new QAction(QIcon(iconPath), text, this);
    action->setToolTip(tooltip);
    auto *button = new QToolButton(this);
    button->setDefaultAction(action);
    button->setIconSize(iconSize());
    button->setToolButtonStyle(toolButtonStyle());
    if (text == QLatin1String("Indicators")) button->setObjectName("chartIndicatorsButton");
    addWidget(button);
    return action;
}

QToolButton* TopToolbar::addIconButton(const QString& iconPath, const QString& tooltip) {
    QToolButton* button = new QToolButton(this);
    button->setIcon(QIcon(iconPath));
    button->setToolTip(tooltip);
    button->setAccessibleName(tooltip);
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
