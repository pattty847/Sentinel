#include <gtest/gtest.h>
#include "themes/FontManager.hpp"
#include "themes/ThemeManager.hpp"
#include "widgets/FontSettingsDialog.hpp"
#include "widgets/HeatmapTelemetryDock.hpp"
#include "widgets/PaperTradingDock.hpp"
#include "widgets/ScreenerDock.hpp"
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
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QStandardItemModel>
#include <QTableView>
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
void expectLabels(QWidget& widget, const QString& family) {
    const auto labels = widget.findChildren<QLabel*>();
    ASSERT_FALSE(labels.isEmpty());
    for (auto* label : labels) {
        SCOPED_TRACE(label->objectName().toStdString() + " " + label->text().toStdString());
        EXPECT_EQ(QFontInfo(label->font()).family(), family);
        EXPECT_DOUBLE_EQ(label->font().pointSizeF(), 10.0);
    }
}

void updateScreener(ScreenerDock& dock, const QJsonArray& rows) {
    ASSERT_TRUE(QMetaObject::invokeMethod(&dock, "onScreenerUpdate", Qt::DirectConnection,
        Q_ARG(QString, QStringLiteral("crypto")), Q_ARG(int, rows.size()),
        Q_ARG(QByteArray, QJsonDocument(rows).toJson(QJsonDocument::Compact))));
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
        expectLabels(status, family); expectLabels(telemetry, family); expectLabels(dialog, family);
        EXPECT_TRUE(status.findChild<QLabel*>("marketSymbol")->font().bold());
        EXPECT_TRUE(telemetry.findChild<QLabel*>("telemetry.marketHealth")->font().bold());
        status.hide(); telemetry.hide(); dialog.hide();
        const auto other = requested == testFamilies[0] ? testFamilies[1] : testFamilies[0];
        ASSERT_TRUE(FontManager::instance().applyFontFamily(other, qApp)); settle();
        expectLabels(status, other); expectLabels(telemetry, other); expectLabels(dialog, other);
        status.show(); telemetry.show(); dialog.show(); settle();
        expectLabels(status, other); expectLabels(telemetry, other);
        EXPECT_EQ(dialog.findChild<QComboBox*>("uiFontFamily")->currentText(), other);
        StatusBar newStatus; HeatmapTelemetryDock newTelemetry;
        newStatus.ensurePolished(); newTelemetry.ensurePolished();
        expectLabels(newStatus, other); expectLabels(newTelemetry, other);
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
            EXPECT_EQ(label->font().pixelSize(), 14);
            EXPECT_GE(label->fontMetrics().height(), 10);
        }
    }
    ASSERT_TRUE(FontManager::instance().applyFontFamily(testFamilies.front(), qApp)); settle();
    expectLabels(status, FontManager::instance().currentFontFamily());
    expectLabels(telemetry, FontManager::instance().currentFontFamily());
}

TEST(FontWidgets, PaperTradingRuntimeAndHiddenWidgetsInheritSelectedFont) {
    PaperTradingDock paper;
    WatchlistDock watch;
    ScreenerDock screener;
    paper.setSymbol(QStringLiteral("BTC-USD"));
    paper.show(); watch.show(); screener.show(); settle();

    for (const QString& requested : {testFamilies.front(), savedInstalledFamily}) {
        SCOPED_TRACE(requested.toStdString());
        ASSERT_TRUE(FontManager::instance().applyFontFamily(requested, qApp)); settle();
        const QString family = FontManager::instance().currentFontFamily();
        expectLabels(paper, family);
        expectLabels(watch, family);
        expectLabels(screener, family);
        for (QWidget* view : {static_cast<QWidget*>(paper.findChild<QTableWidget*>("paperOrderLog")),
                              static_cast<QWidget*>(paper.findChild<QPlainTextEdit*>()),
                              static_cast<QWidget*>(watch.findChild<QTreeView*>("watchRows")),
                              static_cast<QWidget*>(screener.findChild<QTableView*>("screenerRows"))}) {
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
            if (label->text().startsWith(QStringLiteral("$71023"))) {
                foundPrice = true;
                EXPECT_EQ(QFontInfo(label->font()).family(), family);
                EXPECT_TRUE(label->styleSheet().contains(QStringLiteral("#4caf50")));
            }
        }
        EXPECT_TRUE(foundPrice);
        paper.setSymbol(QStringLiteral("ETH-USD"));
        for (auto* label : paper.findChildren<QLabel*>()) {
            if (label->text() == QStringLiteral("---"))
                EXPECT_EQ(QFontInfo(label->font()).family(), family);
        }
        paper.setSymbol(QStringLiteral("BTC-USD"));

        auto* tabs = paper.findChild<QTabWidget*>();
        ASSERT_NE(tabs, nullptr);
        tabs->setCurrentIndex(2);
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
        screener.showServiceError(QStringLiteral("Synthetic service error"));
        expectLabels(watch, family);
        expectLabels(screener, family);
        EXPECT_TRUE(screener.findChild<QLabel*>("screenerStatus")->styleSheet().contains(QStringLiteral("#ef5c55")));
        paper.hide(); watch.hide(); screener.hide();
    }
    ASSERT_TRUE(FontManager::instance().applyFontFamily(testFamilies.front(), qApp)); settle();
    paper.show(); watch.show(); screener.show(); settle();
    const QString family = FontManager::instance().currentFontFamily();
    expectLabels(paper, family); expectLabels(watch, family); expectLabels(screener, family);
    PaperTradingDock newPaper; WatchlistDock newWatch; ScreenerDock newScreener;
    newPaper.ensurePolished(); newWatch.ensurePolished(); newScreener.ensurePolished();
    expectLabels(newPaper, family); expectLabels(newWatch, family); expectLabels(newScreener, family);
}

TEST(FontWidgets, ScreenerFontChangeRemeasuresDefaultsAndPreservesUserContext) {
    ASSERT_TRUE(FontManager::instance().applyFontFamily(testFamilies.front(), qApp));
    ScreenerDock dock;
    dock.resize(920, 340); dock.show(); settle();
    auto* table = dock.findChild<QTableView*>("screenerRows");
    ASSERT_NE(table, nullptr);
    QJsonArray rows;
    for (int i = 0; i < 35; ++i) {
        rows.append(QJsonObject{{"symbol", QStringLiteral("ASSET%1-USD").arg(i, 2, 10, QLatin1Char('0'))},
            {"Name", QStringLiteral("Reference market pair number %1").arg(i)},
            {"Price", 12345.67 + i}, {"Change %", 1.25}, {"Volume", 123456.0}});
    }
    updateScreener(dock, rows); settle();
    auto* model = qobject_cast<QStandardItemModel*>(table->model());
    ASSERT_NE(model, nullptr);
    ASSERT_EQ(model->rowCount(), rows.size());
    const int defaultSymbolWidth = table->columnWidth(0);
    const int defaultNameWidth = table->columnWidth(1);
    table->setColumnWidth(2, 173);
    table->sortByColumn(2, Qt::DescendingOrder);
    table->selectRow(12);
    const QString selected = table->currentIndex().siblingAtColumn(0).data().toString();
    table->verticalScrollBar()->setValue(7);
    table->horizontalScrollBar()->setValue(25);
    const int vertical = table->verticalScrollBar()->value();
    const int horizontal = table->horizontalScrollBar()->value();

    ASSERT_TRUE(FontManager::instance().applyFontFamily(savedInstalledFamily, qApp)); settle();
    EXPECT_EQ(table->columnWidth(2), 173);
    EXPECT_NE(table->columnWidth(0), defaultSymbolWidth);
    EXPECT_NE(table->columnWidth(1), defaultNameWidth);
    EXPECT_EQ(table->currentIndex().siblingAtColumn(0).data().toString(), selected);
    EXPECT_EQ(table->horizontalHeader()->sortIndicatorSection(), 2);
    EXPECT_EQ(table->horizontalHeader()->sortIndicatorOrder(), Qt::DescendingOrder);
    EXPECT_EQ(table->verticalScrollBar()->value(), vertical);
    EXPECT_EQ(table->horizontalScrollBar()->value(), horizontal);

    table->setColumnWidth(1, 210);
    dock.hide();
    ASSERT_TRUE(FontManager::instance().applyFontFamily(testFamilies.front(), qApp)); settle();
    EXPECT_EQ(table->columnWidth(1), 210);
    EXPECT_EQ(table->columnWidth(2), 173);
    dock.show(); settle();
    EXPECT_EQ(table->columnWidth(1), 210);
    EXPECT_EQ(table->currentIndex().siblingAtColumn(0).data().toString(), selected);
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

    QWidget screenerFrame;
    QVBoxLayout screenerLayout(&screenerFrame);
    screenerLayout.addWidget(new QLabel(QStringLiteral("Synthetic font fixture — Screener rows"), &screenerFrame));
    ScreenerDock screener(&screenerFrame);
    screenerLayout.addWidget(&screener);
    QJsonArray rows;
    for (int i = 0; i < 30; ++i)
        rows.append(QJsonObject{{"symbol", QStringLiteral("PAIR%1-USD").arg(i, 2, 10, QLatin1Char('0'))},
            {"Name", QStringLiteral("Synthetic market pair %1 with a longer name").arg(i)},
            {"Price", 71023.125 + i}, {"Change %", 2.37}, {"Volume", 18002342.0}});
    updateScreener(screener, rows);

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
            screenerFrame.resize(width, 650); screenerFrame.show();
            paperTabs->setCurrentIndex(0);
            ASSERT_TRUE(save(paperFrame, prefix + "-paper-manual-" + QString::number(width)));
            paperTabs->setCurrentIndex(1);
            ASSERT_TRUE(save(paperFrame, prefix + "-paper-algo-" + QString::number(width)));
            paperTabs->setCurrentIndex(2);
            ASSERT_TRUE(save(paperFrame, prefix + "-paper-backtest-" + QString::number(width)));
            ASSERT_TRUE(save(screenerFrame, prefix + "-screener-" + QString::number(width)));
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
