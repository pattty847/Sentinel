#include "widgets/DockablePanel.hpp"
#include "widgets/WatchlistDock.hpp"
#include "widgets/ScreenerDock.hpp"
#include "widgets/OrderBookDock.hpp"
#include "widgets/SecFilingDock.hpp"
#include "widgets/HeatmapSettingsDialog.hpp"
#include "themes/DarkTheme.hpp"
#include <QSplitter>
#include <QMenu>
#include <QPainter>
#include <QStyleOption>
#include <QHeaderView>
#include <QTableWidget>
#include <QComboBox>
#include <QApplication>
#include <QMainWindow>
#include "widgets/StatusBar.hpp"
#include "widgets/BookAgeText.hpp"
#include "widgets/HeatmapTelemetryDock.hpp"
#include "datasources/RemoteGridDataSource.hpp"
#include <QDateTime>
#include <QLabel>
#include <QFrame>
#include <QStatusBar>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <gtest/gtest.h>

namespace {
class Panel final : public DockablePanel {
public:
    Panel(const QString& id, QWidget* parent) : DockablePanel(id, id, parent) {}
    void buildUi() override {}
};
TEST(ShellChrome, TablesShareRowGeometryAndHeaderTypography) {
    ScreenerDock screener;
    SecFilingDock sec;
    OrderBookDock dom;
    HeatmapGradientEditor gradient("gradient");
    gradient.setStops({{0.0, "#000000"}, {1.0, "#ffffff"}});
    QList<QTableView*> tables = screener.findChildren<QTableView*>();
    tables.append(sec.findChildren<QTableView*>());
    tables.append(dom.findChildren<QTableView*>());
    tables.append(gradient.table());
    ASSERT_EQ(tables.size(), 5);
    for (auto* table : tables) {
        table->ensurePolished();
        auto* header = table->horizontalHeader();
        EXPECT_EQ(table->verticalHeader()->defaultSectionSize(), 22);
        EXPECT_EQ(table->verticalHeader()->minimumSectionSize(), 22);
        EXPECT_EQ(table->verticalHeader()->maximumSectionSize(), 22);
        EXPECT_EQ(header->font().pixelSize(), 11);
        EXPECT_EQ(header->font().weight(), QFont::Normal);
        EXPECT_FALSE(header->highlightSections());
        EXPECT_TRUE(header->styleSheet().contains("#93a1af"));
    }
    EXPECT_EQ(gradient.table()->rowHeight(0), 22);
    auto* screen = screener.findChild<QTableView*>("screenerRows");
    for (int asset : {0, 1}) {
        screener.findChild<QComboBox*>("screenerAsset")->setCurrentIndex(asset);
        for (int column = 0; column < screen->model()->columnCount(); ++column) {
            const bool number = (column >= 2 && column <= 6) || (asset == 1 && (column == 7 || column == 8));
            EXPECT_EQ(screen->model()->headerData(column, Qt::Horizontal, Qt::TextAlignmentRole).toInt(),
                      int((number ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter));
        }
    }
    auto* tx = sec.findChild<QTableView*>("secTransactions");
    EXPECT_EQ(tx->model()->headerData(3, Qt::Horizontal, Qt::TextAlignmentRole).toInt(), int(Qt::AlignRight | Qt::AlignVCenter));
    WatchlistDock watch;
    watch.show();
    QCoreApplication::processEvents();
    auto* tree = watch.findChild<QTreeView*>("watchRows");
    EXPECT_EQ(tree->visualRect(tree->model()->index(0, 0)).height(), 22);
    EXPECT_EQ(tree->header()->font().pixelSize(), 11);
    EXPECT_EQ(tree->header()->defaultAlignment(), Qt::AlignLeft | Qt::AlignVCenter);
}

TEST(ShellChrome, SplittersKeepAOnePixelLineInsideAFivePixelHandle) {
    const QString previous = qApp->styleSheet();
    qApp->setStyleSheet(DarkTheme().stylesheet());
    for (auto orientation : {Qt::Horizontal, Qt::Vertical}) {
        QSplitter splitter(orientation);
        splitter.addWidget(new QWidget);
        splitter.addWidget(new QWidget);
        splitter.resize(300, 300);
        splitter.show();
        QCoreApplication::processEvents();
        auto* handle = splitter.handle(1);
        EXPECT_EQ(orientation == Qt::Horizontal ? handle->width() : handle->height(), 5);
        const auto pixels = handle->grab().toImage();
        int lines = 0;
        for (int p = 0; p < 5; ++p) {
            const QColor pixel = orientation == Qt::Horizontal ? pixels.pixelColor(p, 20) : pixels.pixelColor(20, p);
            if (pixel == QColor("#2B3138")) ++lines;
        }
        EXPECT_EQ(lines, 1);
        QTest::mouseMove(handle, handle->rect().center());
        QCoreApplication::processEvents();
        EXPECT_NE(handle->grab().toImage(), pixels);
        QTest::mousePress(handle, Qt::LeftButton, Qt::NoModifier, handle->rect().center());
        EXPECT_NE(handle->grab().toImage(), pixels);
        QTest::mouseRelease(handle, Qt::LeftButton, Qt::NoModifier, handle->rect().center());
    }
    QMainWindow host;
    host.ensurePolished();
    EXPECT_EQ(host.style()->pixelMetric(QStyle::PM_DockWidgetSeparatorExtent, nullptr, &host), 5);
    qApp->setStyleSheet(previous);
}

TEST(ShellChrome, StatusFieldsRemainOneLeftClusterAtWideSizes) {
    StatusBar bar;
    bar.resize(1200, 28);
    bar.show();
    QCoreApplication::processEvents();
    auto* symbol = bar.findChild<QLabel*>("marketSymbol");
    auto* state = bar.findChild<QLabel*>("marketState");
    auto* age = bar.findChild<QLabel*>("marketBookAge");
    EXPECT_EQ(symbol->x(), 8);
    EXPECT_LT(state->x() - symbol->geometry().right(), 24);
    EXPECT_LT(age->x() - state->geometry().right(), 24);
    EXPECT_LT(age->geometry().right(), 500);
    const auto dividers = bar.findChildren<QFrame*>("statusSeparator");
    ASSERT_EQ(dividers.size(), 2);
    for (auto* divider : dividers) EXPECT_EQ(divider->width(), 1);
}

TEST(ShellChrome, BookAgeHasOneSpellingAndDomOnlyRepeatsAbnormalState) {
    EXPECT_EQ(bookAgeText(std::nullopt), "Book age: Unknown");
    EXPECT_EQ(bookAgeText(100), "Book age: 0.1s");
    EXPECT_EQ(bookAgeText(60000), "Book age: 1m");
    StatusBar bar;
    bar.show();
    EXPECT_EQ(bar.findChild<QLabel*>("marketBookAge")->text(), "Book age: Unknown");
    RemoteGridDataSource source("127.0.0.1", "1");
    auto* health = source.marketHealth();
    health->setActiveSymbol("BTC-USD");
    health->setTransport(MarketHealth::Transport::Connected);
    health->subscriptionAcknowledged("BTC-USD");
    health->bookReceived("BTC-USD", QDateTime::currentMSecsSinceEpoch());
    bar.setMarketHealth(health);
    EXPECT_TRUE(bar.findChild<QLabel*>("marketBookAge")->text().startsWith("Book age: "));
    HeatmapTelemetryDock telemetry;
    telemetry.setMarketHealth(health);
    telemetry.show();
    telemetry.refresh();
    EXPECT_TRUE(telemetry.valueText("marketDetail").contains("Book age: "));
    EXPECT_FALSE(telemetry.valueText("marketDetail").contains("Book receive age"));
    OrderBookDock dom(nullptr, &source);
    dom.onSymbolChanged("BTC-USD");
    dom.show();
    auto refresh = [&] {
        QCoreApplication::processEvents();
        QMetaObject::invokeMethod(&dom, "refreshDisplay", Qt::DirectConnection);
    };
    refresh();
    auto* state = dom.findChild<QLabel*>("domFreshness");
    ASSERT_TRUE(state);
    EXPECT_TRUE(state->isHidden());
    health->bookStale("BTC-USD");
    refresh();
    EXPECT_FALSE(state->isHidden());
    EXPECT_EQ(state->text(), "Stale");
}

TEST(ShellChrome, MenuTogglesHaveAnEmptyIndicatorAndADistinctCheckedState) {
    QMenu menu;
    menu.setStyleSheet(DarkTheme().stylesheet());
    auto* toggle = menu.addAction("Trades");
    toggle->setCheckable(true);
    menu.addAction("Plain command");
    menu.show();
    menu.setActiveAction(nullptr);
    QCoreApplication::processEvents();
    const QRect row = menu.actionGeometry(toggle);
    auto off = menu.grab().toImage();
    int border = 0;
    for (int y = row.top(); y <= row.bottom(); ++y)
        for (int x = row.left(); x < row.left() + 24; ++x)
            border += off.pixelColor(x, y) == QColor("#93A1AF");
    EXPECT_GE(border, 24);
    toggle->setChecked(true);
    auto on = menu.grab().toImage();
    EXPECT_NE(on.copy(QRect(row.left(), row.top(), 24, row.height())),
              off.copy(QRect(row.left(), row.top(), 24, row.height())));
}

TEST(ShellChrome, MenuChecksAndIconsUseTheSameLeftColumn) {
    QMenu menu;
    menu.setStyleSheet(DarkTheme().stylesheet());
    auto* toggle = menu.addAction("Toggle");
    toggle->setCheckable(true);
    QPixmap icon(14, 14);
    icon.fill(Qt::red);
    auto* command = menu.addAction(QIcon(icon), "Command");
    command->setIconVisibleInMenu(true);
    menu.show();
    menu.setActiveAction(nullptr);
    QCoreApplication::processEvents();
    const auto pixels = menu.grab().toImage();
    const auto leftEdge = [&](QAction* action, QColor color) {
        const QRect row = menu.actionGeometry(action);
        int left = pixels.width();
        for (int y = row.top(); y <= row.bottom(); ++y)
            for (int x = row.left(); x < row.left() + 24; ++x)
                if (pixels.pixelColor(x, y) == color) left = std::min(left, x);
        return left;
    };
    const int checkLeft = leftEdge(toggle, QColor("#93A1AF"));
    const int iconLeft = leftEdge(command, Qt::red);
    EXPECT_LT(checkLeft, 24);
    EXPECT_EQ(checkLeft, iconLeft);
}

TEST(ShellChrome, MenuChevronsUseTheDimSixByEightAsset) {
    QMenu menu;
    menu.setStyleSheet(DarkTheme().stylesheet());
    menu.addMenu("Submenu");
    menu.show();
    menu.setActiveAction(nullptr);
    QCoreApplication::processEvents();
    const auto pixels = menu.grab().toImage();
    QRect mark;
    for (int y = 2; y < pixels.height() - 2; ++y)
        for (int x = pixels.width() - 22; x < pixels.width() - 2; ++x) {
            const QColor color = pixels.pixelColor(x, y);
            if (color.blue() > 120 && color.red() < 180 && color.blue() > color.red())
                mark = mark.united(QRect(x, y, 1, 1));
        }
    EXPECT_FALSE(mark.isEmpty());
    EXPECT_LE(mark.width(), 6);
    EXPECT_LE(mark.height(), 8);
}

TEST(ShellChrome, TitlesFollowTabSplitHideFloatAndRestore) {
    QMainWindow host;
    host.resize(900, 500);
    auto* a = new Panel("a", &host);
    auto* b = new Panel("b", &host);
    host.addDockWidget(Qt::LeftDockWidgetArea, a);
    host.addDockWidget(Qt::RightDockWidgetArea, b);
    host.show();
    QCoreApplication::processEvents();
    EXPECT_EQ(a->titleBarWidget(), nullptr);
    host.tabifyDockWidget(a, b);
    ASSERT_TRUE(QTest::qWaitFor([&] { return a->titleBarWidget() && b->titleBarWidget(); }));
    EXPECT_EQ(a->titleBarWidget()->height(), 0);
    const auto saved = host.saveState();
    b->hide();
    ASSERT_TRUE(QTest::qWaitFor([&] { return !a->titleBarWidget(); }));
    b->show();
    ASSERT_TRUE(QTest::qWaitFor([&] { return a->titleBarWidget(); }));
    b->setFloating(true);
    ASSERT_TRUE(QTest::qWaitFor([&] { return !a->titleBarWidget() && !b->titleBarWidget(); }));
    b->setFloating(false);
    ASSERT_TRUE(host.restoreState(saved));
    ASSERT_TRUE(QTest::qWaitFor([&] { return a->titleBarWidget() && b->titleBarWidget(); }));
    host.removeDockWidget(b);
    host.addDockWidget(Qt::RightDockWidgetArea, b);
    host.splitDockWidget(a, b, Qt::Horizontal);
    ASSERT_TRUE(QTest::qWaitFor([&] { return !a->titleBarWidget() && !b->titleBarWidget(); }));
}
}
int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    Q_INIT_RESOURCE(sentinel_svg_resources);
    QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
