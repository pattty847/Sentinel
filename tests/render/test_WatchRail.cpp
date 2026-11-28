#include "widgets/WatchlistDock.hpp"
#include "themes/FontManager.hpp"
#include "themes/ThemeManager.hpp"
#include "themes/WidgetToolTips.hpp"
#include <QHelpEvent>
#include <QToolTip>

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardItemModel>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>
#include <gtest/gtest.h>

namespace {
int rowFor(QStandardItemModel* model, const QString& symbol) {
    for (int row = 0; row < model->rowCount(); ++row)
        if (model->item(row, 0)->data(Qt::UserRole + 1).toString() == symbol ||
            model->item(row, 0)->text() == symbol) return row;
    return -1;
}

TEST(WatchRail, PendingAckRefusalKeyboardAndCatalogTruth) {
    QTemporaryDir settingsDir;
    ASSERT_TRUE(settingsDir.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    WatchlistDock dock;
    auto* tree = dock.findChild<QTreeView*>("watchRows");
    auto* status = dock.findChild<QLabel*>("watchStatus");
    ASSERT_NE(tree, nullptr);
    ASSERT_NE(status, nullptr);
    auto* model = qobject_cast<QStandardItemModel*>(tree->model());
    ASSERT_NE(model, nullptr);
    EXPECT_EQ(model->rowCount(), 7);
    EXPECT_TRUE(status->isHidden());
    QSignalSpy selected(&dock, &WatchlistDock::symbolSelected);
    const int eth = rowFor(model, "ETH-USD");
    ASSERT_GE(eth, 0);
    tree->setCurrentIndex(model->index(eth, 0));
    QTest::keyClick(tree, Qt::Key_Return);
    ASSERT_EQ(selected.count(), 1);
    EXPECT_EQ(selected.at(0).at(0).toString(), "ETH-USD");
    EXPECT_TRUE(status->isHidden()) << "the status bar says the switch; the rail marks the row";
    EXPECT_FALSE(model->item(eth, 0)->text().startsWith("●"));

    dock.setChartSwitchState("ETH-USD", "");
    EXPECT_EQ(model->item(eth, 0)->text(), "ETH-USD");
    EXPECT_TRUE(model->item(eth, 0)->data(WatchlistDock::ActiveRole).toBool());
    EXPECT_EQ(model->item(eth, 0)->font().weight(), QFont::Medium);
    EXPECT_EQ(model->item(eth, 0)->foreground().color(), QColor("#E6EDF3"));
    dock.setChartSwitchState("BTC-USD", "", "ETH-USD", "Connection cap reached");
    EXPECT_TRUE(model->item(eth, 0)->text().startsWith("!"));
    EXPECT_FALSE(status->isHidden());
    EXPECT_TRUE(status->text().contains("Connection cap"));
    dock.setCryptoAvailability({"BTC-USD"}, true, "server catalog");
    QTest::keyClick(tree, Qt::Key_Return);
    EXPECT_EQ(selected.count(), 1);
    EXPECT_TRUE(status->text().contains("unavailable"));
}

TEST(WatchRail, NameColumnUsesTheExpandedDockWidth) {
    WatchlistDock dock;
    EXPECT_EQ(dock.maximumWidth(), QWIDGETSIZE_MAX);
    auto* tree = dock.findChild<QTreeView*>("watchRows");
    dock.resize(320, 400);
    dock.show();
    QCoreApplication::processEvents();
    const int symbolWidth = tree->columnWidth(0);
    const int nameWidth = tree->columnWidth(1);
    dock.resize(640, 400);
    QCoreApplication::processEvents();
    EXPECT_EQ(tree->columnWidth(0), symbolWidth);
    EXPECT_GE(tree->columnWidth(1), nameWidth + 300);
}

TEST(WatchRail, ActiveRowPaintsOnlyTwoAccentPixelsAtTheLeft) {
    WatchlistDock dock;
    dock.resize(500, 400);
    dock.show();
    dock.setChartSwitchState("BTC-USD", "");
    auto* tree = dock.findChild<QTreeView*>("watchRows");
    QCoreApplication::processEvents();
    const auto rect = tree->visualRect(tree->model()->index(0, 0));
    const auto image = tree->viewport()->grab().toImage();
    EXPECT_EQ(image.pixelColor(0, rect.center().y()), QColor("#00AAFF"));
    EXPECT_EQ(image.pixelColor(1, rect.center().y()), QColor("#00AAFF"));
    EXPECT_NE(image.pixelColor(2, rect.center().y()), QColor("#00AAFF"));
    QFont changed("Monaco", 14);
    tree->setFont(changed);
    const auto rowFont = tree->model()->index(0, 0).data(Qt::FontRole).value<QFont>().resolve(tree->font());
    EXPECT_EQ(rowFont.family(), changed.family());
    EXPECT_EQ(rowFont.pointSize(), changed.pointSize());
    EXPECT_EQ(rowFont.weight(), QFont::Medium);
    dock.setChartSwitchState("ETH-USD", "");
    EXPECT_FALSE(tree->model()->index(0, 0).data(WatchlistDock::ActiveRole).toBool());
    EXPECT_EQ(tree->model()->index(0, 0).data(Qt::FontRole).value<QFont>().weight(), QFont::Normal);
}

TEST(WatchRail, StripesStopAfterTheLastFilledRow) {
    WatchlistDock dock;
    dock.resize(500, 600);
    dock.show();
    auto* tree = dock.findChild<QTreeView*>("watchRows");
    QCoreApplication::processEvents();
    EXPECT_FALSE(tree->alternatingRowColors());
    const auto image = tree->viewport()->grab().toImage();
    const auto odd = tree->visualRect(tree->model()->index(1, 1));
    EXPECT_EQ(image.pixelColor(odd.right() - 6, odd.center().y()), QColor("#1e2228"));
    const auto last = tree->visualRect(tree->model()->index(tree->model()->rowCount() - 1, 0));
    const int y = last.bottom() + 10;
    ASSERT_LT(y + last.height(), image.height());
    EXPECT_EQ(image.pixelColor(10, y), image.pixelColor(10, y + last.height()));
}

TEST(WatchRail, ActionsHaveAVisibleSurfaceAndReadableDisabledInk) {
    WatchlistDock dock;
    dock.show();
    QCoreApplication::processEvents();
    dock.findChild<QComboBox*>("watchPreset")->setCurrentIndex(2);
    auto* pin = dock.findChild<QToolButton*>("watchPin");
    auto* add = dock.findChild<QToolButton*>("watchAdd");
    ASSERT_FALSE(pin->isEnabled());
    EXPECT_EQ(pin->palette().color(QPalette::Disabled, QPalette::ButtonText), QColor("#6E7580"));
    EXPECT_EQ(add->palette().color(QPalette::Active, QPalette::ButtonText), QColor("#E6EDF3"));
    for (auto* button : {pin, add}) {
        const auto pixels = button->grab().toImage();
        EXPECT_EQ(pixels.pixelColor(2, button->height() / 2), QColor("#20252B"));
        EXPECT_EQ(pixels.pixelColor(0, button->height() / 2), QColor("#2B3138"));
    }
}

TEST(WatchRail, HoverHighlightsTheRowAndTooltipStaysBesideIt) {
    WatchlistDock dock;
    dock.findChild<QComboBox*>("watchPreset")->setCurrentIndex(0);
    dock.resize(400, 400);
    dock.move(0, 0);
    dock.show();
    dock.setChartSwitchState("BTC-USD", "");
    auto* tree = dock.findChild<QTreeView*>("watchRows");
    QCoreApplication::processEvents();
    const auto first = tree->visualRect(tree->model()->index(0, 0));
    tree->clearSelection();
    tree->setCurrentIndex({});
    QTest::mouseMove(tree->viewport(), first.center());
    QCoreApplication::processEvents();
    const auto pixels = tree->viewport()->grab().toImage();
    EXPECT_EQ(pixels.pixelColor(first.right() - 6, first.center().y()), QColor("#20252B"));
    EXPECT_TRUE(tree->model()->index(0, 0).data(Qt::ToolTipRole).toString().isEmpty());
    // Quiet mode keeps refusal data; a pending marker alone has no tooltip.
    dock.setChartSwitchState("BTC-USD", {}, "ETH-USD", "Not in the current market catalog");
    const QRect tipRow = tree->visualRect(tree->model()->index(1, 0));
    const QPoint oddSample(tipRow.right() - 6, tipRow.center().y());
    const QColor restingOdd = tree->viewport()->grab().toImage().pixelColor(oddSample);
    EXPECT_EQ(restingOdd, QColor("#1e2228"));
    QTest::mouseMove(tree->viewport(), tipRow.center());
    QCoreApplication::processEvents();
    const QColor hoveredOdd = tree->viewport()->grab().toImage().pixelColor(oddSample);
    EXPECT_EQ(hoveredOdd, QColor("#20252B"));
    EXPECT_NE(hoveredOdd, restingOdd);
    auto* tips = WidgetToolTips::install(qApp);
    QHelpEvent help(QEvent::ToolTip, tipRow.center(), tree->viewport()->mapToGlobal(tipRow.center()));
    QApplication::sendEvent(tree->viewport(), &help);
    EXPECT_TRUE(QToolTip::isVisible());
    const QRect row(tree->viewport()->mapToGlobal(QPoint(0, tipRow.top())), QSize(tree->viewport()->width(), tipRow.height()));
    for (auto* widget : QApplication::topLevelWidgets())
        if (widget->objectName() == "qtooltip_label" && widget->isVisible())
            EXPECT_FALSE(widget->geometry().intersects(row));
    QToolTip::hideText();
    delete tips;
}

TEST(WatchRail, FooterSpeaksOnlyForAProblemOnOneWarningLine) {
    WatchlistDock dock;
    dock.findChild<QComboBox*>("watchPreset")->setCurrentIndex(0);
    dock.resize(280, 620);
    dock.show();
    auto* status = dock.findChild<QLabel*>("watchStatus");
    ASSERT_NE(status, nullptr);
    dock.setCryptoAvailability({}, false, "No authoritative runtime market catalog");
    dock.setChartSwitchState("ETH-USD", "");
    QCoreApplication::processEvents();
    EXPECT_TRUE(status->isHidden()) << "a live chart needs no footer: nothing unknown or contradicting";
    dock.setChartSwitchState("ETH-USD", "SOL-USD");
    EXPECT_TRUE(status->isHidden());
    const QString reason = QStringLiteral("SOL-USD subscription refused; the connection cap is reached and ETH-USD stays");
    dock.setChartSwitchState("ETH-USD", "", "SOL-USD", reason);
    QCoreApplication::processEvents();
    ASSERT_FALSE(status->isHidden());
    EXPECT_EQ(status->text(), "! " + reason);
    EXPECT_EQ(status->toolTip(), reason);
    EXPECT_FALSE(status->wordWrap());
    EXPECT_LE(status->height(), status->fontMetrics().height() + 4) << "one line";
    EXPECT_LE(status->width(), dock.width());
    EXPECT_TRUE(status->styleSheet().contains("#e6c07b"));
    dock.setChartSwitchState("ETH-USD", "");
    EXPECT_TRUE(status->isHidden());
}

TEST(WatchRail, ActivePairWithoutQuoteKeepsFooterHiddenWhenCatalogIsKnown) {
    QTemporaryDir settingsDir;
    ASSERT_TRUE(settingsDir.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    WatchlistDock dock;
    dock.findChild<QComboBox*>("watchPreset")->setCurrentIndex(0);
    dock.show();
    auto* status = dock.findChild<QLabel*>("watchStatus");
    auto* tree = dock.findChild<QTreeView*>("watchRows");
    ASSERT_NE(status, nullptr);
    ASSERT_NE(tree, nullptr);

    dock.setCryptoAvailability({"BTC-USD"}, true, "server catalog");
    dock.setChartSwitchState("BTC-USD", {});
    QCoreApplication::processEvents();

    auto* model = qobject_cast<QStandardItemModel*>(tree->model());
    ASSERT_NE(model, nullptr);
    const int activeRow = rowFor(model, "BTC-USD");
    ASSERT_GE(activeRow, 0);
    EXPECT_TRUE(model->item(activeRow, 0)->data(WatchlistDock::ActiveRole).toBool());
    EXPECT_TRUE(status->isHidden());
    EXPECT_TRUE(status->text().isEmpty());
}

TEST(WatchRail, EntryWarningsClearOnTheNextSuccessfulActionButRefusalsStay) {
    QTemporaryDir settingsDir;
    ASSERT_TRUE(settingsDir.isValid());
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    WatchlistDock dock;
    dock.show();
    auto* status = dock.findChild<QLabel*>("watchStatus");
    auto* preset = dock.findChild<QComboBox*>("watchPreset");
    auto* entry = dock.findChild<QLineEdit*>("watchSymbolEntry");
    auto* tree = dock.findChild<QTreeView*>("watchRows");
    // A bad entry, then a successful stock-row activation.
    preset->setCurrentText("Top Stocks");
    entry->setText("not a pair");
    dock.findChild<QToolButton*>("watchAdd")->click();
    ASSERT_FALSE(status->isHidden());
    QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier,
                      tree->visualRect(tree->model()->index(0, 0)).center());
    EXPECT_TRUE(status->isHidden());
    EXPECT_TRUE(status->text().isEmpty());
    // A full Pinned list, then an unpin from the Crypto preset.
    auto add = [&](const QString& pair) {
        entry->setText(pair);
        dock.findChild<QToolButton*>("watchAdd")->click();
    };
    add("BTC-USD");
    for (int i = 1; i < 30; ++i) add(QStringLiteral("P%1-USD").arg(i, 2, 10, QLatin1Char('0')));
    preset->setCurrentText("Crypto");
    dock.setChartSwitchState("SOL-USD", "", "ETH-USD", "Connection cap reached");
    auto select = [&](const QString& symbol) {
        for (int row = 0; row < tree->model()->rowCount(); ++row)
            if (tree->model()->index(row, 0).data(WatchlistDock::TickerRole).toString() == symbol)
                tree->setCurrentIndex(tree->model()->index(row, 0));
    };
    auto* pin = dock.findChild<QToolButton*>("watchPin");
    select("DOGE-USD");
    pin->click();
    ASSERT_EQ(status->text(), "! Pinned list is full (30 pairs)");
    select("BTC-USD");
    pin->click(); // unpin from the Crypto preset
    EXPECT_FALSE(status->text().contains("full"));
    EXPECT_EQ(status->text(), "! Connection cap reached") << "the current refusal stays";
}

TEST(WatchRail, EmptyPinnedListSaysHowToAddAPair) {
    QTemporaryDir settingsDir;
    ASSERT_TRUE(settingsDir.isValid());
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    WatchlistDock dock;
    dock.resize(320, 400);
    dock.show();
    dock.findChild<QComboBox*>("watchPreset")->setCurrentText("Pinned");
    QCoreApplication::processEvents();
    auto* tree = dock.findChild<QTreeView*>("watchRows");
    auto* empty = tree->viewport()->findChild<QWidget*>("watchRowsEmpty");
    ASSERT_NE(empty, nullptr);
    ASSERT_EQ(tree->model()->rowCount(), 0);
    EXPECT_TRUE(empty->isVisible());
    EXPECT_EQ(empty->findChild<QLabel*>()->text(), QString::fromUtf8("No pinned pairs · add one above"));
    dock.findChild<QLineEdit*>("watchSymbolEntry")->setText("XRP-USD");
    dock.findChild<QToolButton*>("watchAdd")->click();
    EXPECT_FALSE(empty->isVisible());
    dock.findChild<QComboBox*>("watchPreset")->setCurrentText("Crypto");
    EXPECT_FALSE(empty->isVisible());
}

TEST(WatchRail, PinsPersistWithDefaultFormatSettings) {
    QTemporaryDir settingsDir;
    ASSERT_TRUE(settingsDir.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    {
        WatchlistDock dock;
        auto* edit = dock.findChild<QLineEdit*>("watchSymbolEntry");
        ASSERT_NE(edit, nullptr);
        edit->setText("XRP-USD");
        dock.findChild<QToolButton*>("watchAdd")->click();
        EXPECT_EQ(dock.findChild<QComboBox*>("watchPreset")->currentText(), "Pinned");
        auto* tree = dock.findChild<QTreeView*>("watchRows");
        EXPECT_EQ(rowFor(qobject_cast<QStandardItemModel*>(tree->model()), "XRP-USD"), 0);
    }
    QSettings settings(QSettings::IniFormat, QSettings::UserScope, "Sentinel", "SentinelTerminal");
    settings.sync();
    EXPECT_TRUE(settings.value("watchRail/pinnedSymbols").toStringList().contains("XRP-USD"));
    WatchlistDock restored;
    auto* tree = restored.findChild<QTreeView*>("watchRows");
    EXPECT_EQ(restored.findChild<QComboBox*>("watchPreset")->currentText(), "Pinned");
    EXPECT_EQ(rowFor(qobject_cast<QStandardItemModel*>(tree->model()), "XRP-USD"), 0);
}

TEST(WatchRail, MouseAndKeyboardDispatchOnce) {
    WatchlistDock dock;
    dock.show();
    auto* tree = dock.findChild<QTreeView*>("watchRows");
    dock.findChild<QComboBox*>("watchPreset")->setCurrentIndex(2); // Stock rows have no pending deduplication.
    auto* model = qobject_cast<QStandardItemModel*>(tree->model());
    const QModelIndex index = model->index(0, 0);
    QSignalSpy selected(&dock, &WatchlistDock::symbolSelected);
    QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, tree->visualRect(index).center());
    EXPECT_EQ(selected.count(), 1);
    QTest::mouseDClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, tree->visualRect(index).center());
    QTest::mouseRelease(tree->viewport(), Qt::LeftButton, Qt::NoModifier, tree->visualRect(index).center());
    EXPECT_EQ(selected.count(), 1);
    tree->setCurrentIndex(index);
    QTest::keyClick(tree, Qt::Key_Return);
    QTest::keyClick(tree, Qt::Key_Space);
    EXPECT_EQ(selected.count(), 3);
}

TEST(WatchRail, NativeVisualFixtures) {
    const QString output = qEnvironmentVariable("SENTINEL_WATCH_RAIL_SHOTS");
    if (output.isEmpty())
        GTEST_SKIP() << "Opt-in widget fixtures require SENTINEL_WATCH_RAIL_SHOTS and an authorized native GUI slot";
    const QString platform = QGuiApplication::platformName();
    if (platform == "offscreen" || platform == "minimal")
        GTEST_SKIP() << "Native fixture capture requires an explicit native QT_QPA_PLATFORM; no offscreen screenshots";
    ASSERT_TRUE(QDir().mkpath(output));
    QTemporaryDir settings;
    ASSERT_TRUE(settings.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());

    auto addFixtureCaption = [](QVBoxLayout& layout, QWidget& frame) {
        auto* caption = new QLabel("FIXTURE · synthetic data and states · no live-provider validation", &frame);
        caption->setWordWrap(true);
        caption->setMargin(6);
        layout.setContentsMargins(0, 0, 0, 0);
        layout.setSpacing(0);
        layout.addWidget(caption);
    };
    auto save = [&](QWidget& frame, const QString& name) {
        frame.show();
        frame.ensurePolished();
        frame.layout()->activate();
        QTest::qWait(100);
        const QPixmap pixels = frame.grab(); // Widget pixels only; never a screen-region capture.
        return !pixels.isNull() && pixels.save(QDir(output).filePath(name + "-fixture.png"));
    };

    QWidget railFrame;
    railFrame.setWindowTitle("W2c Watch rail visual fixture");
    QVBoxLayout railLayout(&railFrame);
    addFixtureCaption(railLayout, railFrame);
    auto* rail = new WatchlistDock(&railFrame);
    railLayout.addWidget(rail);
    railFrame.setFixedWidth(280);
    railFrame.resize(280, 620);
    rail->setCryptoAvailability({}, false);
    ASSERT_TRUE(save(railFrame, "watch-rail-catalog-unknown-280"));
    EXPECT_EQ(rail->width(), 280);
    rail->setChartSwitchState("BTC-USD", "");
    ASSERT_TRUE(save(railFrame, "watch-rail-active-280"));
    rail->setChartSwitchState("BTC-USD", "PEPE-USD");
    ASSERT_TRUE(save(railFrame, "watch-rail-pending-280"));
    rail->setChartSwitchState("BTC-USD", "", "PEPE-USD",
        "Fixture: PEPE-USD subscription refused; connection cap reached. BTC-USD remains active.");
    ASSERT_TRUE(save(railFrame, "watch-rail-refused-280"));
}
} // namespace

int main(int argc, char** argv) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QTemporaryDir settings;
    if (!settings.isValid()) return 1;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    if (!qEnvironmentVariableIsEmpty("SENTINEL_WATCH_RAIL_SHOTS")) {
        Q_INIT_RESOURCE(sentinel_svg_resources);
        Q_INIT_RESOURCE(sentinel_ui_fonts);
        ThemeManager::instance().initializeDefaults();
        ThemeManager::instance().applyTheme("dark", &app);
        FontManager::instance().initialize(&app);
    }
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
