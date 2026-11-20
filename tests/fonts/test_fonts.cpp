#include <gtest/gtest.h>
#include "themes/FontManager.hpp"
#include "themes/ThemeManager.hpp"
#include "themes/ThemePalette.hpp"
#include "widgets/FontSettingsDialog.hpp"
#include "widgets/HeatmapTelemetryDock.hpp"
#include "widgets/PaperTradingDock.hpp"
#include "widgets/StatusBar.hpp"
#include "widgets/WatchlistDock.hpp"
#include "marketdata/model/TradeData.h"
#include <QApplication>
#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QFontDatabase>
#include <QFontInfo>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QTableWidget>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>

namespace {
QString savedInstalledFamily;
QStringList testFamilies;

void settle() { QTest::qWait(30); }
void expectLabels(QWidget& widget, const QString& family, int chromePixels = -1, bool paperScale = false) {
    const auto labels = widget.findChildren<QLabel*>();
    ASSERT_FALSE(labels.isEmpty());
    for (auto* label : labels) {
        SCOPED_TRACE(label->objectName().toStdString() + " " + label->text().toStdString());
        EXPECT_EQ(QFontInfo(label->font()).family(), family);
        if (chromePixels > 0) {
            EXPECT_EQ(label->font().pixelSize(), chromePixels);
        } else if (paperScale && label->font().pointSizeF() < 0) {
            const bool key = label->styleSheet().contains(QStringLiteral("font-size: 16px"));
            EXPECT_TRUE(key || label->styleSheet().contains(QStringLiteral("font-size: 10px")));
            EXPECT_EQ(label->font().pixelSize(), key ? 16 : 10);
        } else {
            EXPECT_DOUBLE_EQ(label->font().pointSizeF(), 10.0);
        }
    }
}

TEST(FontWidgets, SavedUncuratedFamilyAndCloseRemainLiveSaved) {
    ASSERT_FALSE(savedInstalledFamily.isEmpty());
    EXPECT_EQ(FontManager::instance().currentFontFamily(), savedInstalledFamily);
    FontSettingsDialog dialog;
    dialog.show(); settle();
    auto* combo = dialog.findChild<QComboBox*>("uiFontFamily");
    ASSERT_NE(combo, nullptr);
    EXPECT_EQ(combo->currentText(), savedInstalledFamily);
    ASSERT_GE(testFamilies.size(), 2);
    const QString next = testFamilies.front() == savedInstalledFamily ? testFamilies[1] : testFamilies.front();
    combo->setCurrentIndex(combo->findText(next)); settle();
    EXPECT_EQ(FontManager::instance().currentFontFamily(), next);
    QSettings saved(QSettings::defaultFormat(), QSettings::UserScope, "Sentinel", "SentinelGUI");
    saved.sync();
    EXPECT_EQ(saved.value("ui/fontFamily").toString(), next);
    auto* buttons = dialog.findChild<QDialogButtonBox*>();
    ASSERT_NE(buttons, nullptr);
    EXPECT_EQ(buttons->standardButtons(), QDialogButtonBox::Close);
    buttons->button(QDialogButtonBox::Close)->click();
    EXPECT_FALSE(dialog.isVisible());
    ASSERT_TRUE(FontManager::instance().applyFontFamily(savedInstalledFamily, qApp));
    dialog.show(); settle();
    EXPECT_EQ(combo->currentText(), savedInstalledFamily);
    FontSettingsDialog reopened;
    EXPECT_EQ(reopened.findChild<QComboBox*>("uiFontFamily")->currentText(), savedInstalledFamily);
    QSettings disk(saved.fileName(), QSettings::IniFormat);
    saved.sync(); disk.sync();
    EXPECT_EQ(disk.value("ui/fontFamily").toString(), savedInstalledFamily);
}

TEST(FontWidgets, TwoInstalledFamiliesReachExistingHiddenAndRecreatedWidgets) {
    ASSERT_GE(testFamilies.size(), 2) << "Two installed fonts are required to exercise propagation";
    StatusBar status;
    HeatmapTelemetryDock telemetry;
    FontSettingsDialog dialog;
    status.show(); telemetry.show(); dialog.show(); settle();
    for (const auto& requested : testFamilies.mid(0, 2)) {
        SCOPED_TRACE(requested.toStdString());
        ASSERT_TRUE(FontManager::instance().applyFontFamily(requested, qApp)); settle();
        const auto family = FontManager::instance().currentFontFamily();
        expectLabels(status, family, 11); expectLabels(telemetry, family); expectLabels(dialog, family);
        EXPECT_FALSE(status.findChild<QLabel*>("marketSymbol")->font().bold());
        EXPECT_TRUE(telemetry.findChild<QLabel*>("telemetry.marketHealth")->font().bold());
        status.hide(); telemetry.hide(); dialog.hide();
        const auto other = requested == testFamilies[0] ? testFamilies[1] : testFamilies[0];
        ASSERT_TRUE(FontManager::instance().applyFontFamily(other, qApp)); settle();
        expectLabels(status, other, 11); expectLabels(telemetry, other); expectLabels(dialog, other);
        status.show(); telemetry.show(); dialog.show(); settle();
        expectLabels(status, other, 11); expectLabels(telemetry, other);
        EXPECT_EQ(dialog.findChild<QComboBox*>("uiFontFamily")->currentText(), other);
        StatusBar newStatus; HeatmapTelemetryDock newTelemetry;
        newStatus.ensurePolished(); newTelemetry.ensurePolished();
        expectLabels(newStatus, other, 11); expectLabels(newTelemetry, other);
    }
}

TEST(FontWidgets, InheritedPixelFontRemainsReadableAndSelectionRestoresPointBaseline) {
    StatusBar status; HeatmapTelemetryDock telemetry;
    QFont pixelFont(testFamilies.front()); pixelFont.setPixelSize(14);
    qApp->setFont(pixelFont);
    qApp->setStyleSheet(qApp->styleSheet());
    status.show(); telemetry.show(); settle();
    for (QWidget* widget : {static_cast<QWidget*>(&status), static_cast<QWidget*>(&telemetry)}) {
        for (auto* label : widget->findChildren<QLabel*>()) {
            EXPECT_EQ(label->font().pixelSize(), widget == &status ? 11 : 14);
            EXPECT_GE(label->fontMetrics().height(), 10);
        }
    }
    ASSERT_TRUE(FontManager::instance().applyFontFamily(testFamilies.front(), qApp)); settle();
    expectLabels(status, FontManager::instance().currentFontFamily(), 11);
    expectLabels(telemetry, FontManager::instance().currentFontFamily());
}

TEST(FontWidgets, PaperTradingRuntimeAndHiddenWidgetsInheritSelectedFont) {
    PaperTradingDock paper;
    WatchlistDock watch;
    paper.setSymbol(QStringLiteral("BTC-USD"));
    paper.show(); watch.show(); settle();

    for (const QString& requested : {testFamilies.front(), savedInstalledFamily}) {
        SCOPED_TRACE(requested.toStdString());
        ASSERT_TRUE(FontManager::instance().applyFontFamily(requested, qApp)); settle();
        const QString family = FontManager::instance().currentFontFamily();
        expectLabels(paper, family, -1, true);
        expectLabels(watch, family);
        for (QWidget* view : {static_cast<QWidget*>(paper.findChild<QTableWidget*>("paperOrderLog")),
                              static_cast<QWidget*>(paper.findChild<QPlainTextEdit*>()),
                              static_cast<QWidget*>(watch.findChild<QTreeView*>("watchRows"))}) {
            ASSERT_NE(view, nullptr);
            EXPECT_EQ(QFontInfo(view->font()).family(), family);
            EXPECT_DOUBLE_EQ(view->font().pointSizeF(), 10.0);
        }

        paper.onTradeReceived({std::chrono::system_clock::now(), "BTC-USD", "font-fixture",
                               AggressorSide::Buy, 71023.125, 1.0});
        auto* algo = paper.findChild<QLabel*>("paperAlgoStatus");
        ASSERT_NE(algo, nullptr);
        EXPECT_TRUE(algo->font().bold());
        EXPECT_EQ(QFontInfo(algo->font()).family(), family);
        bool foundPrice = false;
        for (auto* label : paper.findChildren<QLabel*>()) {
            if (label->text().startsWith(QStringLiteral("$71,023"))) {
                foundPrice = true;
                EXPECT_EQ(QFontInfo(label->font()).family(), family);
                EXPECT_TRUE(label->styleSheet().contains(theme::text().name()));
            }
        }
        EXPECT_TRUE(foundPrice);
        paper.setSymbol(QStringLiteral("ETH-USD"));
        for (auto* label : paper.findChildren<QLabel*>()) {
            if (label->text() == QStringLiteral("—"))
                EXPECT_EQ(QFontInfo(label->font()).family(), family);
        }
        paper.setSymbol(QStringLiteral("BTC-USD"));

        auto* tabs = paper.findChild<QTabWidget*>();
        ASSERT_NE(tabs, nullptr);
        tabs->setCurrentIndex(2);
        // Journal is now the default; explicitly exercise the existing File error path.
        auto* backtestSource = paper.findChild<QComboBox*>("backtestSource");
        ASSERT_NE(backtestSource, nullptr);
        backtestSource->setCurrentIndex(1);
        for (auto* button : paper.findChildren<QPushButton*>()) {
            if (button->text().contains(QStringLiteral("Run Backtest"))) button->click();
        }
        bool foundError = false;
        for (auto* label : paper.findChildren<QLabel*>()) {
            if (label->text().contains(QStringLiteral("no trade log"))) {
                foundError = true;
                EXPECT_EQ(QFontInfo(label->font()).family(), family);
                EXPECT_TRUE(label->styleSheet().contains(QStringLiteral("#f44336")));
            }
        }
        EXPECT_TRUE(foundError);
        watch.setChartSwitchState(QStringLiteral("BTC-USD"), {}, QStringLiteral("ETH-USD"),
                                  QStringLiteral("Synthetic refusal"));
        expectLabels(watch, family);
        paper.hide(); watch.hide();
    }
    ASSERT_TRUE(FontManager::instance().applyFontFamily(testFamilies.front(), qApp)); settle();
    paper.show(); watch.show(); settle();
    const QString family = FontManager::instance().currentFontFamily();
    expectLabels(paper, family, -1, true); expectLabels(watch, family);
    PaperTradingDock newPaper; WatchlistDock newWatch;
    newPaper.ensurePolished(); newWatch.ensurePolished();
    expectLabels(newPaper, family, -1, true); expectLabels(newWatch, family);
}

TEST(FontWidgets, NativeVisualFixtures) {
    const auto output = qEnvironmentVariable("SENTINEL_FONT_SHOTS");
    if (output.isEmpty()) GTEST_SKIP() << "Opt-in native widget screenshots require an authorized GUI slot";
    ASSERT_TRUE(QDir().mkpath(output));
    ASSERT_GE(testFamilies.size(), 2);
    HeatmapTelemetryDock dock;
    StatusBar status;
    auto provider = []() -> std::optional<QVariantMap> {
        return QVariantMap{{"mode", "Auto"}, {"tick", 5.0}, {"connection", "Synthetic fixture"},
            {"frameMs", 2.0}, {"frameP95Ms", 3.0}, {"loadingSlots", 0}, {"settled", true}};
    };
    auto* toggle = dock.findChild<QToolButton*>(); ASSERT_NE(toggle, nullptr);
    auto save = [&](QWidget& widget, const QString& name) {
        settle(); dock.refresh(); return widget.grab().save(QDir(output).filePath(name + "-fixture.png"));
    };
    for (int i = 0; i < 2; ++i) {
        MarketHealth health;
        health.setTransport(MarketHealth::Transport::Connected);
        health.setActiveSymbol("BTC-USD");
        dock.setMarketHealth(&health); status.setMarketHealth(&health);
        dock.setProvider(provider);
        health.subscriptionRequested("BTC-USD"); health.subscriptionAcknowledged("BTC-USD");
        health.bookReceived("BTC-USD", QDateTime::currentMSecsSinceEpoch());
        ASSERT_TRUE(FontManager::instance().applyFontFamily(testFamilies[i], qApp));
        const QString prefix = QString::number(i) + "-" + testFamilies[i].simplified().replace(' ', '-');
        for (int width : {440, 1000}) {
            dock.resize(width, 720); dock.show(); status.resize(width, 30); status.show();
            toggle->setChecked(false);
            ASSERT_TRUE(save(dock, prefix + "-collapsed-" + QString::number(width)));
            toggle->setChecked(true);
            ASSERT_TRUE(save(dock, prefix + "-expanded-" + QString::number(width)));
            ASSERT_TRUE(save(status, prefix + "-status-" + QString::number(width)));
        }
        health.setTransport(MarketHealth::Transport::Reconnecting);
        ASSERT_TRUE(save(status, prefix + "-status-reconnecting"));
        dock.setProvider({}); toggle->setChecked(false);
        ASSERT_TRUE(save(dock, prefix + "-unavailable"));
        health.setTransport(MarketHealth::Transport::Connected);
        FontSettingsDialog dialog; dialog.show();
        ASSERT_TRUE(save(dialog, prefix + "-font-dialog"));
    }
}

TEST(FontWidgets, NativeRetainedFontFixtures) {
    const QString output = qEnvironmentVariable("SENTINEL_FONT_SHOTS");
    if (output.isEmpty()) GTEST_SKIP() << "Opt-in native fixtures require an authorized GUI slot";
    if (QGuiApplication::platformName() == QStringLiteral("offscreen") ||
        QGuiApplication::platformName() == QStringLiteral("minimal"))
        GTEST_SKIP() << "Native fixture capture requires a native Qt platform";
    ASSERT_TRUE(QDir().mkpath(output));

    QWidget paperFrame;
    QVBoxLayout paperLayout(&paperFrame);
    paperLayout.addWidget(new QLabel(QStringLiteral("Synthetic font fixture — Paper Trading"), &paperFrame));
    PaperTradingDock paper(&paperFrame);
    paperLayout.addWidget(&paper);
    paper.setSymbol(QStringLiteral("BTC-USD"));
    paper.onTradeReceived({std::chrono::system_clock::now(), "BTC-USD", "font-fixture",
                           AggressorSide::Buy, 71023.125, 1.0});
    auto* paperTabs = paper.findChild<QTabWidget*>();
    ASSERT_NE(paperTabs, nullptr);

    QWidget watchFrame;
    QVBoxLayout watchLayout(&watchFrame);
    watchLayout.addWidget(new QLabel(QStringLiteral("Synthetic font fixture — Watch rail state"), &watchFrame));
    WatchlistDock watch(&watchFrame);
    watchLayout.addWidget(&watch);
    watch.setCryptoAvailability({}, false);
    watch.setChartSwitchState(QStringLiteral("BTC-USD"), QStringLiteral("ETH-USD"));

    auto save = [&](QWidget& widget, const QString& name) {
        settle();
        return widget.grab().save(QDir(output).filePath(name + QStringLiteral("-fixture.png")));
    };
    for (const QString& requested : {QStringLiteral("Roboto Mono"), savedInstalledFamily}) {
        ASSERT_TRUE(FontManager::instance().applyFontFamily(requested, qApp)); settle();
        const QString prefix = requested.simplified().replace(' ', '-');
        for (int width : {520, 1000}) {
            paperFrame.resize(width, 650); paperFrame.show();
            paperTabs->setCurrentIndex(0);
            ASSERT_TRUE(save(paperFrame, prefix + "-paper-manual-" + QString::number(width)));
            paperTabs->setCurrentIndex(1);
            ASSERT_TRUE(save(paperFrame, prefix + "-paper-algo-" + QString::number(width)));
            paperTabs->setCurrentIndex(2);
            ASSERT_TRUE(save(paperFrame, prefix + "-paper-backtest-" + QString::number(width)));
        }
        watchFrame.resize(280, 620); watchFrame.show();
        ASSERT_TRUE(save(watchFrame, prefix + "-watch-280"));
    }
}
}

int main(int argc, char** argv) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QTemporaryDir settings;
    if (!settings.isValid()) return 2;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    Q_INIT_RESOURCE(sentinel_ui_fonts);
    const auto installed = QFontDatabase::families();
    // Seed an installed family outside FontManager's curated choices before
    // initialization, as a previous application run/user preference may do.
    for (const auto& candidate : {QString("Helvetica"), QString("Arial"), QString("DejaVu Serif")}) {
        if (installed.contains(candidate)) { savedInstalledFamily = QFontInfo(QFont(candidate)).family(); break; }
    }
    if (savedInstalledFamily.isEmpty()) {
        const QStringList curated{"Inter", "IBM Plex Sans", "Noto Sans", "Ubuntu", "DejaVu Sans", "Liberation Sans", "Roboto Mono"};
        for (const auto& candidate : installed) {
            if (!curated.contains(candidate)) { savedInstalledFamily = QFontInfo(QFont(candidate)).family(); break; }
        }
    }
    QSettings saved(QSettings::defaultFormat(), QSettings::UserScope, "Sentinel", "SentinelGUI");
    saved.setValue("ui/fontFamily", savedInstalledFamily); saved.sync();
    ThemeManager::instance().initializeDefaults(); ThemeManager::instance().applyTheme("dark", &app);
    FontManager::instance().initialize(&app);
    testFamilies = FontManager::instance().availableFonts();
    qInfo() << "Font fixture installed families:" << QFontDatabase::families().size()
            << "selectable:" << testFamilies << "saved outside curated list:" << savedInstalledFamily;
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
