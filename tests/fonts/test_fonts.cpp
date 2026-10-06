#include <gtest/gtest.h>
#include "themes/FontManager.hpp"
#include "themes/ThemeManager.hpp"
#include "widgets/FontSettingsDialog.hpp"
#include "widgets/HeatmapTelemetryDock.hpp"
#include "widgets/StatusBar.hpp"
#include <QApplication>
#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QFontDatabase>
#include <QFontInfo>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>

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
