#include "TopToolbar.hpp"
#include <QAction>
#include <QToolButton>
#include <QWidget>
#include <QHBoxLayout>
#include <QLabel>
#include <QIcon>
#include <QSlider>

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

    // Chart mode toggles (all independent, can combine)
    auto* candleAction = addAction(QIcon(":/svg/candlestick_chart.svg"), "Candles");
    candleAction->setCheckable(true);
    auto* heatmapAction = addAction(QIcon(":/svg/grid_view.svg"), "Heatmap");
    heatmapAction->setCheckable(true);
    heatmapAction->setChecked(true);
    auto* tpoAction = addAction(QIcon(":/svg/tpo_chart.svg"), "TPO");
    tpoAction->setCheckable(true);

    connect(candleAction, &QAction::triggered, this, [this]() { emit chartModeSelected(ChartMode::TRADITIONAL_CANDLES); });
    connect(heatmapAction, &QAction::triggered, this, [this]() { emit chartModeSelected(ChartMode::ORDER_BOOK_HEATMAP); });
    connect(tpoAction, &QAction::triggered, this, [this]() { emit chartModeSelected(ChartMode::HYBRID_CANDLES_TRADES); });

    addSeparator();

    m_timeframeCombo = new QComboBox(this);
    m_timeframeCombo->addItems({"1s", "1m", "5m", "15m", "1h", "4h", "1D"});
    m_timeframeCombo->setFixedWidth(70);
    addWidget(m_timeframeCombo);
    connect(m_timeframeCombo, &QComboBox::currentTextChanged, this, &TopToolbar::timeframeSelected);

    m_chartTypeCombo = new QComboBox(this);
    m_chartTypeCombo->addItems({"Candle", "Hollow", "Line"});
    m_chartTypeCombo->setFixedWidth(90);
    addWidget(m_chartTypeCombo);
    connect(m_chartTypeCombo, &QComboBox::currentTextChanged, this, &TopToolbar::chartTypeSelected);

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
            m_timeframeCombo->setCurrentIndex(idx);
        }
    }
}
