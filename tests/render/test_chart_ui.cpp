// Slice S7b (labels and the liquidity range on the GPU main chart) and the chart
// UI follow-ups (owner request 2026-10-02): the toolbar adapts to the chart's
// layers, the chart settings menu and dialog Chart tab round-trip their settings.
//
// GPU cases run a UnifiedGridRenderer over the synthetic HMC2 recording (as
// test_ugr_gpu) on the selected QRhi backend and skip with the reason when it
// cannot create one: a skipped case is no result. Toolbar/menu cases need no GPU.
// Every QSettings is a temporary INI file (never the owner's).
#include "UnifiedGridRenderer.h"
#include "models/PriceAxisModel.hpp"
#include "models/TimeAxisModel.hpp"
#include "lab/LabData.hpp"
#include "lab/OffscreenQuick.hpp"
#include "lab/RhiBackend.hpp"
#include "mainwindow/HeatmapChartControls.hpp"
#include "render/heatmap/HeatmapGpuLayer.hpp"
#include "render/heatmap/HeatmapSettingsModel.hpp"
#include "widgets/HeatmapSettingsDialog.hpp"
#include "widgets/TopToolbar.hpp"
#include "widgets/ChartDock.hpp"
#include "themes/ThemeManager.hpp"
#include "themes/FontManager.hpp"
#include "SyntheticHmc2Fixture.hpp"
#include <QAction>
#include <QApplication>
#include <QMainWindow>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QVBoxLayout>
#include <QToolButton>
#include <QPainter>
#include <QElapsedTimer>
#include <QMenu>
#include <QQuickWindow>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QKeyEvent>
#include <QSettings>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QTimer>
#include <QSpinBox>
#include <QShortcut>
#include <QSignalSpy>
#include <QPushButton>
#include <QStyleOptionSlider>
#include <QWidgetAction>
#include <QPointer>
#include <gtest/gtest.h>
#include <iostream>
#include <algorithm>

void qml_register_types_Sentinel_Charts();

namespace {
using namespace synthetic_hmc2;
using heatmap::HeatmapSettingsModel;
using heatmap::HeatmapSettingsStore;
using Mode = TopToolbar::ModeState;
using Shown = TopToolbar::ControlVisibility;

struct TempStore {
    QTemporaryDir dir;
    QString path = dir.filePath("settings.ini");
    QSettings settings{path, QSettings::IniFormat};
    HeatmapSettingsStore store{settings};
    ClientHeatmapConfig config;
    heatmap::HeatmapChartSettings reload() {
        settings.sync();
        QSettings again(path, QSettings::IniFormat);
        return HeatmapSettingsStore(again).load("main", config);
    }
};

QAction *menuAction(QMenu *menu, const QString &name, int nth = 0) {
    int seen = 0;
    std::function<QAction *(QMenu *)> find = [&](QMenu *parent) -> QAction * {
        for (QAction *action : parent->actions()) {
            if (action->objectName() == name && seen++ == nth) return action;
            if (action->menu()) if (auto *match = find(action->menu())) return match;
        }
        return nullptr;
    };
    if (auto *action = find(menu)) return action;
    ADD_FAILURE() << "no menu action " << name.toStdString() << " #" << nth;
    return nullptr;
}

// Exercise the real button, including Qt's InstantPopup event loop. The timer
// inspects the open popup and always closes it before mouseClick returns.
void useOverflow(TopToolbar &toolbar, const std::function<void(QMenu *)> &inspect) {
    ASSERT_TRUE(toolbar.controlsButton()->isVisible()) << "width=" << toolbar.width() << " minimum=" << toolbar.minimumWidth() << " hint=" << toolbar.sizeHint().width();
    toolbar.window()->raise();
    toolbar.window()->activateWindow();
    ASSERT_TRUE(QTest::qWaitForWindowActive(toolbar.window(), 2000));
    bool entered = false;
    QTimer inspectTimer;
    inspectTimer.setSingleShot(true);
    QObject::connect(&inspectTimer, &QTimer::timeout, &toolbar, [&] {
        entered = true;
        auto *menu = toolbar.controlsButton()->menu();
        EXPECT_TRUE(menu->isVisible());
        if (menu->isVisible()) inspect(menu);
        for (auto *child : menu->findChildren<QMenu *>()) child->hide();
        menu->hide();
    });
    inspectTimer.start(30);
    QTest::mouseClick(toolbar.controlsButton(), Qt::LeftButton);
    if (!entered) QTest::qWait(50);
    EXPECT_TRUE(entered);
}

void useGear(TopToolbar &toolbar, const std::function<void(QMenu *)> &inspect) {
    bool entered = false;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &toolbar, [&] {
        entered = true;
        auto *menu = toolbar.chartMenu();
        EXPECT_TRUE(menu->isVisible());
        if (menu->isVisible()) inspect(menu);
        for (auto *child : menu->findChildren<QMenu *>()) child->hide();
        menu->hide();
    });
    timer.start(30);
    QTest::mouseClick(toolbar.chartMenuButton(), Qt::LeftButton);
    if (!entered) QTest::qWait(50);
    EXPECT_TRUE(entered);
}

// Use the separate complete menu for keyboard-only settings coverage.
void useCompleteMenu(TopToolbar &toolbar, const std::function<void(QMenu *)> &inspect) {
    toolbar.showControlsMenu();
    QCoreApplication::processEvents();
    ASSERT_TRUE(toolbar.controlsMenu()->isVisible());
    inspect(toolbar.controlsMenu());
    toolbar.controlsMenu()->hide();
}

QStringList menuEntries(QMenu *menu) {
    QStringList entries;
    for (auto *action : menu->actions())
        if (action->isVisible()) entries << (action->isSeparator() ? QStringLiteral("|") : action->text());
    return entries;
}

int widthForOneHiddenControl(TopToolbar &toolbar) {
    toolbar.resize(1920, 44);
    toolbar.show();
    QTest::qWait(20);
    const int full = toolbar.sizeHint().width();
    for (int width = full; width > full - 150; --width) {
        toolbar.resize(width, 44);
        QCoreApplication::processEvents();
        if (toolbar.controlsButton()->isVisible() && menuEntries(toolbar.controlsButton()->menu()).size() == 1)
            return width;
    }
    return -1;
}

void clickSubmenuEntry(QMenu *root, QMenu *submenu, QAction *entry) {
    ASSERT_TRUE(submenu);
    ASSERT_TRUE(entry);
    ASSERT_TRUE(submenu->menuAction()->isEnabled());
    root->setActiveAction(submenu->menuAction());
    QTest::keyClick(root, Qt::Key_Right);
    QCoreApplication::processEvents();
    ASSERT_TRUE(submenu->isVisible());
    submenu->setActiveAction(entry);
    QTest::keyClick(submenu, Qt::Key_Return);
}

// Deliver keys to the focused widget, rather than directly to an unfocused
// QMenu: this catches the embedded QWindow popup focus failure.
void exerciseMenuKeyboard(QMenu *root) {
    ASSERT_TRUE(root->isEnabled()) << "popup availability must not depend on the overflow button";
    ASSERT_TRUE(QTest::qWaitFor([&] { return root->hasFocus(); }, 1000));
    auto press = [](Qt::Key key) {
        auto *target = QApplication::focusWidget();
        ASSERT_TRUE(target);
        QTest::keyClick(target, key);
        QTest::qWait(10);
    };
    auto *first = root->activeAction();
    ASSERT_TRUE(first);
    press(Qt::Key_Down);
    EXPECT_NE(root->activeAction(), first);
    press(Qt::Key_Up);
    EXPECT_EQ(root->activeAction(), first);
    press(Qt::Key_S);
    EXPECT_NE(root->activeAction(), first);
    press(Qt::Key_W);
    EXPECT_EQ(root->activeAction(), first);
    auto *submenu = root->findChild<QMenu *>("controlsTimeframes");
    ASSERT_TRUE(submenu);
    root->setActiveAction(submenu->menuAction());
    press(Qt::Key_Right);
    ASSERT_TRUE(QTest::qWaitFor([&] { return submenu->isVisible() && submenu->hasFocus(); }, 1000));
    auto *subFirst = submenu->activeAction();
    ASSERT_TRUE(subFirst);
    press(Qt::Key_S);
    EXPECT_NE(submenu->activeAction(), subFirst);
    press(Qt::Key_W);
    EXPECT_EQ(submenu->activeAction(), subFirst);
    press(Qt::Key_A);
    EXPECT_FALSE(submenu->isVisible());
    ASSERT_TRUE(QTest::qWaitFor([&] { return root->hasFocus(); }, 1000));
    press(Qt::Key_D);
    ASSERT_TRUE(QTest::qWaitFor([&] { return submenu->isVisible() && submenu->hasFocus(); }, 1000));
    press(Qt::Key_Escape);
    EXPECT_FALSE(submenu->isVisible());
    ASSERT_TRUE(QTest::qWaitFor([&] { return root->hasFocus(); }, 1000));
    press(Qt::Key_Right);
    ASSERT_TRUE(QTest::qWaitFor([&] { return submenu->isVisible() && submenu->hasFocus(); }, 1000));
    auto *chosen = submenu->activeAction();
    ASSERT_TRUE(chosen);
    QSignalSpy triggered(chosen, &QAction::triggered);
    press(Qt::Key_Return);
    EXPECT_EQ(triggered.size(), 1);
    EXPECT_FALSE(root->isVisible());
}

// ------------------------------------------------------------ toolbar modes
// The rules live in one place (TopToolbar::controlVisibility).
TEST(ChartToolbar, ControlVisibilityRulesPerMode) {
    struct Case {
        const char *what;
        Mode mode;
        Shown shown;
    };
    const std::vector<Case> cases{
        {"heatmap gpu + candles", {true, false, false, false, true, true},
         {true, false, false, true, false, true, false, false, false}},
        {"heatmap legacy (no Labels toggle: legacy always labels)", {true, false, false, false, true, false},
         {true, false, false, false, true, true, false, false, false}},
        {"tpo", {false, false, true, false, true, true}, {false, false, false, false, false, true, true, true, false}},
        {"volume profile", {false, false, false, true, false, true},
         {false, false, false, false, false, false, true, false, false}},
        {"footprint only", {false, true, false, false, false, true},
         {false, false, false, false, false, false, false, false}},
        {"heatmap + footprint, no candles", {true, true, false, false, false, true},
         {true, false, false, true, false, false, false, false, false}},
    };
    TopToolbar toolbar;
    for (const auto &c : cases) {
        SCOPED_TRACE(c.what);
        EXPECT_EQ(TopToolbar::controlVisibility(c.mode), c.shown);
        toolbar.setModeState(c.mode);
        EXPECT_EQ(toolbar.shownControls(), c.shown) << "the toolbar applies the rules";
        EXPECT_EQ(toolbar.tpoSessionCombo()->isVisibleTo(&toolbar), c.shown.tpoSession);
        EXPECT_EQ(toolbar.rangeSlider()->isVisibleTo(&toolbar), c.shown.rangeSlider);
        EXPECT_EQ(toolbar.tickModeCombo()->isVisibleTo(&toolbar), c.shown.tickSelector);
        EXPECT_EQ(toolbar.labelsButton()->isVisibleTo(&toolbar), c.shown.labelsToggle);
        EXPECT_EQ(toolbar.liquidityModeCombo()->isVisibleTo(&toolbar), c.shown.liquidity);
        EXPECT_EQ(toolbar.candlesChecked(), c.mode.candles);
    }
}

TEST(ChartToolbar, NarrowChartRetainsPrimaryEntryPointsAndHonestUnavailableAction) {
    TopToolbar toolbar;
    toolbar.resize(420, 42);
    toolbar.show();
    QCoreApplication::processEvents();
    ASSERT_TRUE(toolbar.controlsButton()->isVisible());
    ASSERT_TRUE(toolbar.chartMenuButton()->isVisible());
    EXPECT_LT(toolbar.controlsButton()->geometry().right(), toolbar.width());
    EXPECT_LT(toolbar.chartMenuButton()->geometry().right(), toolbar.width());
    auto *indicators = toolbar.findChild<QAction *>("chartIndicatorsAction");
    ASSERT_TRUE(indicators);
    EXPECT_FALSE(indicators->isEnabled());
    EXPECT_TRUE(indicators->toolTip().contains("not available"));
    const auto vp = toolbar.findChildren<QToolButton *>();
    EXPECT_TRUE(std::any_of(vp.begin(), vp.end(), [](QToolButton *button) {
        return button->text() == "VP" && button->toolTip() == "Volume profile";
    }));
}

TEST(ChartToolbar, WideChartNamesTickAndSeparatesAppearance) {
    TopToolbar toolbar;
    toolbar.resize(1920, 42);
    toolbar.show();
    QCoreApplication::processEvents();
    auto *tick = toolbar.findChild<QLabel *>("chartTickLabel");
    ASSERT_TRUE(tick);
    EXPECT_TRUE(tick->isVisible());
    EXPECT_EQ(tick->text(), "Tick");
    EXPECT_FALSE(toolbar.findChild<QComboBox *>("colorPresetCombo")->isVisible());
    EXPECT_FALSE(toolbar.liquidityModeCombo()->isVisible());
    EXPECT_FALSE(toolbar.labelsButton()->isVisible());
    auto *fullscreen = toolbar.findChild<QAction *>("chartFullscreenAction");
    ASSERT_TRUE(fullscreen);
    EXPECT_TRUE(toolbar.controlsMenu()->actions().contains(fullscreen));
    toolbar.setFullscreen(true);
    EXPECT_TRUE(fullscreen->isChecked());
    EXPECT_TRUE(fullscreen->toolTip().contains("Exit"));
    toolbar.setFullscreen(false);
    EXPECT_FALSE(fullscreen->isChecked());
}

TEST(ChartToolbar, ControlsMenuReachesLayersTimeframeTickAndSearch) {
    TopToolbar toolbar;
    QSignalSpy heatmap(&toolbar, &TopToolbar::heatmapToggled);
    QSignalSpy timeframe(&toolbar, &TopToolbar::timeframeSelected);
    QSignalSpy tick(&toolbar, &TopToolbar::tickModeRequested);
    QSignalSpy search(&toolbar, &TopToolbar::quickSearchRequested);
    auto *menu = toolbar.controlsMenu();
    ASSERT_TRUE(menu);
    menuAction(menu, "controlsHeatmap")->trigger();
    ASSERT_EQ(heatmap.size(), 1);
    EXPECT_FALSE(heatmap.at(0).at(0).toBool());
    emit menu->aboutToShow();
    EXPECT_FALSE(menuAction(menu, "controlsHeatmap")->isChecked());
    auto *timeframes = menu->findChild<QMenu *>("controlsTimeframes");
    ASSERT_TRUE(timeframes);
    timeframes->actions().at(2)->trigger();
    ASSERT_EQ(timeframe.size(), 1);
    EXPECT_EQ(timeframe.at(0).at(0).toString(), "5m");
    TopToolbar::TickSelectorState state;
    state.enabled = true;
    state.offeredUnits = {10, 20};
    state.drawnUnits = 10;
    toolbar.setTickSelectorState(state);
    emit menu->aboutToShow();
    menuAction(menu, "controlsTickManual")->trigger();
    ASSERT_EQ(tick.size(), 1);
    EXPECT_TRUE(tick.at(0).at(0).toBool());
    menuAction(menu, "chartQuickSearchAction")->trigger();
    EXPECT_EQ(search.size(), 1);
}

TEST(ChartToolbar, NarrowTickMenuRetainsUnavailableValueAndResolutionWarning) {
    TopToolbar toolbar;
    QSignalSpy requests(&toolbar, &TopToolbar::tickPresetRequested);
    TopToolbar::TickSelectorState state;
    state.enabled = true;
    state.manual = true;
    state.manualUnits = 15;
    state.drawnUnits = 15;
    state.offeredUnits = {10, 20};
    state.indicator = "Locked tick $0.15: columns are veiled";
    auto *tickMenu = toolbar.controlsMenu()->findChild<QMenu *>("controlsTick");
    ASSERT_TRUE(tickMenu);
    for (bool manual : {true, false}) {
        SCOPED_TRACE(manual ? "Manual" : "Auto");
        state.manual = manual;
        toolbar.setTickSelectorState(state);
        emit toolbar.controlsMenu()->aboutToShow();
        auto *unavailable = menuAction(tickMenu, "controlsTickPreset", 1);
        ASSERT_TRUE(unavailable);
        EXPECT_EQ(unavailable->text(), "$0.15 (unavailable)");
        EXPECT_TRUE(unavailable->isChecked());
        EXPECT_FALSE(unavailable->isEnabled());
        EXPECT_EQ(unavailable->toolTip(), toolbar.tickPresetCombo()->itemData(1, Qt::ToolTipRole).toString());
        unavailable->trigger();
        EXPECT_TRUE(requests.isEmpty());
        auto *warning = menuAction(tickMenu, "controlsTickIndicator");
        ASSERT_TRUE(warning);
        EXPECT_EQ(warning->text(), state.indicator);
        EXPECT_FALSE(warning->isEnabled());
        EXPECT_EQ(tickMenu->menuAction()->toolTip(), state.indicator);
    }
    auto *offered = menuAction(tickMenu, "controlsTickPreset", 0);
    ASSERT_TRUE(offered);
    EXPECT_TRUE(offered->isEnabled());
    offered->trigger();
    ASSERT_EQ(requests.size(), 1);
    EXPECT_EQ(requests.at(0).at(0).toLongLong(), 10);

    // Once loaded data can build the current tick, remove the warning and make
    // that same value selectable, without duplicating its menu entry.
    state.offeredUnits = {10, 15, 20};
    state.indicator.clear();
    toolbar.setTickSelectorState(state);
    emit toolbar.controlsMenu()->aboutToShow();
    EXPECT_EQ(tickMenu->findChildren<QAction *>("controlsTickPreset").size(), 3);
    auto *current = menuAction(tickMenu, "controlsTickPreset", 1);
    ASSERT_TRUE(current);
    EXPECT_EQ(current->text(), "$0.15");
    EXPECT_TRUE(current->isChecked());
    EXPECT_TRUE(current->isEnabled());
    EXPECT_TRUE(tickMenu->findChildren<QAction *>("controlsTickIndicator").isEmpty());
    EXPECT_FALSE(tickMenu->menuAction()->toolTip().contains("veiled")); // Qt falls back to the action text.
}

TEST(ChartToolbar, RealOverflowRestoresInlineControlsAcrossWidthModeAndFontChanges) {
    TopToolbar toolbar;
    toolbar.setTickSelectorState({true, {}, false, 10, 0, {10, 20}, 100, {}});
    toolbar.show();
    const QFont original = toolbar.font();
    for (int fontSize : {11, 18, 11}) {
        QFont font = original;
        font.setPointSize(fontSize);
        toolbar.setFont(font);
        for (const Mode mode : {Mode{}, Mode{false, false, true, false, true, true},
                               Mode{false, false, false, true, false, true}}) {
            toolbar.setModeState(mode);
            for (int width : {420, 480, 960, 1920, 480, 1920}) {
                SCOPED_TRACE(::testing::Message() << "font=" << fontSize << " width=" << width << " tpo=" << mode.tpo);
                toolbar.resize(width, 48);
                QTest::qWait(20);
                ASSERT_EQ(toolbar.width(), width);
                EXPECT_TRUE(toolbar.symbolSearch()->isVisible());
                EXPECT_TRUE(toolbar.chartMenuButton()->isVisible());
                auto *native = toolbar.findChild<QToolButton *>("qt_toolbar_ext_button");
                ASSERT_TRUE(native);
                EXPECT_FALSE(native->isVisible()) << "only the explicit overflow may appear";
                for (auto *button : toolbar.findChildren<QToolButton *>()) EXPECT_NE(button->text(), "Controls");
                auto *tickLabel = toolbar.findChild<QLabel *>("chartTickLabel");
                EXPECT_EQ(tickLabel->isVisible(), toolbar.tickModeCombo()->isVisible());
                EXPECT_EQ(tickLabel->isVisible(), toolbar.tickPresetCombo()->isVisible());
                EXPECT_EQ(toolbar.rangeSlider()->isVisible(), toolbar.rangeLabel()->isVisible());
                EXPECT_EQ(toolbar.shownControls(), TopToolbar::controlVisibility(mode)) << "availability includes overflow";
                if (toolbar.controlsButton()->isVisible()) {
                    EXPECT_GE(toolbar.chartMenuButton()->geometry().right(), width - 12);
                    EXPECT_LT(toolbar.controlsButton()->geometry().right(), toolbar.chartMenuButton()->geometry().left());
                    useOverflow(toolbar, [&](QMenu *menu) {
                        EXPECT_NE(menu, toolbar.controlsMenu());
                        EXPECT_FALSE(menu->actions().contains(toolbar.findChild<QAction *>("chartScreenshotAction")));
                        for (auto *action : menu->actions()) EXPECT_NE(action->text(), "Quick Search");
                    });
                } else {
                    EXPECT_GE(width, 960);
                    EXPECT_EQ(toolbar.tickModeCombo()->isVisible(), mode.heatmap);
                    EXPECT_EQ(toolbar.tpoSessionCombo()->isVisible(), mode.tpo || mode.volumeProfile);
                }
                const QRect buttonRect = toolbar.controlsButton()->geometry();
                const bool overflow = toolbar.controlsButton()->isVisible();
                QTest::qWait(20);
                EXPECT_EQ(toolbar.controlsButton()->geometry(), buttonRect);
                EXPECT_EQ(toolbar.controlsButton()->isVisible(), overflow);
            }
        }
    }
    for (int width = 420; width < 1920; width += 7) toolbar.resize(width, 48);
    toolbar.resize(1920, 48);
    QTest::qWait(30);
    EXPECT_FALSE(toolbar.controlsButton()->isVisible());
}

TEST(ChartToolbar, OverflowAppearsOnlyBelowExactSizeHintBoundary) {
    TopToolbar toolbar;
    toolbar.show();
    for (const Mode mode : {Mode{}, Mode{true, true, true, true, true, true},
                           Mode{false, true, false, false, false, true}}) {
        toolbar.setModeState(mode);
        toolbar.resize(3200, 44);
        QTest::qWait(20);
        ASSERT_FALSE(toolbar.controlsButton()->isVisible());
        const int fullWidth = toolbar.sizeHint().width();
        SCOPED_TRACE(::testing::Message() << "full width=" << fullWidth);
        toolbar.resize(fullWidth, 44);
        QTest::qWait(20);
        ASSERT_EQ(toolbar.width(), fullWidth);
        EXPECT_FALSE(toolbar.controlsButton()->isVisible());
        EXPECT_TRUE(toolbar.controlsButton()->menu()->isEmpty());
        toolbar.resize(fullWidth - 1, 44);
        QTest::qWait(20);
        ASSERT_EQ(toolbar.width(), fullWidth - 1);
        EXPECT_TRUE(toolbar.controlsButton()->isVisible());
        EXPECT_FALSE(toolbar.controlsButton()->menu()->isEmpty());
        toolbar.resize(fullWidth, 44);
        QTest::qWait(20);
        EXPECT_FALSE(toolbar.controlsButton()->isVisible());
    }
}

TEST(ChartToolbar, OpenOverflowEditorSurvivesFitChangesAndRebuildsAfterHide) {
    TopToolbar toolbar;
    toolbar.show();
    QTest::qWait(20);
    toolbar.resize(280, 44);
    QTest::qWait(20);
    QPointer<QWidget> editor;
    useOverflow(toolbar, [&](QMenu *menu) {
        auto *entry = qobject_cast<QWidgetAction *>(menuAction(menu, "overflowLiquidityRange"));
        ASSERT_TRUE(entry);
        editor = entry->defaultWidget();
        ASSERT_TRUE(editor);
        const auto before = menuEntries(menu);
        toolbar.setModeState({true, false, false, false, true, false});
        toolbar.setTickSelectorState({false, "Legacy renderer", false, 0, 0, {}, 100, {}});
        QFont larger = toolbar.font();
        larger.setPointSize(larger.pointSize() + 1);
        toolbar.setFont(larger);
        QTest::qWait(20); // deliver the queued fit while the popup is open
        EXPECT_TRUE(menu->isVisible());
        ASSERT_TRUE(editor) << "fit must not delete the open QWidgetAction editor";
        EXPECT_EQ(editor.data(), entry->defaultWidget());
        EXPECT_EQ(menuEntries(menu), before) << "membership changes wait until the popup closes";
    });
    QTest::qWait(20); // aboutToHide rebuild happens after Qt marks it hidden
    EXPECT_FALSE(editor) << "the closed popup must rebuild the old editor";
    const auto after = menuEntries(toolbar.controlsButton()->menu());
    EXPECT_FALSE(after.contains("Liquidity range"));
    EXPECT_TRUE(after.contains("Liquidity threshold"));
    useOverflow(toolbar, [&](QMenu *menu) {
        EXPECT_TRUE(menu->findChild<QSlider *>("overflowThresholdSlider"));
        EXPECT_FALSE(menu->findChild<LiquidityRangeSlider *>("overflowRangeSlider"));
    });
}

TEST(ChartToolbar, OneHiddenControlIsTheOnlyOverflowEntry) {
    TopToolbar toolbar;
    ASSERT_GT(widthForOneHiddenControl(toolbar), 0);
    useOverflow(toolbar, [&](QMenu *menu) {
        EXPECT_EQ(menuEntries(menu), QStringList{"Fullscreen"});
        EXPECT_EQ(menu->actions().size(), 1);
        EXPECT_EQ(menu->actions().first(), toolbar.findChild<QAction *>("chartFullscreenAction"));
    });
    EXPECT_TRUE(toolbar.findChild<QToolButton *>("chartIndicatorsButton")->isVisible());
    EXPECT_TRUE(toolbar.rangeSlider()->isVisible());
}

TEST(ChartToolbar, AllHiddenControlsKeepToolbarOrderAndSeparatorGroups) {
    TopToolbar toolbar;
    toolbar.setModeState({true, true, true, true, true, true});
    toolbar.setTickSelectorState({true, {}, false, 10, 0, {10, 20}, 100, {}});
    toolbar.show();
    QTest::qWait(20);
    toolbar.resize(220, 44);
    QTest::qWait(20);
    const QStringList expected{"Candles", "Heatmap", "Footprint", "TPO", "Volume profile", "|",
        "Timeframe", "Heatmap tick mode", "Heatmap tick size", "Candle style",
        "TPO / volume profile session", "TPO layout", "|", "Liquidity range", "|", "Indicators", "|", "Fullscreen"};
    QSignalSpy heatmap(&toolbar, &TopToolbar::heatmapToggled);
    useOverflow(toolbar, [&](QMenu *menu) {
        EXPECT_EQ(menuEntries(menu), expected);
        EXPECT_FALSE(menuAction(menu, "chartIndicatorsAction")->isEnabled());
        EXPECT_TRUE(menuAction(menu, "chartIndicatorsAction")->toolTip().contains("not available"));
        auto *toggle = menuAction(menu, "controlsHeatmap");
        ASSERT_TRUE(toggle);
        EXPECT_TRUE(toggle->isCheckable());
        EXPECT_TRUE(toggle->isChecked());
        QTest::mouseClick(menu, Qt::LeftButton, {}, menu->actionGeometry(toggle).center());
    });
    ASSERT_EQ(heatmap.size(), 1);
    EXPECT_FALSE(heatmap[0][0].toBool());
    toolbar.setLayerToggleStates(true, true, true, true);
    useOverflow(toolbar, [&](QMenu *menu) { EXPECT_TRUE(menuAction(menu, "controlsHeatmap")->isChecked()); });
    EXPECT_TRUE(toolbar.symbolSearch()->isVisible());
    EXPECT_TRUE(toolbar.subscribeButton()->isVisible());
    EXPECT_TRUE(toolbar.chartMenuButton()->isVisible());
    for (auto *combo : {toolbar.tickModeCombo(), toolbar.tickPresetCombo(), toolbar.chartTypeCombo(),
                       toolbar.tpoSessionCombo(), toolbar.tpoLayoutCombo()}) EXPECT_FALSE(combo->isVisible());
    EXPECT_FALSE(toolbar.rangeSlider()->isVisible());
}

TEST(ChartToolbar, WideningRestoresInlineControlAndRemovesOverflowEntryPerChart) {
    TopToolbar narrow, wide;
    wide.resize(1920, 44);
    wide.show();
    ASSERT_GT(widthForOneHiddenControl(narrow), 0);
    EXPECT_FALSE(wide.controlsButton()->isVisible());
    auto *fullscreen = narrow.findChild<QAction *>("chartFullscreenAction");
    QToolButton *button = nullptr;
    for (auto *candidate : narrow.findChildren<QToolButton *>())
        if (candidate->defaultAction() == fullscreen) button = candidate;
    ASSERT_TRUE(button);
    EXPECT_FALSE(button->isVisible());
    EXPECT_EQ(menuEntries(narrow.controlsButton()->menu()), QStringList{"Fullscreen"});
    narrow.resize(1920, 44);
    QTest::qWait(20);
    EXPECT_TRUE(button->isVisible());
    EXPECT_FALSE(narrow.controlsButton()->isVisible());
    EXPECT_TRUE(narrow.controlsButton()->menu()->isEmpty());
    EXPECT_FALSE(wide.controlsButton()->isVisible());
}

TEST(ChartToolbar, OverflowTickCombosKeepUnavailableValueAndDisabledReason) {
    TopToolbar toolbar;
    toolbar.show();
    QTest::qWait(20);
    toolbar.resize(280, 44);
    toolbar.setTickSelectorState({true, {}, true, 15, 15, {10, 20}, 100, "Locked tick is veiled"});
    QTest::qWait(20);
    QSignalSpy ticks(&toolbar, &TopToolbar::tickPresetRequested);
    useOverflow(toolbar, [&](QMenu *menu) {
        auto *presets = menu->findChild<QMenu *>("overflowTickPreset");
        ASSERT_TRUE(presets);
        EXPECT_TRUE(presets->actions()[1]->isChecked());
        EXPECT_FALSE(presets->actions()[1]->isEnabled());
        EXPECT_TRUE(presets->actions()[1]->toolTip().contains("veiled"));
        clickSubmenuEntry(menu, presets, presets->actions()[2]);
    });
    ASSERT_EQ(ticks.size(), 1);
    EXPECT_EQ(ticks[0][0].toLongLong(), 20);
    EXPECT_EQ(toolbar.tickPresetCombo()->currentData().toLongLong(), 20);
    toolbar.setModeState({true, false, false, false, true, false});
    toolbar.setTickSelectorState({false, "Tick selection requires the GPU heatmap", false, 0, 0, {}, 100, {}});
    QTest::qWait(20);
    useOverflow(toolbar, [&](QMenu *menu) {
        auto *mode = menu->findChild<QMenu *>("overflowTickMode");
        ASSERT_TRUE(mode);
        EXPECT_FALSE(mode->menuAction()->isEnabled());
        EXPECT_EQ(mode->menuAction()->toolTip(), toolbar.tickModeCombo()->toolTip());
        EXPECT_TRUE(mode->menuAction()->toolTip().contains("GPU"));
    });
    toolbar.setModeState({false, false, false, false, true, true});
    QTest::qWait(20);
    useCompleteMenu(toolbar, [&](QMenu *menu) {
        auto *ticksMenu = menu->findChild<QMenu *>("controlsTick");
        EXPECT_FALSE(ticksMenu->menuAction()->isEnabled());
        EXPECT_TRUE(ticksMenu->menuAction()->toolTip().contains("Enable the heatmap"));
    });
}

TEST(ChartToolbar, GearStaysRightmostAfterOverflowAndOwnsWorkspaceCommands) {
    TopToolbar toolbar;
    toolbar.resize(1920, 44);
    toolbar.show();
    QTest::qWait(20);
    ASSERT_GE(toolbar.actions().size(), 4);
    EXPECT_EQ(toolbar.widgetForAction(toolbar.actions()[0]), toolbar.symbolSearch());
    EXPECT_EQ(toolbar.widgetForAction(toolbar.actions()[1]), toolbar.subscribeButton());
    EXPECT_EQ(toolbar.widgetForAction(toolbar.actions().back()), toolbar.chartMenuButton());
    for (int width : {120, 220, 420, 960, 1920}) {
        toolbar.resize(width, 44);
        QTest::qWait(20);
        ASSERT_TRUE(toolbar.chartMenuButton()->isVisible())
            << "requested=" << width << " actual=" << toolbar.width()
            << " minimum=" << toolbar.minimumWidth() << " hint=" << toolbar.sizeHint().width();
        if (width == 120) EXPECT_GT(toolbar.width(), width) << "Pinned controls enforce the chart's minimum width";
        EXPECT_GE(toolbar.chartMenuButton()->geometry().right(), toolbar.width() - 12);
        EXPECT_TRUE(toolbar.symbolSearch()->isVisible());
        EXPECT_TRUE(toolbar.subscribeButton()->isVisible());
        if (auto *extension = toolbar.findChild<QToolButton *>("qt_toolbar_ext_button"))
            EXPECT_FALSE(extension->isVisible());
        if (toolbar.controlsButton()->isVisible()) {
            EXPECT_EQ(toolbar.widgetForAction(toolbar.actions()[toolbar.actions().size() - 2]), toolbar.controlsButton());
            EXPECT_LT(toolbar.controlsButton()->geometry().right(), toolbar.chartMenuButton()->geometry().left());
        }
    }
    emit toolbar.chartMenu()->aboutToShow();
    QSignalSpy layouts(&toolbar, &TopToolbar::layoutsRequested);
    QSignalSpy search(&toolbar, &TopToolbar::quickSearchRequested);
    QSignalSpy shots(&toolbar, &TopToolbar::screenshotRequested);
    for (auto *name : {"chartLayoutsAction", "chartQuickSearchAction", "chartScreenshotAction"}) {
        auto *entry = menuAction(toolbar.chartMenu(), name);
        ASSERT_TRUE(entry);
        EXPECT_TRUE(entry->isVisible());
        EXPECT_FALSE(entry->text().isEmpty());
        EXPECT_FALSE(toolbar.actions().contains(entry));
        for (auto *action : toolbar.actions()) {
            auto *button = qobject_cast<QToolButton *>(toolbar.widgetForAction(action));
            if (button) EXPECT_NE(button->defaultAction(), entry);
        }
        entry->trigger();
    }
    EXPECT_EQ(layouts.size(), 1);
    EXPECT_EQ(search.size(), 1);
    EXPECT_EQ(shots.size(), 1);
}

TEST(ChartToolbar, ChartShortcutOpensCompleteMenuFromWidgetsAndEmbeddedQuickWindow) {
    QMainWindow window;
    auto *outside = new QLineEdit(&window);
    window.setCentralWidget(outside);
    auto *dock = new ChartDock(&window);
    window.addDockWidget(Qt::RightDockWidgetArea, dock);
    window.resize(2100, 500);
    window.show();
    window.raise();
    window.activateWindow();
    ASSERT_TRUE(QTest::qWaitForWindowActive(&window, 2000));
    QTest::qWait(40);
    auto *toolbar = dock->toolbar();
    auto *shortcut = dock->findChild<QShortcut *>("chartControlsShortcut");
    ASSERT_TRUE(shortcut);
    EXPECT_EQ(shortcut->context(), Qt::WidgetWithChildrenShortcut);
    for (int width : {480, 1920}) {
        toolbar->setFixedWidth(width);
        QTest::qWait(20);
        for (auto *target : {static_cast<QWidget *>(toolbar->symbolSearch()), static_cast<QWidget *>(toolbar->chartMenuButton())}) {
            window.raise();
            window.activateWindow();
            ASSERT_TRUE(QTest::qWaitForWindowActive(&window, 2000));
            SCOPED_TRACE(target->objectName().toStdString());
            target->setFocus();
            ASSERT_TRUE(QTest::qWaitFor([&] { return target->hasFocus(); }, 2000));
            QTest::keyClick(target, Qt::Key_F10, Qt::ShiftModifier);
            QTest::qWait(10);
            EXPECT_TRUE(toolbar->controlsMenu()->isVisible());
            EXPECT_NE(toolbar->controlsMenu(), toolbar->controlsButton()->menu());
            EXPECT_EQ(menuEntries(toolbar->controlsMenu()).first(), "Candles");
            EXPECT_TRUE(menuAction(toolbar->controlsMenu(), "chartQuickSearchAction"));
            EXPECT_TRUE(menuAction(toolbar->controlsMenu(), "chartScreenshotAction"));
            EXPECT_TRUE(toolbar->controlsMenu()->activeAction());
            toolbar->controlsMenu()->hide();
            QTest::qWait(20);
        }
        dock->qmlContainer()->setFocus();
        dock->qquickView()->requestActivate();
        ASSERT_TRUE(QTest::qWaitFor([&] { return QGuiApplication::focusWindow() == dock->qquickView(); }, 2000));
        QTest::keyClick(dock->qquickView(), Qt::Key_F10, Qt::ShiftModifier);
        QTest::qWait(10);
        EXPECT_TRUE(toolbar->controlsMenu()->isVisible()) << "embedded QQuickView focus";
        exerciseMenuKeyboard(toolbar->controlsMenu());
        toolbar->controlsMenu()->hide();
        QTest::qWait(20);
    }
    window.raise();
    window.activateWindow();
    window.windowHandle()->requestActivate();
    ASSERT_TRUE(QTest::qWaitForWindowActive(&window, 2000));
    outside->setFocus();
    ASSERT_TRUE(QTest::qWaitFor([&] { return outside->hasFocus(); }, 2000));
    QTest::keyClick(outside, Qt::Key_F10, Qt::ShiftModifier);
    QTest::qWait(10);
    EXPECT_FALSE(toolbar->controlsMenu()->isVisible());
}

TEST(ChartToolbar, OverflowKeyboardFocusArrowsWasdAndEditorTyping) {
    TopToolbar toolbar;
    toolbar.show();
    QTest::qWait(20);
    toolbar.resize(220, 44);
    QTest::qWait(20);
    useOverflow(toolbar, [&](QMenu *menu) { exerciseMenuKeyboard(menu); });
    useOverflow(toolbar, [&](QMenu *menu) {
        ASSERT_TRUE(QTest::qWaitFor([&] { return menu->hasFocus(); }, 1000));
        auto *entry = new QWidgetAction(menu);
        auto *editor = new QLineEdit(menu);
        entry->setDefaultWidget(editor);
        menu->addAction(entry);
        menu->setActiveAction(entry);
        editor->setFocus();
        ASSERT_TRUE(QTest::qWaitFor([&] { return editor->hasFocus(); }, 1000));
        // Deliver through the popup window, which routes to its focused editor.
        // Sending directly to the QLineEdit would bypass that production path.
        for (int key : {Qt::Key_W, Qt::Key_A, Qt::Key_S, Qt::Key_D})
            QTest::keyClick(menu->windowHandle(), static_cast<Qt::Key>(key));
        EXPECT_EQ(editor->text(), "wasd");
        EXPECT_TRUE(menu->isVisible());
        EXPECT_TRUE(editor->hasFocus());
        auto *slider = menu->findChild<LiquidityRangeSlider *>("overflowRangeSlider");
        ASSERT_TRUE(slider);
        slider->setFocus();
        ASSERT_TRUE(QTest::qWaitFor([&] { return slider->hasFocus(); }, 1000));
        auto *active = menu->activeAction();
        QTest::keyClick(slider, Qt::Key_D);
        EXPECT_TRUE(slider->hasFocus());
        EXPECT_EQ(menu->activeAction(), active) << "WASD must not redirect an editor's ignored keys";
        menu->setFocus();
        ASSERT_TRUE(QTest::qWaitFor([&] { return menu->hasFocus(); }, 1000));
        menu->setActiveAction(menu->actions().first());
        active = menu->activeAction();
        // Reproduce mismatched popup/application focus even offscreen: an
        // ignored key originating at an editor must not use the menu's focus.
        QTest::keyClick(slider, Qt::Key_S);
        EXPECT_EQ(menu->activeAction(), active) << "Use the editor receiver, not platform focus";
        QTest::keyClick(menu, Qt::Key_S, Qt::ControlModifier);
        EXPECT_EQ(menu->activeAction(), active);
        ASSERT_TRUE(QApplication::focusWidget());
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Escape);
        EXPECT_FALSE(menu->isVisible());
    });
}

TEST(ChartToolbar, OverflowDownTraversesSliderRowsWithoutEditingAndEscapeCloses) {
    TopToolbar toolbar;
    toolbar.show();
    QTest::qWait(20);
    toolbar.resize(220, 44);
    for (bool gpu : {true, false}) {
        toolbar.setModeState({true, false, false, false, true, gpu});
        QTest::qWait(20);
        useOverflow(toolbar, [&](QMenu *menu) {
            ASSERT_TRUE(QTest::qWaitFor([&] { return menu->hasFocus(); }, 1000));
            auto *range = menu->findChild<LiquidityRangeSlider *>("overflowRangeSlider");
            auto *threshold = menu->findChild<QSlider *>("overflowThresholdSlider");
            ASSERT_EQ(bool(range), gpu);
            ASSERT_EQ(bool(threshold), !gpu);
            const double low = range ? range->low() : 0;
            const double high = range ? range->high() : 0;
            const int strength = threshold ? threshold->value() : 0;
            QList<QAction *> entries;
            for (auto *action : menu->actions())
                if (action->isVisible() && action->isEnabled() && !action->isSeparator()) entries << action;
            ASSERT_GT(entries.size(), 2);
            menu->setActiveAction(entries.first());
            for (int i = 1; i < entries.size(); ++i) {
                QTest::keyClick(menu->windowHandle(), Qt::Key_Down);
                EXPECT_EQ(menu->activeAction(), entries[i]) << "row " << i;
            }
            EXPECT_EQ(menu->activeAction(), entries.last());
            if (range) {
                EXPECT_DOUBLE_EQ(range->low(), low);
                EXPECT_DOUBLE_EQ(range->high(), high);
            }
            if (threshold) EXPECT_EQ(threshold->value(), strength);
            QTest::keyClick(menu->windowHandle(), Qt::Key_Escape);
            EXPECT_FALSE(menu->isVisible());
        });
    }
}

// Opt-in own-widget captures only. This fixture has synthetic tick availability
// and no transport/chart feed; production controls/settings supply the behavior.
TEST(ChartToolbarFixture, NativeSyntheticScreenshots) {
    const QString output = qEnvironmentVariable("SENTINEL_TOOLBAR_SHOTS");
    if (output.isEmpty()) GTEST_SKIP() << "Opt-in synthetic fixture requires output path and root's native GUI slot";
    if (QGuiApplication::platformName() == "offscreen" || QGuiApplication::platformName() == "minimal")
        GTEST_SKIP() << "Native QWidget platform required for authorized fixture screenshots";
    ASSERT_TRUE(QDir::isAbsolutePath(output));
    ASSERT_TRUE(QDir().mkpath(output));
    TempStore store;
    HeatmapSettingsModel model(store.store, "synthetic-toolbar-fixture", store.config);
    UnifiedGridRenderer renderer;
    HeatmapChartControls controls(&model);
    QWidget surface;
    surface.setWindowTitle("Sentinel — synthetic toolbar fixture (no live feed)");
    auto* layout = new QVBoxLayout(&surface);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* label = new QLabel("SYNTHETIC TOOLBAR FIXTURE · no live feed", &surface);
    layout->addWidget(label);
    auto* toolbar = new TopToolbar(&surface);
    layout->addWidget(toolbar);
    controls.setToolbar(toolbar);
    controls.setRenderer(&renderer);
    toolbar->symbolSearch()->setText("ETH-USD");
    toolbar->setBaseAssetSymbol("ETH-USD");
    toolbar->setAvailableTimeframes({60'000, 300'000, 900'000});
    toolbar->setTimeframeMs(300'000);
    ASSERT_TRUE(model.apply({{"sensitivityMin", 0.125}, {"sensitivityMax", 12.5}, {"labelCurrency", "usd"}}, false).isEmpty());
    const auto save = [&](const QString& name, QMenu* menu = nullptr) {
        QTest::qWait(100);
        const auto own = surface.grab();
        if (!menu) return own.save(QDir(output).filePath(name + "-synthetic-fixture.png"));
        const auto popup = menu->grab();
        QPixmap composed(std::max(own.width(), popup.width()), own.height() + popup.height());
        composed.setDevicePixelRatio(own.devicePixelRatio());
        composed.fill(surface.palette().window().color());
        QPainter painter(&composed);
        painter.drawPixmap(0, 0, own);
        painter.drawPixmap(0, int(own.height() / own.devicePixelRatio()), popup);
        painter.end();
        return composed.save(QDir(output).filePath(name + "-synthetic-fixture.png"));
    };
    for (const QString mode : {QString("heatmap"), QString("tpo"), QString("volume")}) {
        if (mode == "heatmap") renderer.setHeatmapLayerEnabled(true);
        else if (mode == "tpo") renderer.setTpoLayerEnabled(true);
        else renderer.setVolumeProfileLayerEnabled(true);
        renderer.setTpoSessionType(4);
        renderer.setTpoLayout("split");
        QCoreApplication::processEvents();
        controls.syncNow();
        // MainWindow supplies this synchronization in production whenever the
        // renderer's mutually exclusive layer state changes.
        toolbar->setLayerToggleStates(renderer.heatmapLayerEnabled(),
                                      renderer.footprintLayerEnabled(),
                                      renderer.tpoLayerEnabled(),
                                      renderer.volumeProfileLayerEnabled());
        auto* controlsMenu = toolbar->controlsMenu();
        emit controlsMenu->aboutToShow();
        auto* heatmapAction = menuAction(controlsMenu, "controlsHeatmap");
        auto* footprintAction = menuAction(controlsMenu, "controlsFootprint");
        auto* tpoAction = menuAction(controlsMenu, "controlsTpo");
        auto* volumeAction = menuAction(controlsMenu, "controlsVolumeProfile");
        ASSERT_TRUE(heatmapAction);
        ASSERT_TRUE(footprintAction);
        ASSERT_TRUE(tpoAction);
        ASSERT_TRUE(volumeAction);
        EXPECT_EQ(heatmapAction->isChecked(), mode == "heatmap");
        EXPECT_FALSE(footprintAction->isChecked());
        EXPECT_EQ(tpoAction->isChecked(), mode == "tpo");
        EXPECT_EQ(volumeAction->isChecked(), mode == "volume");
        for (int width : {420, 480, 960, 1920}) {
            surface.setFixedWidth(width);
            surface.adjustSize();
            surface.show();
            surface.activateWindow();
            QTest::qWait(100);
            // Explicit synthetic availability: current locked $0.15 is retained
            // but unavailable, while loaded data offers $0.10/$0.20/$0.50.
            TopToolbar::TickSelectorState tick;
            tick.enabled = true;
            tick.manual = true;
            tick.manualUnits = tick.drawnUnits = 15;
            tick.offeredUnits = {10, 20, 50};
            tick.indicator = "Synthetic resolution: locked $0.15 unavailable in loaded columns";
            toolbar->setTickSelectorState(tick);
            toolbar->setLiquidityRange(0.01, 100, model.settings().sensitivityMin, model.settings().sensitivityMax);
            toolbar->symbolSearch()->setFocus(Qt::TabFocusReason);
            const QString name = QString("toolbar-%1-%2").arg(mode).arg(width);
            ASSERT_TRUE(save(name));
            if (toolbar->controlsButton()->isVisible()) useOverflow(*toolbar, [&](QMenu *menu) {
                menu->setFocus(Qt::TabFocusReason);
                QTest::keyClick(menu, Qt::Key_Down);
                ASSERT_TRUE(menu->activeAction());
                ASSERT_TRUE(save(name + "-controls-keyboard", menu));
                if (mode == "heatmap") {
                    auto* ticks = menu->findChild<QMenu*>("overflowTickPreset");
                    if (!ticks) return;
                    menu->setActiveAction(ticks->menuAction());
                    QTest::keyClick(menu, Qt::Key_Right);
                    ticks->setFocus(Qt::TabFocusReason);
                    QTest::keyClick(ticks, Qt::Key_Down);
                    ASSERT_TRUE(ticks->activeAction());
                    ASSERT_TRUE(save(name + "-tick-keyboard", ticks));
                    ticks->hide();
                }
                menu->hide();
            });
        }
    }
}

// Load the actual production shell without opening a window or starting a feed.
// This catches QML/accessibility errors and tests the independent P/A controls.
TEST(ChartShell, AxisControlsHaveKeyboardActionsAndIndependentFollowStates) {
    QQmlEngine engine;
    engine.addImportPath("qrc:/qt/qml");
    engine.rootContext()->setContextProperty("uiTheme", QVariant::fromValue<QObject *>(nullptr));
    engine.rootContext()->setContextProperty("dataSource", QVariant::fromValue<QObject *>(nullptr));
    QQmlComponent component(&engine, QUrl("qrc:/qt/qml/Sentinel/Charts/qml/DepthChartView.qml"));
    ASSERT_TRUE(component.isReady()) << component.errorString().toStdString();
    std::unique_ptr<QObject> shell(component.create());
    ASSERT_TRUE(shell) << component.errorString().toStdString();
    shell->setProperty("width", 960);
    shell->setProperty("height", 640);
    auto *renderer = shell->findChild<UnifiedGridRenderer *>("unifiedGridRenderer");
    auto *price = shell->findChild<QQuickItem *>("autoPriceScaleButton");
    auto *live = shell->findChild<QQuickItem *>("followLiveButton");
    ASSERT_TRUE(renderer && price && live);
    renderer->setHeatmapRenderer("gpu");
    renderer->setAutoPriceScale(false);
    renderer->enableAutoScroll(false);
    const auto press = [](QObject *target, int key) {
        QKeyEvent event(QEvent::KeyPress, key, Qt::NoModifier);
        QCoreApplication::sendEvent(target, &event);
    };
    press(price, Qt::Key_Space);
    EXPECT_TRUE(renderer->autoPriceScale());
    EXPECT_FALSE(renderer->autoScrollEnabled());
    press(live, Qt::Key_Return);
    EXPECT_TRUE(renderer->autoScrollEnabled());
    EXPECT_TRUE(renderer->autoPriceScale());
    press(shell.get(), Qt::Key_P);
    EXPECT_FALSE(renderer->autoPriceScale());
    EXPECT_TRUE(renderer->autoScrollEnabled());
    press(shell.get(), Qt::Key_A);
    EXPECT_FALSE(renderer->autoScrollEnabled());
    for (const char *name : {"priceAxisControl", "timeAxisControl"}) {
        auto *axis = shell->findChild<QQuickItem *>(name);
        ASSERT_TRUE(axis);
        EXPECT_TRUE(axis->activeFocusOnTab());
    }
}

// The label request retry (S7b review): repeated no-result failures double the
// delay up to 30 s; a result resets it.
TEST(ChartLabelRetry, BackoffDoublesToTheCapAndResetsAfterAResult) {
    heatmap::gpu::LabelRetryPolicy retry;
    std::vector<int> delays{retry.delayMs()};
    for (int i = 0; i < 7; ++i) {
        retry.failed();
        delays.push_back(retry.delayMs());
    }
    EXPECT_EQ(delays, (std::vector<int>{2000, 4000, 8000, 16000, 30000, 30000, 30000, 30000}));
    retry.succeeded();
    EXPECT_EQ(retry.delayMs(), 2000);
    retry.failed();
    EXPECT_EQ(retry.delayMs(), 4000);
}

// --------------------------------------------------- range slider and menu
class ChartControls : public testing::Test {
protected:
    TempStore t;
    std::unique_ptr<HeatmapSettingsModel> model = std::make_unique<HeatmapSettingsModel>(t.store, "main", t.config);
    std::unique_ptr<HeatmapChartControls> controls = std::make_unique<HeatmapChartControls>(model.get());
    std::unique_ptr<TopToolbar> toolbar = std::make_unique<TopToolbar>();
    void SetUp() override {
        controls->setToolbar(toolbar.get());
        toolbar->resize(2400, 40);
        toolbar->show();
        QCoreApplication::processEvents();
    }
};

// A drag on the range slider applies process-only while it moves and persists on
// release; the model drives the handles otherwise.
TEST_F(ChartControls, RangeSliderDragAppliesLiveAndPersistsOnRelease) {
    auto *slider = toolbar->rangeSlider();
    ASSERT_TRUE(slider->isVisible());
    const double before = model->settings().sensitivityMin;
    const QPoint low(int(slider->xOf(slider->low())), slider->height() / 2);
    QTest::mousePress(slider, Qt::LeftButton, {}, low);
    QMouseEvent move(QEvent::MouseMove, QPointF(low + QPoint(30, 0)), slider->mapToGlobal(QPointF(low + QPoint(30, 0))),
                     Qt::NoButton, Qt::LeftButton, {});
    QApplication::sendEvent(slider, &move);
    EXPECT_GT(model->settings().sensitivityMin, before) << "applied while dragging";
    EXPECT_EQ(t.reload().sensitivityMin, before) << "not persisted while dragging";
    QTest::mouseRelease(slider, Qt::LeftButton, {}, low + QPoint(30, 0));
    const double after = model->settings().sensitivityMin;
    EXPECT_GT(after, before);
    EXPECT_EQ(t.reload().sensitivityMin, after) << "persisted on release";
    EXPECT_LT(model->settings().sensitivityMin, model->settings().sensitivityMax);
    // The model (API, dialog) moves the handles.
    ASSERT_TRUE(model->apply({{"sensitivityMin", 0.2}, {"sensitivityMax", 20.0}}).isEmpty());
    EXPECT_DOUBLE_EQ(slider->low(), 0.2);
    EXPECT_DOUBLE_EQ(slider->high(), 20.0);
    EXPECT_TRUE(toolbar->rangeLabel()->text().contains("0.2 - 20"));
}

// The low handle never passes the high one, and the ends always contain both.
TEST_F(ChartControls, RangeSliderKeepsLowBelowHigh) {
    auto *slider = toolbar->rangeSlider();
    slider->setDomain(0.01, 100);
    const QPoint low(int(slider->xOf(slider->low())), slider->height() / 2);
    QTest::mousePress(slider, Qt::LeftButton, {}, low);
    QTest::mouseRelease(slider, Qt::LeftButton, {}, QPoint(slider->width() - 1, slider->height() / 2));
    EXPECT_LT(model->settings().sensitivityMin, model->settings().sensitivityMax);
    EXPECT_GE(model->settings().sensitivityMax / model->settings().sensitivityMin, 1.0);
    ASSERT_TRUE(model->apply({{"sensitivityMin", 1e-6}, {"sensitivityMax", 1e6}}).isEmpty());
    EXPECT_LE(slider->endLo(), 1e-6);
    EXPECT_GE(slider->endHi(), 1e6);
}

TEST_F(ChartControls, RangeSliderKeyboardEditsBaseAssetValuesAndPersists) {
    auto *slider = toolbar->rangeSlider();
    toolbar->setBaseAssetSymbol("ETH-USD");
    EXPECT_TRUE(slider->toolTip().contains("ETH per cell"));
    EXPECT_TRUE(toolbar->rangeLabel()->text().endsWith(" ETH"));
    EXPECT_EQ(slider->focusPolicy(), Qt::StrongFocus);
    QSignalSpy edits(slider, &LiquidityRangeSlider::rangeEdited);
    const double before = slider->low();
    slider->setFocus();
    QTest::keyClick(slider, Qt::Key_Home);
    QTest::keyClick(slider, Qt::Key_Right);
    ASSERT_EQ(edits.size(), 1);
    EXPECT_GT(slider->low(), before);
    EXPECT_TRUE(edits.at(0).at(2).toBool());
    EXPECT_EQ(t.reload().sensitivityMin, slider->low());
    const double highBefore = slider->high();
    QTest::keyClick(slider, Qt::Key_End);
    QTest::keyClick(slider, Qt::Key_Left);
    EXPECT_LT(slider->high(), highBefore);
    EXPECT_GE(slider->high() / slider->low(), LiquidityRangeSlider::kMinRatio);
}

TEST_F(ChartControls, RangeSliderKeepsItsDragDomainWhileDataChanges) {
    auto *slider = toolbar->rangeSlider();
    slider->setDomain(0.01, 100);
    const double lo = slider->endLo(), hi = slider->endHi();
    const QPoint handle(int(slider->xOf(slider->low())), slider->height() / 2);
    QTest::mousePress(slider, Qt::LeftButton, {}, handle);
    slider->setDomain(1e-6, 1e6);
    EXPECT_DOUBLE_EQ(slider->endLo(), lo);
    EXPECT_DOUBLE_EQ(slider->endHi(), hi);
    QTest::mouseRelease(slider, Qt::LeftButton, {}, handle + QPoint(8, 0));
    EXPECT_LE(slider->endLo(), 1e-6);
    EXPECT_GE(slider->endHi(), 1e6);
}

TEST_F(ChartControls, HiddenComboSubmenusRoundTripInlineAndChartState) {
    UnifiedGridRenderer renderer;
    renderer.setTpoLayerEnabled(true);
    controls->setRenderer(&renderer);
    toolbar->resize(280, 44);
    QTest::qWait(20);
    ASSERT_FALSE(toolbar->chartTypeCombo()->isVisible());
    ASSERT_FALSE(toolbar->tpoSessionCombo()->isVisible());
    QSignalSpy styles(toolbar.get(), &TopToolbar::chartTypeSelected);
    QSignalSpy sessions(toolbar.get(), &TopToolbar::tpoSessionSelected);
    useOverflow(*toolbar, [&](QMenu *menu) {
        ASSERT_NE(menu, toolbar->controlsMenu()) << "choose the hidden control from the extension, not the complete keyboard menu";
        auto *stylesMenu = menu->findChild<QMenu *>("controlsCandleStyles");
        ASSERT_TRUE(stylesMenu);
        EXPECT_TRUE(stylesMenu->actions()[toolbar->chartTypeCombo()->currentIndex()]->isChecked());
        clickSubmenuEntry(menu, stylesMenu, stylesMenu->actions()[1]);
    });
    EXPECT_EQ(styles.size(), 1);
    EXPECT_EQ(toolbar->chartTypeCombo()->currentIndex(), 1);
    EXPECT_EQ(renderer.candleStyle(), 1);
    useOverflow(*toolbar, [&](QMenu *menu) {
        auto *sessionsMenu = menu->findChild<QMenu *>("controlsTpoSession");
        ASSERT_TRUE(sessionsMenu);
        clickSubmenuEntry(menu, sessionsMenu, sessionsMenu->actions()[1]);
    });
    EXPECT_EQ(sessions.size(), 1);
    EXPECT_EQ(toolbar->tpoSessionCombo()->currentData().toInt(), 1);
    EXPECT_EQ(renderer.tpoSessionType(), 1);
    toolbar->chartTypeCombo()->setCurrentIndex(2);
    EXPECT_EQ(renderer.candleStyle(), 2);
    toolbar->tpoSessionCombo()->setCurrentIndex(2);
    emit toolbar->tpoSessionCombo()->activated(2);
    EXPECT_EQ(renderer.tpoSessionType(), 2);
    useOverflow(*toolbar, [&](QMenu *menu) {
        EXPECT_TRUE(menu->findChild<QMenu *>("controlsCandleStyles")->actions()[2]->isChecked());
        EXPECT_TRUE(menu->findChild<QMenu *>("controlsTpoSession")->actions()[2]->isChecked());
    });
    renderer.setCandleStyle(0);
    renderer.setTpoSessionType(4);
    controls->syncNow();
    useOverflow(*toolbar, [&](QMenu *menu) {
        EXPECT_EQ(toolbar->chartTypeCombo()->currentIndex(), 0);
        EXPECT_EQ(toolbar->tpoSessionCombo()->currentData().toInt(), 4);
        EXPECT_TRUE(menu->findChild<QMenu *>("controlsCandleStyles")->actions()[0]->isChecked());
        EXPECT_TRUE(menu->findChild<QMenu *>("controlsTpoSession")->actions()[4]->isChecked());
    });
}

TEST_F(ChartControls, OverflowSlidersDragAndShareInlineAndChartState) {
    toolbar->resize(280, 44);
    QTest::qWait(20);
    QSignalSpy ranges(toolbar.get(), &TopToolbar::liquidityRangeEdited);
    useOverflow(*toolbar, [&](QMenu *menu) {
        QPointer<LiquidityRangeSlider> slider = menu->findChild<LiquidityRangeSlider *>("overflowRangeSlider");
        auto *label = menu->findChild<QLabel *>("overflowRangeLabel");
        ASSERT_TRUE(slider && label);
        EXPECT_TRUE(menu->findChildren<QDoubleSpinBox *>().isEmpty());
        EXPECT_EQ(slider->size(), toolbar->rangeSlider()->size());
        EXPECT_EQ(label->text(), toolbar->rangeLabel()->text());
        EXPECT_DOUBLE_EQ(slider->low(), toolbar->rangeSlider()->low());
        EXPECT_DOUBLE_EQ(slider->high(), toolbar->rangeSlider()->high());
        EXPECT_DOUBLE_EQ(slider->endLo(), toolbar->rangeSlider()->endLo());
        EXPECT_DOUBLE_EQ(slider->endHi(), toolbar->rangeSlider()->endHi());
        const double persisted = t.reload().sensitivityMin;
        const QPoint start(int(slider->xOf(slider->low())), slider->height() / 2);
        const QPoint target = start + QPoint(30, 0);
        QTest::mousePress(slider, Qt::LeftButton, {}, start);
        ASSERT_TRUE(slider->dragging());
        QMouseEvent move(QEvent::MouseMove, QPointF(target), slider->mapToGlobal(QPointF(target)),
                         Qt::NoButton, Qt::LeftButton, {});
        QApplication::sendEvent(slider, &move);
        QTest::qWait(20);
        ASSERT_TRUE(slider) << "fit updates during a live drag must keep the editor";
        EXPECT_TRUE(menu->isVisible()) << "the QWidgetAction must allow mouse dragging";
        EXPECT_GT(model->settings().sensitivityMin, persisted);
        EXPECT_DOUBLE_EQ(t.reload().sensitivityMin, persisted) << "drag is process-only until release";
        EXPECT_DOUBLE_EQ(toolbar->rangeSlider()->low(), slider->low());
        EXPECT_EQ(label->text(), toolbar->rangeLabel()->text());
        QTest::mouseRelease(slider, Qt::LeftButton, {}, target);
        EXPECT_FALSE(slider->dragging());
        EXPECT_TRUE(menu->isVisible());
        ASSERT_FALSE(ranges.isEmpty());
        EXPECT_TRUE(ranges.last()[2].toBool());
        EXPECT_DOUBLE_EQ(t.reload().sensitivityMin, slider->low());
        const int edits = ranges.size();
        ASSERT_TRUE(model->apply({{"sensitivityMin", 0.3}, {"sensitivityMax", 30.0}}).isEmpty());
        EXPECT_DOUBLE_EQ(slider->low(), 0.3);
        EXPECT_DOUBLE_EQ(slider->high(), 30);
        EXPECT_EQ(label->text(), toolbar->rangeLabel()->text());
        EXPECT_EQ(ranges.size(), edits) << "model synchronization must not emit user edits";
        toolbar->rangeSlider()->setValues(0.4, 40);
        emit toolbar->rangeSlider()->rangeEdited(0.4, 40, true);
        EXPECT_DOUBLE_EQ(slider->low(), 0.4);
        EXPECT_DOUBLE_EQ(slider->high(), 40);
        EXPECT_DOUBLE_EQ(model->settings().sensitivityMin, 0.4);
        EXPECT_DOUBLE_EQ(t.reload().sensitivityMax, 40);
        toolbar->setBaseAssetSymbol("ETH-USD");
        EXPECT_EQ(label->text(), toolbar->rangeLabel()->text());
        EXPECT_TRUE(label->text().endsWith(" ETH"));
        EXPECT_TRUE(slider->toolTip().contains("ETH per cell"));
    });
    toolbar->setModeState({true, false, false, false, true, false});
    QTest::qWait(20);
    QSignalSpy thresholds(toolbar.get(), &TopToolbar::liquidityThresholdChanged);
    useOverflow(*toolbar, [&](QMenu *menu) {
        auto *strength = menu->findChild<QSlider *>("overflowThresholdSlider");
        ASSERT_TRUE(strength);
        EXPECT_EQ(strength->orientation(), Qt::Horizontal);
        EXPECT_EQ(strength->width(), toolbar->liquiditySlider()->width());
        strength->setValue(450);
        EXPECT_EQ(toolbar->liquiditySlider()->value(), 450);
        toolbar->liquiditySlider()->setValue(600);
        EXPECT_EQ(strength->value(), 600);
        QStyleOptionSlider option;
        option.initFrom(strength);
        option.orientation = Qt::Horizontal;
        option.minimum = strength->minimum();
        option.maximum = strength->maximum();
        option.sliderPosition = option.sliderValue = strength->value();
        const QRect handle = strength->style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, strength);
        const QPoint target = handle.center() + QPoint(-25, 0);
        const int before = strength->value();
        QTest::mousePress(strength, Qt::LeftButton, {}, handle.center());
        ASSERT_TRUE(strength->isSliderDown());
        QMouseEvent move(QEvent::MouseMove, QPointF(target), strength->mapToGlobal(QPointF(target)),
                         Qt::NoButton, Qt::LeftButton, {});
        QApplication::sendEvent(strength, &move);
        QTest::mouseRelease(strength, Qt::LeftButton, {}, target);
        EXPECT_TRUE(menu->isVisible());
        EXPECT_LT(strength->value(), before);
        EXPECT_EQ(strength->value(), toolbar->liquiditySlider()->value());
        EXPECT_DOUBLE_EQ(thresholds.last()[0].toDouble(), strength->value());
    });
    EXPECT_GE(thresholds.size(), 3);
}

TEST_F(ChartControls, GearMovedCommandsSurviveHostRebuildWithoutVisibleDuplicates) {
    HeatmapChartControls::MenuHooks hooks;
    int saves = 0;
    hooks.saveLayout = [&] { ++saves; };
    hooks.restoreLayout = [] {};
    hooks.screenshot = [] {};
    for (int pass = 0; pass < 2; ++pass) {
        controls->setMenuHooks(hooks);
        emit toolbar->chartMenu()->aboutToShow();
        const auto entries = menuEntries(toolbar->chartMenu());
        EXPECT_EQ(entries.count("Layouts"), 1);
        EXPECT_EQ(entries.count("Quick Search"), 1);
        EXPECT_EQ(entries.count("Screenshot"), 1);
        EXPECT_FALSE(entries.contains("Save chart screenshot"));
        auto *layouts = menuAction(toolbar->chartMenu(), "chartLayoutsAction");
        ASSERT_TRUE(layouts);
        ASSERT_TRUE(layouts->menu());
        EXPECT_TRUE(menuAction(layouts->menu(), "chartMenuSaveLayout")->isEnabled());
        useGear(*toolbar, [&](QMenu *menu) {
            menu->setActiveAction(layouts);
            QTest::keyClick(menu, Qt::Key_Right);
            ASSERT_TRUE(layouts->menu()->isVisible());
            auto *save = menuAction(layouts->menu(), "chartMenuSaveLayout");
            layouts->menu()->setActiveAction(save);
            QTest::keyClick(layouts->menu(), Qt::Key_Return);
        });
        EXPECT_EQ(saves, pass + 1);
        QSignalSpy shots(toolbar.get(), &TopToolbar::screenshotRequested);
        useGear(*toolbar, [&](QMenu *menu) {
            auto *shot = menuAction(menu, "chartScreenshotAction");
            ASSERT_TRUE(shot);
            QTest::mouseClick(menu, Qt::LeftButton, {}, menu->actionGeometry(shot).center());
        });
        EXPECT_EQ(shots.size(), 1);
        for (auto *action : toolbar->actions()) {
            auto *button = qobject_cast<QToolButton *>(toolbar->widgetForAction(action));
            if (button && button->defaultAction()) EXPECT_FALSE(entries.contains(button->defaultAction()->text()));
        }
    }
}

TEST_F(ChartControls, CompleteKeyboardMenuSharesPersistedLabelsAndRebuiltLayoutHooks) {
    UnifiedGridRenderer renderer;
    controls->setRenderer(&renderer);
    toolbar->resize(480, 44);
    QTest::qWait(20);
    int saved = 0, restored = 0;
    for (int pass = 0; pass < 2; ++pass) {
        HeatmapChartControls::MenuHooks hooks;
        hooks.saveLayout = [&] { ++saved; };
        hooks.restoreLayout = [&] { ++restored; };
        controls->setMenuHooks(hooks); // destroys/rebuilds the gear's owned actions
        useCompleteMenu(*toolbar, [&](QMenu *menu) {
            auto *layouts = menu->findChild<QMenu *>("controlsLayouts");
            auto *save = menuAction(menu, "chartMenuSaveLayout");
            EXPECT_EQ(save, menuAction(toolbar->chartMenu(), "chartMenuSaveLayout"));
            clickSubmenuEntry(menu, layouts, save);
        });
        EXPECT_EQ(saved, pass + 1);
        useCompleteMenu(*toolbar, [&](QMenu *menu) {
            auto *layouts = menu->findChild<QMenu *>("controlsLayouts");
            clickSubmenuEntry(menu, layouts, menuAction(menu, "chartMenuRestoreLayout"));
        });
        EXPECT_EQ(restored, pass + 1);
        const bool before = model->settings().showLabels;
        useCompleteMenu(*toolbar, [&](QMenu *menu) {
            auto *labels = menu->findChild<QMenu *>("controlsLabels");
            auto *toggle = menuAction(menu, "chartMenuLabels");
            EXPECT_EQ(toggle, menuAction(toolbar->chartMenu(), "chartMenuLabels"));
            EXPECT_EQ(toggle->isChecked(), before);
            clickSubmenuEntry(menu, labels, toggle);
        });
        EXPECT_EQ(model->settings().showLabels, !before);
        EXPECT_EQ(t.reload().showLabels, !before);
        useCompleteMenu(*toolbar, [&](QMenu *menu) {
            auto *labels = menu->findChild<QMenu *>("controlsLabels");
            menu->setActiveAction(labels->menuAction());
            QTest::keyClick(menu, Qt::Key_Right);
            auto *currency = toolbar->chartMenu()->findChild<QMenu *>("chartMenuCurrency");
            clickSubmenuEntry(labels, currency, menuAction(menu, pass ? "chartMenuCurrencyAsset" : "chartMenuCurrencyUsd"));
        });
        EXPECT_EQ(model->settings().labelCurrency, pass ? "asset" : "usd");
        EXPECT_EQ(t.reload().labelCurrency, model->settings().labelCurrency);
        useCompleteMenu(*toolbar, [&](QMenu *menu) {
            auto *labels = menu->findChild<QMenu *>("controlsLabels");
            menu->setActiveAction(labels->menuAction());
            QTest::keyClick(menu, Qt::Key_Right);
            auto *sizes = toolbar->chartMenu()->findChild<QMenu *>("chartMenuLabelSize");
            clickSubmenuEntry(labels, sizes, sizes->actions().last());
        });
        EXPECT_EQ(t.reload().labelMinPx, model->settings().labelMinPx);
        EXPECT_EQ(t.reload().labelMaxPx, model->settings().labelMaxPx);
    }
    toolbar->resize(1920, 44);
    QTest::qWait(20);
    EXPECT_FALSE(toolbar->controlsButton()->isVisible());
    EXPECT_EQ(toolbar->labelsButton()->isChecked(), model->settings().showLabels);
    EXPECT_EQ(toolbar->liquidityModeCombo()->currentIndex(), model->settings().labelCurrency == "usd" ? 1 : 0);
}

TEST_F(ChartControls, CandleStyleMenuAndComboReachTheExistingRenderer) {
    UnifiedGridRenderer renderer;
    controls->setRenderer(&renderer);
    toolbar->chartTypeCombo()->setCurrentIndex(1);
    EXPECT_EQ(renderer.candleStyle(), 1);
    auto *styles = toolbar->controlsMenu()->findChild<QMenu *>("controlsCandleStyles");
    ASSERT_TRUE(styles);
    styles->actions().at(2)->trigger();
    EXPECT_EQ(renderer.candleStyle(), 2);
    EXPECT_EQ(toolbar->chartTypeCombo()->currentIndex(), 2);
    renderer.setCandleStyle(0);
    QCoreApplication::processEvents();
    EXPECT_EQ(toolbar->chartTypeCombo()->currentIndex(), 0);
    controls->setRenderer(nullptr);
    emit toolbar->controlsMenu()->aboutToShow();
    EXPECT_FALSE(styles->menuAction()->isEnabled());
    EXPECT_FALSE(toolbar->chartTypeCombo()->isEnabled());
}

TEST_F(ChartControls, GearPaletteAndLayoutActionsPersistAndUseHostHooks) {
    int saves = 0, restores = 0, resets = 0;
    QString openedTab;
    HeatmapChartControls::MenuHooks hooks;
    hooks.saveLayout = [&] { ++saves; };
    hooks.restoreLayout = [&] { ++restores; };
    hooks.resetLayout = [&] { ++resets; };
    hooks.openSettings = [&](const QString &tab) { openedTab = tab; };
    controls->setMenuHooks(hooks);
    auto *gear = toolbar->chartMenu();
    EXPECT_EQ(gear->findChildren<QMenu *>("chartMenuLayouts").size(), 1) << "rebuilt menus discard old submenus";
    menuAction(gear, "chartMenuSaveLayout")->trigger();
    menuAction(gear, "chartMenuRestoreLayout")->trigger();
    menuAction(gear, "chartMenuResetLayout")->trigger();
    EXPECT_EQ(saves, 1);
    EXPECT_EQ(restores, 1);
    EXPECT_EQ(resets, 1);
    menuAction(gear, "chartMenuPalettePreset", 1)->trigger();
    EXPECT_EQ(model->settings().palettePreset, "Fire");
    EXPECT_EQ(t.reload().palettePreset, "Fire");
    emit gear->aboutToShow();
    EXPECT_TRUE(menuAction(gear, "chartMenuPalettePreset", 1)->isChecked());
    menuAction(gear, "chartMenuDebugSettings")->trigger();
    EXPECT_EQ(openedTab, "Debug");
    EXPECT_FALSE(menuAction(gear, "chartMenuDebugOverlay")->isEnabled());
}

// The chart settings menu: one entry point; every entry round-trips through the
// settings model (persisted), and the model's changes show in the menu.
TEST_F(ChartControls, ChartMenuRoundTripsTheLabelSettings) {
    int opened = 0, shots = 0;
    QString tab;
    HeatmapChartControls::MenuHooks hooks;
    hooks.openSettings = [&](const QString &name) { ++opened; tab = name; };
    hooks.screenshot = [&] { ++shots; };
    controls->setMenuHooks(hooks);
    QMenu *menu = toolbar->chartMenu();
    ASSERT_TRUE(menu);
    EXPECT_EQ(toolbar->chartMenuButton()->menu(), menu);
    menuAction(menu, "chartMenuSettings")->trigger();
    EXPECT_EQ(opened, 1);
    EXPECT_EQ(tab, "Chart");
    menuAction(menu, "chartMenuScreenshot")->trigger();
    EXPECT_EQ(shots, 1);
    EXPECT_FALSE(menuAction(menu, "chartMenuSaveLayout")->isEnabled()) << "no hook, disabled";

    // The menu shows the model (labels on, USD, the small size by default).
    EXPECT_TRUE(menuAction(menu, "chartMenuLabels")->isChecked());
    EXPECT_TRUE(menuAction(menu, "chartMenuCurrencyUsd")->isChecked());
    EXPECT_TRUE(menuAction(menu, "chartMenuLabelSizePreset", 0)->isChecked());
    // Labels off, asset currency, the medium size: model and store.
    menuAction(menu, "chartMenuLabels")->trigger();
    emit menu->aboutToShow();
    EXPECT_FALSE(menuAction(menu, "chartMenuLabels")->isChecked());
    menuAction(menu, "chartMenuCurrencyAsset")->trigger();
    menuAction(menu, "chartMenuLabelSizePreset", 1)->trigger();
    const auto &s = model->settings();
    EXPECT_FALSE(s.showLabels);
    EXPECT_EQ(s.labelCurrency, "asset");
    EXPECT_EQ(s.labelMinPx, 14);
    EXPECT_EQ(s.labelMaxPx, 18);
    const auto saved = t.reload();
    EXPECT_FALSE(saved.showLabels);
    EXPECT_EQ(saved.labelCurrency, "asset");
    EXPECT_EQ(saved.labelMaxPx, 18);
    // The toolbar shows them.
    EXPECT_FALSE(toolbar->labelsButton()->isChecked());
    EXPECT_EQ(toolbar->liquidityModeCombo()->currentText(), "Asset");

    // The other way: an API-style patch shows in the menu and the toolbar.
    ASSERT_TRUE(model->apply({{"showLabels", true}, {"labelCurrency", "usd"}, {"labelMinPx", 12.0}, {"labelMaxPx", 15.0}})
                    .isEmpty());
    emit menu->aboutToShow();
    EXPECT_TRUE(menuAction(menu, "chartMenuLabels")->isChecked());
    EXPECT_TRUE(menuAction(menu, "chartMenuCurrencyUsd")->isChecked());
    EXPECT_FALSE(menuAction(menu, "chartMenuCurrencyAsset")->isChecked());
    EXPECT_TRUE(menuAction(menu, "chartMenuLabelSizePreset", 0)->isChecked());
    EXPECT_TRUE(toolbar->labelsButton()->isChecked());
    EXPECT_EQ(toolbar->liquidityModeCombo()->currentText(), "USD");
    // Appearance edits now live in the gear, reachable at narrow widths.
    menuAction(menu, "chartMenuLabels")->trigger();
    EXPECT_FALSE(model->settings().showLabels);
    menuAction(menu, "chartMenuCurrencyAsset")->trigger();
    EXPECT_EQ(model->settings().labelCurrency, "asset");
}

TEST_F(ChartControls, LegacyCurrencyRemainsAvailableInAppearanceMenu) {
    ASSERT_TRUE(model->apply({{"renderer", "legacy"}}, false).isEmpty());
    controls->syncNow();
    EXPECT_FALSE(toolbar->shownControls().liquidity) << "currency is secondary appearance, not primary-strip chrome";
    EXPECT_TRUE(toolbar->shownControls().thresholdSlider);
    emit toolbar->chartMenu()->aboutToShow();
    auto* currency = toolbar->chartMenu()->findChild<QMenu*>("chartMenuCurrency");
    ASSERT_TRUE(currency);
    EXPECT_TRUE(currency->menuAction()->isVisible());
    EXPECT_TRUE(currency->menuAction()->isEnabled());
    auto* asset = menuAction(currency, "chartMenuCurrencyAsset");
    auto* usd = menuAction(currency, "chartMenuCurrencyUsd");
    ASSERT_TRUE(asset && usd);
    EXPECT_TRUE(asset->isVisible());
    EXPECT_TRUE(asset->isEnabled());
    asset->trigger();
    EXPECT_EQ(model->settings().labelCurrency, "asset");
    emit toolbar->chartMenu()->aboutToShow();
    EXPECT_TRUE(asset->isChecked());
    EXPECT_FALSE(usd->isChecked());
    usd->trigger();
    EXPECT_EQ(model->settings().labelCurrency, "usd");
    emit toolbar->chartMenu()->aboutToShow();
    EXPECT_TRUE(usd->isChecked());
    EXPECT_FALSE(asset->isChecked());
}

// The dialog's Chart tab edits the same keys, and follows the model.
TEST_F(ChartControls, DialogChartTabRoundTrip) {
    HeatmapSettingsDialog dialog(model.get(), nullptr);
    controls->setDialog(&dialog);
    EXPECT_EQ(dialog.tabs()->tabText(0), "Chart");
    auto *show = dialog.findChild<QCheckBox *>("showLabels");
    auto *currency = dialog.findChild<QComboBox *>("labelCurrency");
    auto *minPx = dialog.findChild<QDoubleSpinBox *>("labelMinPx");
    auto *maxPx = dialog.findChild<QDoubleSpinBox *>("labelMaxPx");
    ASSERT_TRUE(show && currency && minPx && maxPx);
    EXPECT_TRUE(show->isChecked());
    EXPECT_EQ(currency->currentData().toString(), "usd");
    show->setChecked(false);
    currency->setCurrentIndex(currency->findData("asset"));
    maxPx->setValue(20);
    minPx->setValue(16);
    EXPECT_FALSE(model->settings().showLabels);
    EXPECT_EQ(model->settings().labelCurrency, "asset");
    EXPECT_EQ(model->settings().labelMinPx, 16);
    EXPECT_EQ(model->settings().labelMaxPx, 20);
    EXPECT_EQ(t.reload().labelMinPx, 16);
    ASSERT_TRUE(model->apply({{"showLabels", true}, {"labelMaxPx", 24.0}}).isEmpty());
    EXPECT_TRUE(show->isChecked());
    EXPECT_EQ(maxPx->value(), 24);
    // A minimum above the maximum moves the maximum with it (never invalid).
    minPx->setValue(22);
    maxPx->setValue(18);
    EXPECT_LE(model->settings().labelMinPx, model->settings().labelMaxPx);
    // Reset Chart restores the defaults.
    dialog.findChild<QPushButton *>("resetChart")->click();
    EXPECT_EQ(model->settings().labelMinPx, 12);
    EXPECT_EQ(model->settings().labelMaxPx, 15);
    EXPECT_EQ(model->settings().labelCurrency, "usd");
}

// Patch validation of the new keys (Agent API heatmap/settings).
TEST_F(ChartControls, LabelSettingsAreValidated) {
    EXPECT_FALSE(model->apply({{"labelCurrency", "eur"}}).isEmpty());
    EXPECT_FALSE(model->apply({{"showLabels", 1}}).isEmpty());
    ASSERT_TRUE(model->apply({{"labelMinPx", 2.0}, {"labelMaxPx", 99.0}}).isEmpty());
    EXPECT_EQ(model->settings().labelMinPx, 8);  // clamped
    EXPECT_EQ(model->settings().labelMaxPx, 32);
    ASSERT_TRUE(model->apply({{"labelMinPx", 20.0}, {"labelMaxPx", 10.0}}).isEmpty());
    EXPECT_LE(model->settings().labelMinPx, model->settings().labelMaxPx);
}

// ------------------------------------------------------------- GPU chart
QTemporaryDir *fixtureDir = nullptr;

class ChartLabels : public testing::Test {
protected:
    std::unique_ptr<lab::OffscreenQuick> scene;
    UnifiedGridRenderer *ugr = nullptr;
    QString error;
    QImage image;
    TempStore t;
    std::unique_ptr<HeatmapSettingsModel> model;
    std::unique_ptr<HeatmapChartControls> controls;
    std::unique_ptr<TopToolbar> toolbar;
    const int64_t viewLo = epoch + 2 * kHourMs;
    void SetUp() override {
        if (const QString why = lab::gpuUnavailableReason(); !why.isEmpty())
            GTEST_SKIP() << "GPU case skipped: " << why.toStdString();
        scene = std::make_unique<lab::OffscreenQuick>();
        ASSERT_TRUE(scene->create(QSize(640, 320), &error)) << error.toStdString();
        scene->window()->setColor(Qt::black);
        ugr = new UnifiedGridRenderer(scene->window()->contentItem());
        ugr->setSize(QSizeF(640, 320));
        ugr->setHeatmapService(&lab::LabData::instance().service());
        ugr->setActiveSymbol("BTC-USD");
        ugr->setTimeframe(int(minute));
        model = std::make_unique<HeatmapSettingsModel>(t.store, "main", t.config);
        // Bright fixture colours (sizes 0.01..0.53), Manual $1, no crossfade unless a case asks.
        ASSERT_TRUE(model->apply({{"renderer", "gpu"}, {"palettePreset", "Fire"}, {"sensitivityMin", 0.001},
                                  {"sensitivityMax", 1.0}, {"tickMode", "manual"}, {"manualTick", 100},
                                  {"crossfadeMs", 0}}, false)
                        .isEmpty());
        controls = std::make_unique<HeatmapChartControls>(model.get());
        toolbar = std::make_unique<TopToolbar>();
        controls->setToolbar(toolbar.get());
        controls->setRenderer(ugr);
    }
    void TearDown() override {
        controls.reset();
        delete ugr;
        ugr = nullptr;
        if (scene) scene->renderFrame(&error);
        scene.reset();
    }
    heatmap::gpu::HeatmapGpuLayer &layer() const { return *ugr->gpuHeatmapLayer(); }
    // 10 minutes x $16 over 640 x 320: $1 cells of 64 x 20 px.
    void zoomIn(double priceLo = 100'000) { ugr->setViewport(viewLo, viewLo + 10 * minute, priceLo, priceLo + 16); }
    // onFrame runs after every frame (invariants), done ends the pump.
    template <class Done, class OnFrame> bool pump(int ms, Done done, OnFrame onFrame) {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < ms) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            ugr->update();
            image = scene->renderFrame(&error);
            if (image.isNull()) return false;
            onFrame();
            if (done()) return true;
        }
        return false;
    }
    template <class Done> bool pump(int ms, Done done) { return pump(ms, done, [] {}); }
    bool labelsDrawn() const { return !ugr->gpuLabelGlyphs().empty() && ugr->gpuLabelLayout().stats().labels > 0; }
    // Labels must belong to the drawn picture in every frame (plan section 3).
    void checkFrame() {
        const auto labels = layer().labelsForFrame();
        const auto &st = layer().tileStats();
        if (labels) {
            ASSERT_EQ(labels->key.tickUnits, st.drawnTickUnits.load());
            ASSERT_EQ(labels->key.tfMs, st.drawnTfMs.load());
            ASSERT_FALSE(st.crossfading.load());
        }
        if (!labels) ASSERT_TRUE(ugr->gpuLabelGlyphs().empty()) << "glyphs without a matched result";
        for (const auto &g : ugr->gpuLabelGlyphs()) {
            ASSERT_GE(g.rect.left(), 0.0);
            ASSERT_GE(g.rect.top(), 0.0);
            ASSERT_LE(g.rect.right(), 640.0);
            ASSERT_LE(g.rect.bottom(), 320.0);
        }
    }
    bool waitForLabels(int ms = 20'000) {
        return pump(ms, [&] { return labelsDrawn(); }, [&] { checkFrame(); });
    }
    heatmap::HeatmapCellQuery *query() const { return layer().controller()->cellQuery(); }
};

TEST_F(ChartLabels, LabelsDrawOnColouredCellsAtTheDrawnTick) {
    zoomIn();
    ASSERT_TRUE(waitForLabels()) << error.toStdString();
    const auto labels = layer().labelsForFrame();
    ASSERT_TRUE(labels);
    EXPECT_EQ(labels->key.tickUnits, 100);
    EXPECT_EQ(labels->key.tfMs, minute);
    const auto &stats = ugr->gpuLabelLayout().stats();
    EXPECT_GE(stats.sizePx, 12.0);
    EXPECT_LE(stats.sizePx, 15.0);
    // About the visible $1 x 1m cells (10 x 16), all coloured in the fixture's near band.
    EXPECT_GE(stats.labels, 100u);
    EXPECT_LE(stats.labels, 11u * 17u);
    // The request covered the view plus a margin, within the query's limit.
    const auto request = layer().postedLabelRequest();
    EXPECT_LE(uint64_t(request.columns) * request.rows, heatmap::gpu::HeatmapGpuLayer::kMaxLabelCells);
    EXPECT_LE(request.firstBucket, viewLo / minute);
    EXPECT_GE(request.firstBucket + int64_t(request.columns), (viewLo + 10 * minute) / minute);
    // The slider's ends follow the window's liquidity.
    pump(400, [] { return false; });
    const auto range = layer().liquidityRange();
    ASSERT_TRUE(range.valid);
    EXPECT_GT(range.lo, 0.0);
    EXPECT_LE(range.hi, 0.6);
    EXPECT_DOUBLE_EQ(toolbar->rangeSlider()->endHi(), std::max(range.hi, 1.0 * 1.25));
    // The low handle above every cell hides every label with the colour.
    ASSERT_TRUE(model->apply({{"sensitivityMin", 0.9}, {"sensitivityMax", 2.0}}, false).isEmpty());
    ASSERT_TRUE(pump(2000, [&] { return ugr->gpuLabelLayout().stats().labels == 0; }));
    // Labels off: none drawn (the request still runs for the slider's ends).
    ASSERT_TRUE(model->apply({{"sensitivityMin", 0.001}, {"sensitivityMax", 1.0}, {"showLabels", false}}, false).isEmpty());
    pump(300, [] { return false; });
    EXPECT_TRUE(ugr->gpuLabelGlyphs().empty());
    EXPECT_FALSE(layer().labelsForFrame());
}

// The gate: with more than kMaxLabelCells visible cells nothing is posted.
TEST_F(ChartLabels, ZoomedOutPostsNoLabelRequest) {
    ugr->setViewport(viewLo, viewLo + 120 * minute, 99'900, 100'300); // 120 x 400 $1 cells
    const auto before = layer().labelCounters().posted;
    pump(1500, [] { return false; }, [&] { checkFrame(); });
    EXPECT_EQ(layer().labelCounters().posted, before);
    EXPECT_EQ(layer().postedLabelRequest().serial, 0u);
    EXPECT_TRUE(ugr->gpuLabelGlyphs().empty());
}

// HeatmapCellQuery::cancel() drops its result without a signal: the chart must
// not keep drawing a held copy, and it asks again (retry with backoff).
TEST_F(ChartLabels, ACancelledQueryDrawsNoStaleLabelsAndRetries) {
    zoomIn();
    ASSERT_TRUE(waitForLabels()) << error.toStdString();
    auto *q = query();
    auto cancel = [&] {
        if (q->thread() == QThread::currentThread()) q->cancel();
        else QMetaObject::invokeMethod(q, [q] { q->cancel(); }, Qt::BlockingQueuedConnection);
        ASSERT_FALSE(q->latestLabels());
    };
    // A quiet chart: settled, labels drawn and no request for 20 frames.
    auto quiet = [&] {
        int frames = 0;
        uint64_t posted = layer().labelCounters().posted;
        return pump(20'000, [&] {
            frames = layer().settled() && layer().labelCounters().posted == posted && labelsDrawn() ? frames + 1 : 0;
            posted = layer().labelCounters().posted;
            return frames >= 20;
        });
    };
    for (int round = 0; round < 3; ++round) {
        SCOPED_TRACE(round);
        ASSERT_TRUE(quiet());
        const uint64_t drawnSerial = ugr->gpuLabelSerial(), posted = layer().labelCounters().posted;
        const uint64_t retries = layer().labelCounters().retries;
        ASSERT_GT(drawnSerial, 0u);
        cancel();
        // The very next frame never draws the cancelled result.
        ASSERT_TRUE(pump(1000, [] { return true; }));
        EXPECT_NE(ugr->gpuLabelSerial(), drawnSerial) << "no label drawn from a cancelled result";
        // They come back: by a new request (releasing the query's wants can republish
        // the SpanSet) or, with nothing else changing, by the retry.
        ASSERT_TRUE(pump(heatmap::gpu::HeatmapGpuLayer::kLabelRetryMs + 8000,
                         [&] { return labelsDrawn() && ugr->gpuLabelSerial() > drawnSerial; }, [&] { checkFrame(); }))
            << error.toStdString();
        const bool retried = layer().labelCounters().retries > retries;
        if (layer().labelCounters().posted == posted + 1 && !retried) continue; // a version change re-posted
        if (retried) {
            EXPECT_EQ(layer().labelCounters().retries, retries + 1);
            EXPECT_EQ(layer().labelRetryMs(), heatmap::gpu::HeatmapGpuLayer::kLabelRetryMs) << "reset after a result";
            return; // the retry path ran
        }
    }
    std::cout << "[note] every cancel was followed by a republished SpanSet; the retry ran: "
              << layer().labelCounters().retries << std::endl;
}

// Tick changes: a result of another tick never draws; during the crossfade none
// draws; the new tick's labels arrive.
TEST_F(ChartLabels, LabelsNeverDrawForAnotherTickOrDuringACrossfade) {
    ASSERT_TRUE(model->apply({{"crossfadeMs", 600}}, false).isEmpty());
    zoomIn();
    ASSERT_TRUE(waitForLabels()) << error.toStdString();
    ASSERT_TRUE(model->apply({{"tickMode", "manual"}, {"manualTick", 200}}, false).isEmpty());
    bool sawCrossfade = false;
    ASSERT_TRUE(pump(20'000, [&] {
        const auto labels = layer().labelsForFrame();
        return labels && labels->key.tickUnits == 200 && labelsDrawn();
    }, [&] {
        checkFrame();
        if (layer().tileStats().crossfading.load()) {
            sawCrossfade = true;
            EXPECT_TRUE(ugr->gpuLabelGlyphs().empty()) << "no labels during the crossfade";
        }
    })) << error.toStdString();
    EXPECT_TRUE(sawCrossfade);
    // The old tick's result is gone from use: the drawn tick is $2 now.
    EXPECT_EQ(layer().tileStats().drawnTickUnits.load(), 200);
}

// A symbol switch while a request is in flight: the old symbol's labels (and a
// result that lands after the switch) never draw on the new symbol's picture.
TEST_F(ChartLabels, ASymbolSwitchMidRequestDropsTheOldSymbolsLabels) {
    zoomIn();
    ASSERT_TRUE(waitForLabels()) << error.toStdString();
    const uint64_t oldSerial = layer().labelsForFrame()->key.serial;
    // A new window (a request in flight), then the switch in the same turn.
    zoomIn(100'004);
    pump(1, [] { return true; });
    ugr->setActiveSymbol("ETH-USD");
    EXPECT_FALSE(layer().labelsForFrame()) << "dropped at once, before any frame";
    pump(1500, [] { return false; }, [&] {
        checkFrame();
        EXPECT_TRUE(ugr->gpuLabelGlyphs().empty());
    });
    // Whatever the query holds now, only a request made after the switch may draw.
    if (const auto labels = layer().labelsForFrame()) EXPECT_GE(labels->key.serial, layer().labelCounters().epoch);
    // Back to BTC: only a request made after the switch draws.
    ugr->setActiveSymbol("BTC-USD");
    zoomIn();
    ASSERT_TRUE(waitForLabels()) << error.toStdString();
    EXPECT_GT(layer().labelsForFrame()->key.serial, oldSerial);
    EXPECT_GE(layer().labelsForFrame()->key.serial, layer().labelCounters().epoch);
}

// The toolbar follows the chart's layers (TPO hides the heatmap-only controls),
// and the appearance menu drives the model and the legacy label currency.
TEST_F(ChartLabels, ToolbarFollowsTheChartsLayers) {
    pump(300, [] { return false; });
    EXPECT_TRUE(toolbar->shownControls().rangeSlider);
    EXPECT_TRUE(toolbar->shownControls().tickSelector);
    EXPECT_FALSE(toolbar->shownControls().tpoSession);
    ugr->setTpoLayerEnabled(true); // exclusive today: the heatmap goes off
    pump(300, [] { return false; });
    auto shown = toolbar->shownControls();
    EXPECT_FALSE(shown.tickSelector);
    EXPECT_FALSE(shown.rangeSlider);
    EXPECT_FALSE(shown.palette);
    EXPECT_FALSE(shown.liquidity);
    EXPECT_TRUE(shown.tpoSession);
    EXPECT_TRUE(shown.tpoLayout);
    // TPO controls drive the renderer and follow it.
    const int split = toolbar->tpoLayoutCombo()->findData("split");
    toolbar->tpoLayoutCombo()->setCurrentIndex(split);
    emit toolbar->tpoLayoutCombo()->activated(split);
    EXPECT_EQ(ugr->tpoLayout().toStdString(), "split");
    ugr->setTpoLayout("collapsed"); // the dialog or the API: tpoStyleChanged, normal event processing
    pump(100, [] { return false; });
    EXPECT_EQ(toolbar->tpoLayoutCombo()->currentData().toString(), "collapsed");
    ugr->setHeatmapLayerEnabled(true);
    pump(300, [] { return false; });
    shown = toolbar->shownControls();
    EXPECT_TRUE(shown.tickSelector);
    EXPECT_TRUE(shown.rangeSlider);
    EXPECT_FALSE(shown.tpoSession);
    // Legacy renderer: the threshold slider instead of the range slider.
    ASSERT_TRUE(model->apply({{"renderer", "legacy"}}, false).isEmpty());
    pump(300, [] { return false; });
    shown = toolbar->shownControls();
    EXPECT_FALSE(shown.rangeSlider);
    EXPECT_TRUE(shown.thresholdSlider);
    EXPECT_FALSE(shown.labelsToggle) << "legacy always draws its labels: no toggle";
    emit toolbar->chartMenu()->aboutToShow();
    EXPECT_FALSE(menuAction(toolbar->chartMenu(), "chartMenuLabels")->isVisible()) << "hidden in legacy, as the toolbar";
    EXPECT_FALSE(shown.liquidity) << "appearance stays in the gear menu";
    auto *currency = toolbar->chartMenu()->findChild<QMenu *>("chartMenuCurrency");
    ASSERT_TRUE(currency);
    EXPECT_TRUE(currency->menuAction()->isVisible());
    EXPECT_TRUE(currency->menuAction()->isEnabled());
    auto *asset = menuAction(currency, "chartMenuCurrencyAsset");
    ASSERT_TRUE(asset && asset->isVisible() && asset->isEnabled());
    asset->trigger();
    EXPECT_EQ(model->settings().labelCurrency, "asset");
    EXPECT_EQ(ugr->liquidityLabelMode(), 0);
    emit toolbar->chartMenu()->aboutToShow();
    EXPECT_TRUE(asset->isChecked());
    ASSERT_TRUE(model->apply({{"labelCurrency", "usd"}}, false).isEmpty());
    EXPECT_EQ(ugr->liquidityLabelMode(), 1);
    emit toolbar->chartMenu()->aboutToShow();
    EXPECT_TRUE(menuAction(currency, "chartMenuCurrencyUsd")->isChecked());
    EXPECT_FALSE(asset->isChecked());
}

// Renderer state changed elsewhere (Agent API, another surface) shows in the
// toolbar and the dialog through normal event processing (no explicit sync),
// without echoing back to the renderer.
TEST_F(ChartLabels, RendererBackedControlsFollowTheRendererThroughEvents) {
    HeatmapSettingsDialog dialog(model.get(), ugr);
    controls->setDialog(&dialog);
    pump(100, [] { return false; });
    QSignalSpy styleSignals(ugr, &UnifiedGridRenderer::candleStyleChanged);
    ugr->setCandleStyle(2);
    ugr->setTpoLayout("split");
    pump(200, [] { return false; });
    EXPECT_EQ(dialog.findChild<QComboBox *>("candleStyle")->currentIndex(), 2);
    EXPECT_EQ(toolbar->chartTypeCombo()->currentIndex(), 2);
    EXPECT_EQ(toolbar->tpoLayoutCombo()->currentData().toString(), "split");
    EXPECT_EQ(dialog.findChild<QComboBox *>("tpoLayout")->currentData().toString(), "split");
    EXPECT_EQ(styleSignals.count(), 1) << "no echo from the controls back to the renderer";
    ugr->setCandleStyle(0);
    pump(200, [] { return false; });
    EXPECT_EQ(dialog.findChild<QComboBox *>("candleStyle")->currentIndex(), 0);
    EXPECT_EQ(toolbar->chartTypeCombo()->currentIndex(), 0);
}

// The crossfade's last frame lays labels out with the previous frame's
// "crossfading" flag (the node prepares after the layout), so it draws none; the
// node then stops asking for frames. The chart must request one more frame on its
// own (frames render ONLY when the scene asks: no update() from the test).
TEST_F(ChartLabels, LabelsReturnAfterACrossfadeOnAnIdleChart) {
    ASSERT_TRUE(model->apply({{"crossfadeMs", 300}}, false).isEmpty());
    zoomIn();
    ASSERT_TRUE(waitForLabels()) << error.toStdString();
    auto pumpOnRequest = [&](int ms, auto done) {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < ms) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            if (scene->frameRequested()) {
                image = scene->renderFrame(&error);
                if (image.isNull()) return false;
                checkFrame();
            }
            if (done()) return true;
        }
        return false;
    };
    // Idle first: no frames are asked for.
    pumpOnRequest(500, [] { return false; });
    ASSERT_TRUE(model->apply({{"tickMode", "manual"}, {"manualTick", 200}}, false).isEmpty());
    bool faded = false;
    ASSERT_TRUE(pumpOnRequest(5000, [&] {
        faded = faded || layer().tileStats().crossfading.load();
        return faded && !layer().tileStats().crossfading.load();
    })) << "the crossfade ended";
    // After the fade's last frame, labels of the new tick appear with no outside
    // redraw, at once (an unrelated periodic frame about 2 s later must not be what
    // brings them back).
    ASSERT_TRUE(pumpOnRequest(1000, [&] {
        const auto labels = layer().labelsForFrame();
        return labels && labels->key.tickUnits == 200 && labelsDrawn() && ugr->gpuLabelSerial() == labels->key.serial;
    })) << "labels stayed hidden after the crossfade on an idle chart";
}

// Lifecycle: the settings dialog, the chart menu and the toolbar stay usable when
// the chart (renderer) is destroyed under them.
TEST_F(ChartLabels, DialogAndMenuSurviveTheRendererBeingDestroyed) {
    HeatmapSettingsDialog dialog(model.get(), ugr);
    controls->setDialog(&dialog);
    dialog.show();
    zoomIn();
    ASSERT_TRUE(waitForLabels()) << error.toStdString();
    delete ugr;
    ugr = nullptr;
    QCoreApplication::processEvents();
    // Every surface still edits the model; renderer-backed entries do nothing.
    dialog.findChild<QCheckBox *>("showLabels")->setChecked(false);
    EXPECT_FALSE(model->settings().showLabels);
    dialog.findChild<QComboBox *>("candleStyle")->setCurrentIndex(2);
    controls->syncNow();
    emit toolbar->chartMenu()->aboutToShow();
    menuAction(toolbar->chartMenu(), "chartMenuCandleStylePreset", 1)->trigger();
    EXPECT_FALSE(menuAction(toolbar->chartMenu(), "chartMenuCandleStylePreset", 1)->isEnabled()) << "no chart";
    emit toolbar->chartTypeCombo()->activated(2); // the toolbar's candle style with no chart
    menuAction(toolbar->chartMenu(), "chartMenuCurrencyAsset")->trigger();
    EXPECT_EQ(model->settings().labelCurrency, "asset");
    emit toolbar->tpoLayoutCombo()->activated(1);
    emit toolbar->rangeSlider()->rangeEdited(0.01, 2.0, true);
    EXPECT_DOUBLE_EQ(model->settings().sensitivityMax, 2.0);
    dialog.refreshFromRenderer();
    dialog.close();
}
} // namespace

int main(int argc, char **argv) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QTemporaryDir settingsDir;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsDir.path());
    qmlRegisterModule("Sentinel", 1, 0);
    qmlRegisterType<UnifiedGridRenderer>("Sentinel", 1, 0, "UnifiedGridRenderer");
    qmlRegisterType<TimeAxisModel>("Sentinel", 1, 0, "TimeAxisModel");
    qmlRegisterType<PriceAxisModel>("Sentinel", 1, 0, "PriceAxisModel");
    qml_register_types_Sentinel_Charts();
    // Focus/toolbar tests need a real QQuickWindow but no GPU. Honor the
    // explicit software request in sandboxed focused runs before Qt fixes it.
    if (qEnvironmentVariable("QT_QUICK_BACKEND") == "software")
        QQuickWindow::setSceneGraphBackend(QStringLiteral("software"));
    else
        lab::selectQuickSceneGraph();
    Q_INIT_RESOURCE(sentinel_ui_fonts);
    Q_INIT_RESOURCE(sentinel_svg_resources);
    ThemeManager::instance().initializeDefaults();
    ThemeManager::instance().applyTheme("dark", &app);
    const QString fixtureFont = qEnvironmentVariable("SENTINEL_TOOLBAR_FONT");
    if (!fixtureFont.isEmpty()) {
        QSettings fixtureSettings(QSettings::defaultFormat(), QSettings::UserScope, "Sentinel", "SentinelGUI");
        fixtureSettings.setValue("ui/fontFamily", fixtureFont);
    }
    FontManager::instance().initialize(&app);
    ::testing::InitGoogleTest(&argc, argv);
    std::cout << "[sentinel] " << lab::describeRhi().toStdString() << std::endl;
    fixtureDir = new QTemporaryDir;
    writeRecording(*fixtureDir, 4 * 60);
    lab::LabData::configure(fixtureDir->path().toStdString(), epoch + 4 * kHourMs);
    return RUN_ALL_TESTS();
}
