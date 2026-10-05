#include <gtest/gtest.h>
#include "widgets/StockChartDock.hpp"
#include "widgets/SecFilingDock.hpp"
#include "render/CandlestickBatched.hpp"
#include "themes/ThemeManager.hpp"
#include "themes/FontManager.hpp"
#include <QSettings>
#include <QApplication>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTest>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QFile>
#include <QLabel>
#include <QQmlEngine>
#include <QJSValue>
#include <QScopeGuard>
#include <QPainter>
#include <QDir>
#include <memory>
#ifdef Q_OS_UNIX
#include <signal.h>
#endif

class FixtureProvider : public ResearchProcess {
public:
    struct Request { quint64 id; QString script; QStringList args; };
    QList<Request> requests;
    int cancellations = 0;
    void run(quint64 id, const QString& script, const QStringList& args) override { requests.append({id, script, args}); }
    void cancel() override { ++cancellations; }
    void reply(int index, const QByteArray& data, const QString& error = {}) { emit completed(requests.at(index).id, data, error); }
};
static QByteArray candles(const QString& symbol, const QString& period = "5y") {
    QJsonObject c{{"date", "2026-09-30"}, {"ts_ms", 1790726400000.0}, {"open", 10}, {"high", 12}, {"low", 9}, {"close", 11}, {"volume", 100}};
    return "OHLCV_DATA:" + QJsonDocument(QJsonObject{{"ticker", symbol}, {"period", period}, {"candles", QJsonArray{c}}}).toJson(QJsonDocument::Compact) + '\n';
}
static bool waitUntil(const std::function<bool()>& predicate) {
    QElapsedTimer timer; timer.start();
    while (!predicate() && timer.elapsed() < 3000) QTest::qWait(10);
    return predicate();
}
static int count(QObject* root) {
    const auto data = root->property("candleData");
    return (data.metaType() == QMetaType::fromType<QJSValue>() ? data.value<QJSValue>().toVariant() : data).toList().size();
}

TEST(StockResearch, ClearsOldCandlesAndRejectsOldRequestsIncludingSameTickerRetry) {
    FixtureProvider provider, sec;
    StockChartDock dock(nullptr, &provider, &sec);
    auto* root = dock.qquickView()->rootObject();
    ASSERT_NE(root, nullptr);
    dock.loadSymbol("AAPL", "Apple");
    EXPECT_TRUE(root->property("loading").toBool());
    EXPECT_EQ(root->property("asOf").toString(), "Unknown");
    provider.reply(0, candles("AAPL"));
    EXPECT_EQ(count(root), 1);
    EXPECT_EQ(root->property("dataSymbol").toString(), "AAPL");
    EXPECT_EQ(root->property("asOf").toString(), "2026-09-30");
    EXPECT_NE(root->property("retrievedAt").toString(), "Unknown");
    dock.loadSymbol("MSFT");
    EXPECT_EQ(count(root), 0);
    EXPECT_EQ(root->property("ticker").toString(), "MSFT");
    EXPECT_EQ(root->property("dataSymbol").toString(), "");
    EXPECT_TRUE(root->property("loading").toBool());
    provider.reply(0, candles("AAPL"));
    EXPECT_EQ(count(root), 0);
    EXPECT_TRUE(root->property("loading").toBool());
    dock.loadSymbol("MSFT");
    provider.reply(1, candles("MSFT"));
    EXPECT_EQ(count(root), 0);
    provider.reply(2, {}, "fixture unavailable");
    EXPECT_FALSE(root->property("loading").toBool());
    EXPECT_TRUE(root->property("statusMsg").toString().contains("fixture unavailable"));
    EXPECT_EQ(root->property("asOf").toString(), "Unknown");
}
TEST(StockResearch, RejectsWrongPayloadIdentityAndIncompletePricesAndSeparatesSecError) {
    FixtureProvider provider, sec;
    StockChartDock dock(nullptr, &provider, &sec);
    auto* root = dock.qquickView()->rootObject(); ASSERT_NE(root, nullptr);
    dock.loadSymbol("AAPL"); provider.reply(0, candles("MSFT"));
    EXPECT_EQ(count(root), 0);
    EXPECT_TRUE(root->property("statusMsg").toString().contains("match"));
    dock.loadSymbol("AAPL"); provider.reply(1, candles("AAPL", "1y"));
    EXPECT_EQ(count(root), 0);
    dock.loadSymbol("AAPL");
    auto bad = candles("AAPL"); bad.replace("\"open\":10", "\"open\":null");
    provider.reply(2, bad);
    EXPECT_EQ(count(root), 0);
    EXPECT_EQ(root->property("asOf").toString(), "Unknown");
    dock.loadSymbol("AAPL"); provider.reply(3, candles("AAPL"));
    sec.reply(3, {}, "SEC fixture down");
    EXPECT_EQ(count(root), 1);
    EXPECT_TRUE(root->property("statusMsg").toString().isEmpty());
    EXPECT_TRUE(root->property("secStatus").toString().contains("SEC fixture down"));
}
TEST(StockResearch, PeriodChangeAndTypedTickerClearAndHiddenResultsRemainCorrect) {
    FixtureProvider provider, sec;
    StockChartDock dock(nullptr, &provider, &sec);
    auto* root = dock.qquickView()->rootObject(); ASSERT_NE(root, nullptr);
    dock.loadSymbol("AAPL"); provider.reply(0, candles("AAPL"));
    QMetaObject::invokeMethod(&dock, "onPeriodChanged", Q_ARG(QString, "1y"));
    EXPECT_EQ(count(root), 0);
    provider.reply(0, candles("AAPL")); EXPECT_EQ(count(root), 0);
    provider.reply(1, candles("AAPL", "1y")); EXPECT_EQ(count(root), 1);
    auto* input = dock.findChild<QLineEdit*>("stockTicker"); ASSERT_NE(input, nullptr);
    input->setText("MSFT"); emit input->textEdited("MSFT");
    EXPECT_EQ(count(root), 0);
    EXPECT_EQ(root->property("company").toString(), "");
    provider.reply(1, candles("AAPL", "1y")); EXPECT_EQ(count(root), 0);
    dock.hide(); dock.loadSymbol("NVDA"); provider.reply(2, candles("NVDA", "1y"));
    EXPECT_EQ(root->property("dataSymbol").toString(), "NVDA");
    EXPECT_EQ(count(root), 1);
    dock.resize(480, 320); dock.show(); QCoreApplication::processEvents();
    EXPECT_LE(dock.qmlContainer()->width(), dock.width());
}
TEST(SecResearch, UnsupportedPairsNeverStartAndStaleSwitchRetryResultsAreRejected) {
    FixtureProvider provider;
    SecApiClient client(nullptr, &provider);
    QSignalSpy results(&client, &SecApiClient::filingsReady), errors(&client, &SecApiClient::apiError);
    for (const auto& symbol : {"BTC-USD", "ETH-USDT", "DOGE/USD"}) client.fetchFilings(symbol);
    EXPECT_TRUE(provider.requests.isEmpty()); EXPECT_EQ(errors.count(), 3);
    client.fetchFilings("BRK-B"); ASSERT_EQ(provider.requests.size(), 1);
    client.fetchFilings("MSFT");
    provider.reply(0, "FILINGS_DATA:[]\n"); EXPECT_EQ(results.count(), 0);
    client.fetchFilings("MSFT");
    provider.reply(1, "FILINGS_DATA:[]\n"); EXPECT_EQ(results.count(), 0);
    provider.reply(2, "FILINGS_DATA:[]\n"); EXPECT_EQ(results.count(), 1);
    EXPECT_TRUE(client.retrievedAt().isValid());
    client.cancel(); provider.reply(2, "FILINGS_DATA:[]\n"); EXPECT_EQ(results.count(), 1);
}
TEST(SecResearch, SwitchClearsResultsAndNumericCellsAreReadOnlyWithUnknownMissingValues) {
    FixtureProvider provider;
    SecFilingDock dock(nullptr, &provider);
    auto* status = dock.findChild<QLabel*>("secStatus");
    auto* table = dock.findChild<QTableView*>("secTransactions");
    ASSERT_NE(status, nullptr); ASSERT_NE(table, nullptr);
    dock.onSymbolChanged("AAPL");
    QMetaObject::invokeMethod(&dock, "fetchInsiderTransactions");
    EXPECT_TRUE(status->text().contains("Loading"));
    provider.reply(0, "TRANSACTIONS_DATA:[{\"date\":\"2026-09-30\",\"filer\":\"Fixture\",\"type\":\"P\",\"shares\":0,\"price\":null}]\n");
    EXPECT_EQ(table->model()->rowCount(), 1);
    EXPECT_EQ(table->model()->index(0,3).data().toString(), "0");
    EXPECT_EQ(table->model()->index(0,4).data().toString(), "Unknown");
    EXPECT_FALSE(table->model()->flags(table->model()->index(0,3)).testFlag(Qt::ItemIsEditable));
    EXPECT_EQ(table->editTriggers(), QAbstractItemView::NoEditTriggers);
    EXPECT_TRUE(status->text().contains("AAPL · SEC EDGAR · Retrieved:"));
    EXPECT_FALSE(status->text().contains("Unknown"));
    dock.onSymbolChanged("BTC-USD");
    EXPECT_EQ(dock.findChild<QLineEdit*>("secTicker")->text(), "BTC-USD");
    EXPECT_EQ(table->model()->rowCount(), 0);
    EXPECT_TRUE(status->text().contains("unsupported"));
    EXPECT_TRUE(status->text().contains("Unknown"));
    provider.reply(0, "TRANSACTIONS_DATA:[{\"shares\":123}]\n");
    EXPECT_EQ(table->model()->rowCount(), 0);
    dock.onSymbolChanged("MSFT"); QMetaObject::invokeMethod(&dock, "fetchFilings");
    provider.reply(1, {}, "fixture offline");
    EXPECT_TRUE(status->text().contains("MSFT")); EXPECT_TRUE(status->text().contains("fixture offline"));
    EXPECT_EQ(table->model()->rowCount(), 0);
    dock.resize(340, 380); dock.show(); QCoreApplication::processEvents();
    EXPECT_LE(status->width(), dock.width());
}
TEST(ResearchProcess, CancellationDoesNotWaitAndDestructionDoesNotCallBack) {
#ifdef Q_OS_UNIX
    QTemporaryDir directory;
    QFile uv(directory.filePath("uv")); ASSERT_TRUE(uv.open(QIODevice::WriteOnly));
    uv.write("#!/bin/sh\ntrap '' TERM\nsleep 30 &\necho $! > \"$4\"\nwait\n"); uv.close();
    uv.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    const auto oldPath = qgetenv("PATH");
    const auto restorePath = qScopeGuard([&] { qputenv("PATH", oldPath); }); qputenv("PATH", directory.path().toUtf8() + ':' + oldPath);
    auto runner = std::make_unique<ResearchProcess>();
    QSignalSpy completed(runner.get(), &ResearchProcess::completed);
    runner->run(1, "fixture", {directory.filePath("child.pid")});
    QPointer<QProcess> process;
    for (auto* child : qApp->findChildren<QProcess*>()) if (child->program() == "uv") process = child;
    ASSERT_TRUE(process);
    ASSERT_TRUE(waitUntil([&] { return process && process->state() == QProcess::Running; }));
    QFile pidFile(directory.filePath("child.pid"));
    ASSERT_TRUE(waitUntil([&] { return pidFile.exists() && pidFile.size() > 0; }));
    ASSERT_TRUE(pidFile.open(QIODevice::ReadOnly));
    const auto childPid = pidFile.readAll().trimmed().toLongLong();
    ASSERT_GT(childPid, 0);
    const auto group = process->processId();
    QElapsedTimer elapsed; elapsed.start(); runner->cancel();
    EXPECT_LT(elapsed.elapsed(), 100);
    EXPECT_TRUE(waitUntil([&] { return !process; }));
    EXPECT_TRUE(waitUntil([&] { return ::kill(pid_t(childPid), 0) != 0; })) << "Python fixture child remained alive";
    EXPECT_TRUE(waitUntil([&] { return ::kill(-pid_t(group), 0) != 0; })) << "uv/Python process group remained alive";
    runner->run(2, "fixture", {directory.filePath("second-child.pid")}); QTest::qWait(40);
    elapsed.restart(); runner.reset(); EXPECT_LT(elapsed.elapsed(), 100);
    QTest::qWait(40); EXPECT_EQ(completed.count(), 0);
    qputenv("PATH", oldPath);
#endif
}
TEST(ResearchProcess, LauncherExitStillReapsItsChild) {
#ifdef Q_OS_UNIX
    QTemporaryDir directory;
    QFile uv(directory.filePath("uv")); ASSERT_TRUE(uv.open(QIODevice::WriteOnly));
    uv.write("#!/bin/sh\nsleep 30 &\necho $! > \"$4\"\nexit 0\n"); uv.close();
    uv.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    const auto oldPath = qgetenv("PATH");
    const auto restorePath = qScopeGuard([&] { qputenv("PATH", oldPath); });
    qputenv("PATH", directory.path().toUtf8() + ':' + oldPath);
    ResearchProcess runner;
    QSignalSpy completed(&runner, &ResearchProcess::completed);
    runner.run(1, "fixture", {directory.filePath("child.pid")});
    ASSERT_TRUE(waitUntil([&] { return completed.count() == 1; }));
    QFile pidFile(directory.filePath("child.pid")); ASSERT_TRUE(pidFile.open(QIODevice::ReadOnly));
    const auto pid = pidFile.readAll().trimmed().toLongLong(); ASSERT_GT(pid, 0);
    EXPECT_TRUE(waitUntil([&] { return ::kill(pid_t(pid), 0) != 0; })) << "Child survived its uv wrapper";
#endif
}
TEST(ResearchProcess, FailureEmitsOnceAndReentrantReplacementSurvives) {
#ifdef Q_OS_UNIX
    QTemporaryDir directory;
    QFile uv(directory.filePath("uv")); ASSERT_TRUE(uv.open(QIODevice::WriteOnly));
    uv.write("#!/bin/sh\nprintf 'fixture success\\n'\n"); uv.close();
    uv.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    const auto oldPath = qgetenv("PATH");
    const auto restorePath = qScopeGuard([&] { qputenv("PATH", oldPath); });
    // A missing executable causes errorOccurred without finished.
    qputenv("PATH", "/nonexistent-research-provider");
    ResearchProcess runner;
    QSignalSpy completed(&runner, &ResearchProcess::completed);
    QObject::connect(&runner, &ResearchProcess::completed, &runner,
        [&](quint64 id, const QByteArray&, const QString&) {
            if (id == 1) {
                qputenv("PATH", directory.path().toUtf8() + ':' + oldPath);
                runner.run(2, "fixture", {});
            }
        });
    runner.run(1, "fixture", {});
    EXPECT_TRUE(waitUntil([&] { return completed.count() >= 2; }));
    QTest::qWait(30);
    ASSERT_EQ(completed.count(), 2);
    EXPECT_EQ(completed[0][0].toULongLong(), 1);
    EXPECT_FALSE(completed[0][2].toString().isEmpty());
    EXPECT_EQ(completed[1][0].toULongLong(), 2);
    EXPECT_TRUE(completed[1][2].toString().isEmpty());
    EXPECT_TRUE(completed[1][1].toByteArray().contains("fixture success"));
    qputenv("PATH", oldPath);
#endif
}
TEST(ResearchProcess, OversizedFinalOutputIsAnErrorOnlyOnce) {
#ifdef Q_OS_UNIX
    QTemporaryDir directory;
    QFile uv(directory.filePath("uv")); ASSERT_TRUE(uv.open(QIODevice::WriteOnly));
    uv.write("#!/bin/sh\nhead -c 8388609 /dev/zero\n"); uv.close();
    uv.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    const auto oldPath = qgetenv("PATH");
    const auto restorePath = qScopeGuard([&] { qputenv("PATH", oldPath); }); qputenv("PATH", directory.path().toUtf8() + ':' + oldPath);
    ResearchProcess runner;
    QSignalSpy completed(&runner, &ResearchProcess::completed);
    runner.run(1, "fixture", {});
    EXPECT_TRUE(waitUntil([&] { return completed.count() > 0; }));
    QTest::qWait(30);
    ASSERT_EQ(completed.count(), 1);
    EXPECT_TRUE(completed[0][2].toString().contains("exceeded"));
    EXPECT_TRUE(completed[0][1].toByteArray().isEmpty());
    qputenv("PATH", oldPath);
#endif
}
TEST(ResearchUi, VisualFixtures) {
    const QString output = qEnvironmentVariable("SENTINEL_RESEARCH_SHOTS");
    if (output.isEmpty()) GTEST_SKIP() << "Opt-in fixture screenshots require an authorized GUI host slot";
    ASSERT_TRUE(QDir().mkpath(output));
    FixtureProvider candlesProvider, secProvider, filingProvider;
    StockChartDock stock(nullptr, &candlesProvider, &secProvider);
    auto saveStock = [&](const QString& name) {
        QTest::qWait(150);
        auto pixmap = stock.grab();
        const auto chart = stock.qquickView()->grabWindow();
        if (chart.isNull()) return false;
        QPainter painter(&pixmap);
        painter.drawImage(QRect(stock.qmlContainer()->mapTo(&stock, QPoint()), stock.qmlContainer()->size()), chart);
        painter.end();
        return pixmap.save(QDir(output).filePath(name + ".png"));
    };
    stock.resize(900, 520); stock.show(); stock.loadSymbol("AAPL", "Fixture · Apple");
    candlesProvider.reply(0, candles("AAPL"));
    secProvider.reply(0, "INSIDER_SIGNALS_DATA:{\"symbol\":\"AAPL\",\"daily_aggregates\":[]}\n");
    ASSERT_TRUE(saveStock("stock-populated-fixture"));
    stock.resize(480, 360); stock.loadSymbol("MSFT", "Fixture · Microsoft");
    ASSERT_TRUE(saveStock("stock-switch-loading-narrow-fixture"));
    candlesProvider.reply(1, {}, "Fixture: provider unavailable");
    secProvider.reply(1, {}, "Fixture: SEC unavailable");
    ASSERT_TRUE(saveStock("stock-error-narrow-fixture"));
    stock.hide();
    SecFilingDock sec(nullptr, &filingProvider);
    sec.resize(500, 620); sec.show();
    sec.onSymbolChanged("AAPL"); QMetaObject::invokeMethod(&sec, "fetchInsiderTransactions");
    filingProvider.reply(0, "TRANSACTIONS_DATA:[{\"date\":\"2026-09-30\",\"filer\":\"Fixture director\",\"type\":\"P\",\"shares\":1250,\"price\":null}]\n");
    QTest::qWait(100);
    ASSERT_TRUE(sec.grab().save(QDir(output).filePath("sec-populated-fixture.png")));
    sec.resize(340, 480); sec.onSymbolChanged("BTC-USD"); QTest::qWait(100);
    ASSERT_TRUE(sec.grab().save(QDir(output).filePath("sec-crypto-unavailable-narrow-fixture.png")));
}
int main(int argc, char** argv) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    if (qgetenv("QT_QPA_PLATFORM") == "offscreen") QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    QApplication app(argc, argv);
    QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    Q_INIT_RESOURCE(sentinel_svg_resources);
    Q_INIT_RESOURCE(sentinel_ui_fonts);
    ThemeManager::instance().initializeDefaults();
    ThemeManager::instance().applyTheme("dark", &app);
    FontManager::instance().initialize(&app);
    qmlRegisterType<CandlestickBatched>("Sentinel.Charts", 1, 0, "CandlestickBatched");
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
