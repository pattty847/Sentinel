#include "widgets/DockablePanel.hpp"
#include <QApplication>
#include <QMainWindow>
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
    QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
