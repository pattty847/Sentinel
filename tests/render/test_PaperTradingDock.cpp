#include "widgets/PaperTradingDock.hpp"
#include "render/PaperTradeOverlayModel.hpp"
#include "../core/trading/LiveTradingSession.hpp"
#include "datasources/IGridDataSource.hpp"
#include "models/PriceUnits.hpp"
#include <QApplication>
#include <QDoubleSpinBox>
#include <QFocusEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QSettings>
#include <QComboBox>
#include <QTabWidget>
#include <QDateTime>
#include <QDateEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTest>
#include <QTimer>
#include <QTemporaryDir>
#include <QPlainTextEdit>
#include <QToolButton>
#include <cmath>
#include <gtest/gtest.h>

namespace {
class Source final : public IGridDataSource {
public:
    bool connected = true;
    std::vector<trading::TradeCommand> trades;
    std::vector<std::string> algoActions;
    std::vector<std::string> algoCommandIds;
    std::vector<std::string> algoSymbols;
    std::vector<std::string> sent; // "algo:<action>" and "trade:<action>" in send order
    LiveOrderBook book;
    void subscribe(const QString&) override {}
    void unsubscribe(const QString&) override {}
    void requestTpoHistory(const QString&, int64_t, int, int64_t, int, const QString&) override {}
    void cancelTpoHistory(const QString&, const QString&) override {}
    void setCandleHistoryViewport(const QString&, int64_t, qint64, qint64) override {}
    void sendTradeCommand(const trading::TradeCommand& cmd) override {
        trades.push_back(cmd);
        sent.push_back(std::string("trade:") + trading::toString(cmd.action) + ":" + cmd.symbol);
    }
    void sendAlgoCommand(const std::string&, const std::string& action, const std::string& symbol,
                         const trading::AlgoParams&, const std::string& commandId) override {
        algoActions.push_back(action);
        algoSymbols.push_back(symbol);
        algoCommandIds.push_back(commandId);
        sent.push_back("algo:" + action);
    }
    bool isConnectionActive() const override { return connected; }
    const LiveOrderBook& getDirectLiveOrderBook(const std::string&) const override { return book; }
};

void setOnAlgoStop(PaperTradingDock& dock, const char* value) {
    auto* combo = dock.findChild<QComboBox*>("paperOnAlgoStop");
    ASSERT_NE(combo, nullptr);
    combo->setCurrentIndex(combo->findData(QString::fromLatin1(value)));
    ASSERT_EQ(combo->currentData().toString(), QString::fromLatin1(value));
}

Trade tick(const char* symbol, double price) {
    return {std::chrono::system_clock::now(), symbol, "1", AggressorSide::Buy, price, 1.0};
}

TEST(PaperTicket, EditedLimitSurvivesTradesFocusQuantityAndButtons) {
    Source source;
    PaperTradingDock dock;
    dock.setDataSource(&source);
    dock.setSymbol("DOGE-USD");
    auto* price = dock.findChild<QDoubleSpinBox*>("paperLimitPrice");
    auto* qty = dock.findChild<QDoubleSpinBox*>("paperBaseQty");
    ASSERT_NE(price, nullptr);
    dock.onTradeReceived(tick("DOGE-USD", 0.25));
    price->setValue(0.12345678);
    qty->setFocus();
    qty->setValue(3.5);
    for (int i = 0; i < 100; ++i) dock.onTradeReceived(tick("DOGE-USD", 0.3 + i * 0.001));
    EXPECT_DOUBLE_EQ(price->value(), 0.12345678);
    ASSERT_TRUE(QMetaObject::invokeMethod(&dock, "onBuyLimitClicked"));
    ASSERT_TRUE(QMetaObject::invokeMethod(&dock, "onSellMarketClicked"));
    ASSERT_EQ(source.trades.size(), 2u);
    EXPECT_DOUBLE_EQ(source.trades[0].price, 0.12345678);
    EXPECT_DOUBLE_EQ(source.trades[0].qty, 3.5);
    EXPECT_DOUBLE_EQ(price->value(), 0.12345678);
    dock.findChild<QPushButton*>("paperUseLast")->click();
    EXPECT_DOUBLE_EQ(price->value(), 0.399);
    dock.setSymbol("PEPE-USD");
    EXPECT_DOUBLE_EQ(price->value(), 0.0);
    ASSERT_TRUE(QMetaObject::invokeMethod(&dock, "onBuyLimitClicked"));
    ASSERT_TRUE(QMetaObject::invokeMethod(&dock, "onSellLimitClicked"));
    EXPECT_EQ(source.trades.size(), 2u) << "a cleared limit must not send the previous symbol's price";
    dock.onTradeReceived(tick("PEPE-USD", 0.00000971));
    EXPECT_DOUBLE_EQ(price->value(), 0.00000971);
    ASSERT_TRUE(QMetaObject::invokeMethod(&dock, "onBuyLimitClicked"));
    ASSERT_EQ(source.trades.size(), 3u);
    EXPECT_EQ(source.trades.back().symbol, "PEPE-USD");
    EXPECT_DOUBLE_EQ(source.trades.back().price, 0.00000971);
}

TEST(PaperTicket, LimitButtonsFollowDisplayedPrice) {
    PaperTradingDock dock;
    auto* price = dock.findChild<QDoubleSpinBox*>("paperLimitPrice");
    auto* buy = dock.findChild<QPushButton*>("paperBuyLimit");
    auto* sell = dock.findChild<QPushButton*>("paperSellLimit");
    ASSERT_NE(price, nullptr);
    ASSERT_NE(buy, nullptr);
    ASSERT_NE(sell, nullptr);
    auto expectLimitState = [&](const char* phase, bool enabled) {
        SCOPED_TRACE(phase);
        for (auto* button : {buy, sell}) {
            EXPECT_EQ(button->isEnabled(), enabled);
            if (enabled) EXPECT_TRUE(button->toolTip().isEmpty());
            else EXPECT_TRUE(button->toolTip().contains("positive limit price"));
        }
    };

    expectLimitState("startup", false); // Startup has no limit price.
    dock.setSymbol("DOGE-USD");
    expectLimitState("symbol", false);
    price->setValue(0.25);
    expectLimitState("valid", true);
    price->clear(); // The editor can be empty while its prior numeric value is retained.
    expectLimitState("clear", false);
    price->setValue(0.0);
    expectLimitState("zero", false);
    price->setValue(0.125);
    expectLimitState("valid again", true);
    dock.setSymbol("PEPE-USD"); // Reset uses QSignalBlocker.
    expectLimitState("reset", false);
    dock.onTradeReceived(tick("PEPE-USD", 0.00000971)); // Initialization also blocks signals.
    expectLimitState("trade", true);
    price->clear();
    expectLimitState("clear trade price", false);
    dock.findChild<QPushButton*>("paperUseLast")->click();
    expectLimitState("use last after clear", true);
    price->setValue(0.0);
    expectLimitState("clear for last", false);
    dock.findChild<QPushButton*>("paperUseLast")->click();
    expectLimitState("use last", true);
    dock.setSymbol("DOGE-USD");
    dock.onTradeReceived(tick("DOGE-USD", 0.123456789123));
    EXPECT_DOUBLE_EQ(price->value(), 0.12345679);
    price->clear();
    expectLimitState("clear rounded trade price", false);
    dock.findChild<QPushButton*>("paperUseLast")->click();
    expectLimitState("use rounded last after clear", true);
}

TEST(PaperTicket, LimitEditorFocusOutReconcilesButtons) {
    PaperTradingDock dock;
    dock.setSymbol("DOGE-USD");
    dock.show();
    auto* price = dock.findChild<QDoubleSpinBox*>("paperLimitPrice");
    auto* qty = dock.findChild<QDoubleSpinBox*>("paperBaseQty");
    auto* buy = dock.findChild<QPushButton*>("paperBuyLimit");
    ASSERT_NE(price, nullptr);
    ASSERT_NE(qty, nullptr);
    ASSERT_NE(buy, nullptr);
    auto* editor = price->findChild<QLineEdit*>();
    ASSERT_NE(editor, nullptr);
    price->setValue(0.25);
    ASSERT_TRUE(buy->isEnabled());
    editor->setFocus();
    QSignalSpy finished(price, &QAbstractSpinBox::editingFinished);
    editor->selectAll();
    QTest::keyClick(editor, Qt::Key_Backspace);
    EXPECT_TRUE(editor->text().isEmpty());
    EXPECT_FALSE(buy->isEnabled());
    EXPECT_DOUBLE_EQ(price->value(), 0.25);
    {
        QSignalBlocker blocker(editor);
        editor->setText(price->locale().toString(price->value(), 'f', price->decimals()));
    }
    EXPECT_FALSE(buy->isEnabled()); // A blocked editor update needs the focus-out refresh.
    QFocusEvent focusOut(QEvent::FocusOut, Qt::OtherFocusReason);
    QCoreApplication::sendEvent(price, &focusOut);
    qty->setFocus();
    QCoreApplication::processEvents();
    EXPECT_GE(finished.count(), 1);
    bool displayedValid = false;
    const double displayed = price->locale().toDouble(price->cleanText(), &displayedValid);
    EXPECT_EQ(buy->isEnabled(), displayedValid && std::isfinite(displayed) && displayed > 0.0);
    EXPECT_TRUE(buy->isEnabled());
}

TEST(PaperTicket, LowPriceAndLogFollow) {
    PaperTradingDock dock;
    dock.setSymbol("PEPE-USD");
    auto* price = dock.findChild<QDoubleSpinBox*>("paperLimitPrice");
    auto* log = dock.findChild<QTableWidget*>("paperOrderLog");
    dock.onTradeReceived(tick("PEPE-USD", 0.00000971));
    ASSERT_NE(price, nullptr);
    EXPECT_DOUBLE_EQ(price->value(), 0.00000971);
    EXPECT_GE(price->decimals(), 8);
    ASSERT_NE(log, nullptr);
    EXPECT_EQ(log->editTriggers(), QAbstractItemView::NoEditTriggers);
    dock.resize(500, 500);
    dock.show();
    for (int i = 0; i < 40; ++i) {
        trading::OrderUpdate u;
        u.symbol = "PEPE-USD";
        u.orderId = std::to_string(i);
        u.qty = 0.00000001;
        u.limitPrice = 0.00000971;
        dock.onOrderUpdated(u);
    }
    auto* scroll = log->verticalScrollBar();
    ASSERT_GT(scroll->maximum(), 0);
    EXPECT_EQ(scroll->value(), scroll->maximum());
    scroll->setValue(0);
    trading::OrderUpdate u;
    u.symbol = "PEPE-USD";
    u.orderId = "new";
    dock.onOrderUpdated(u);
    EXPECT_EQ(scroll->value(), 0);
    dock.setSymbol("FARTCOIN-USD");
    EXPECT_EQ(log->rowCount(), 0);
    bool awaitingPosition = false;
    for (auto* label : dock.findChildren<QLabel*>())
        awaitingPosition |= label->text() == QStringLiteral("Awaiting position update");
    EXPECT_TRUE(awaitingPosition);
    auto buttons = dock.findChildren<QToolButton*>();
    QToolButton* one = nullptr;
    QToolButton* five = nullptr;
    for (auto* button : buttons) {
        if (button->text() == "1m") one = button;
        if (button->text() == "5m") five = button;
    }
    ASSERT_NE(one, nullptr);
    ASSERT_NE(five, nullptr);
    one->click();
    EXPECT_TRUE(one->isChecked());
    five->click();
    EXPECT_FALSE(one->isChecked());
    EXPECT_TRUE(five->isChecked());
}

TEST(PaperTicket, SubmittedPriceUsesDisplayedPrecision) {
    Source source;
    PaperTradingDock dock;
    dock.setDataSource(&source);
    dock.setSymbol("BTC-USD");
    auto* price = dock.findChild<QDoubleSpinBox*>("paperLimitPrice");
    ASSERT_NE(price, nullptr);
    price->setValue(84000.123);
    ASSERT_TRUE(QMetaObject::invokeMethod(&dock, "onBuyLimitClicked"));
    ASSERT_EQ(source.trades.size(), 1u);
    EXPECT_NEAR(source.trades.back().price, 84000.123, 0.00000001);
    dock.setSymbol("PEPE-USD");
    price->setValue(0.00000971);
    ASSERT_TRUE(QMetaObject::invokeMethod(&dock, "onSellLimitClicked"));
    ASSERT_EQ(source.trades.size(), 2u);
    EXPECT_NEAR(source.trades.back().price, 0.00000971, 0.000000001);
}

TEST(PaperTicket, AlgoActivityIsEvidenceButNoAckTimesOut) {
    Source source;
    PaperTradingDock dock;
    setOnAlgoStop(dock, "never");
    dock.setDataSource(&source);
    dock.setSymbol("DOGE-USD");
    auto* status = dock.findChild<QLabel*>("paperAlgoStatus");
    auto* timer = dock.findChild<QTimer*>("paperAlgoTimeout");
    ASSERT_NE(status, nullptr);
    ASSERT_NE(timer, nullptr);
    dock.findChild<QPushButton*>("paperAlgoStart")->click();
    EXPECT_EQ(source.algoActions.back(), "start");
    EXPECT_EQ(status->text(), "START PENDING");
    trading::AlgoOrderEvent unrelated;
    unrelated.algoId = "AvendellaMM";
    unrelated.symbol = "PEPE-USD";
    dock.onAlgoOrderEvent(unrelated);
    EXPECT_EQ(status->text(), "START PENDING");
    timer->start(1);
    QTest::qWait(20);
    EXPECT_EQ(status->text(), "UNCONFIRMED");
    dock.findChild<QPushButton*>("paperAlgoStart")->click();
    trading::AlgoOrderEvent rejectedOrder;
    rejectedOrder.algoId = "AvendellaMM";
    rejectedOrder.symbol = "DOGE-USD";
    rejectedOrder.status = trading::OrderStatus::Rejected;
    dock.onAlgoOrderEvent(rejectedOrder);
    EXPECT_EQ(status->text(), "RUNNING / ACTIVITY OBSERVED");
    EXPECT_TRUE(status->toolTip().contains("rejected order"));
    dock.findChild<QPushButton*>("paperAlgoStop")->click();
    EXPECT_EQ(status->text(), "STOPPING / UNCONFIRMED");
    dock.onAlgoOrderEvent(rejectedOrder);
    EXPECT_EQ(status->text(), "STOPPING / UNCONFIRMED");
    timer->start(1);
    QTest::qWait(20);
    EXPECT_EQ(status->text(), "UNCONFIRMED");
    dock.onAlgoOrderEvent(rejectedOrder);
    EXPECT_EQ(status->text(), "UNCONFIRMED");
    dock.findChild<QPushButton*>("paperAlgoStart")->click();
    EXPECT_EQ(status->text(), "START PENDING");
    dock.onAlgoOrderEvent(rejectedOrder);
    EXPECT_EQ(status->text(), "RUNNING / ACTIVITY OBSERVED");
    source.connected = false;
    emit source.connectionStatusChanged(false);
    QTest::qWait(10);
    EXPECT_EQ(status->text(), "UNAVAILABLE");
    dock.setSymbol("PEPE-USD");
    EXPECT_EQ(status->text(), "UNAVAILABLE");
}

TEST(PaperTicket, AttachingToDisconnectedSourceStartsUnavailable) {
    Source source;
    source.connected = false;
    PaperTradingDock dock;
    dock.setSymbol("DOGE-USD");
    dock.setDataSource(&source);
    auto* status = dock.findChild<QLabel*>("paperAlgoStatus");
    auto* start = dock.findChild<QPushButton*>("paperAlgoStart");
    ASSERT_NE(status, nullptr);
    ASSERT_NE(start, nullptr);
    EXPECT_EQ(status->text(), "UNAVAILABLE");
    EXPECT_FALSE(start->isEnabled());
    source.connected = true;
    emit source.connectionStatusChanged(true);
    QTest::qWait(10);
    EXPECT_EQ(status->text(), "UNCONFIRMED");
    EXPECT_TRUE(start->isEnabled());
}
TEST(PaperTicket, BacktestSourceDefaultsToJournalAndKeepsFileError) {
    PaperTradingDock dock;
    auto* tabs = dock.findChild<QTabWidget*>();
    ASSERT_NE(tabs, nullptr);
    tabs->setCurrentIndex(2);
    dock.show();
    auto* source = dock.findChild<QComboBox*>("backtestSource");
    auto* from = dock.findChild<QDateEdit*>("backtestFrom");
    auto* to = dock.findChild<QDateEdit*>("backtestTo");
    auto* root = dock.findChild<QLineEdit*>("backtestJournalRoot");
    ASSERT_NE(source, nullptr); ASSERT_NE(from, nullptr); ASSERT_NE(to, nullptr); ASSERT_NE(root, nullptr);
    EXPECT_EQ(source->currentText(), "Journal");
    EXPECT_EQ(from->date(), QDateTime::currentDateTimeUtc().date().addDays(-1));
    EXPECT_EQ(to->date(), QDateTime::currentDateTimeUtc().date());
    EXPECT_FALSE(root->isHidden());
    QPushButton* run = nullptr;
    for (auto* button : dock.findChildren<QPushButton*>())
        if (button->text().contains("Run Backtest")) run = button;
    ASSERT_NE(run, nullptr);
    from->setDate(to->date());
    run->click();
    bool rangeError = false;
    for (auto* label : dock.findChildren<QLabel*>()) rangeError |= label->text().contains("valid UTC range");
    EXPECT_TRUE(rangeError);
    source->setCurrentIndex(1);
    EXPECT_FALSE(root->isVisible());
    run->click();
    bool fileError = false;
    for (auto* label : dock.findChildren<QLabel*>()) fileError |= label->text().contains("no trade log");
    EXPECT_TRUE(fileError);
}

TEST(PaperTicket, R7BacktestModeStatusAndEmptyRangeError) {
    QTemporaryDir empty;
    PaperTradingDock dock;
    auto* source = dock.findChild<QComboBox*>("backtestSource");
    auto* status = dock.findChild<QLabel*>("backtestStatus");
    ASSERT_NE(source, nullptr); ASSERT_NE(status, nullptr);
    source->setCurrentIndex(1);
    EXPECT_EQ(status->text(), "Select a trade log and press Run.");
    source->setCurrentIndex(0);
    EXPECT_EQ(status->text(), "Choose a UTC range and press Run.");
    dock.findChild<QLineEdit*>("backtestJournalRoot")->setText(empty.path());
    dock.findChild<QLineEdit*>("backtestProduct")->setText("DB2A-EMPTY-USD");
    QPushButton* run = nullptr;
    for (auto* button : dock.findChildren<QPushButton*>())
        if (button->text().contains("Run Backtest")) run = button;
    ASSERT_NE(run, nullptr);
    run->click();
    for (int i = 0; i < 100 && !run->isEnabled(); ++i) QTest::qWait(50);
    EXPECT_TRUE(run->isEnabled());
    EXPECT_EQ(status->text(), "Error: no trades in range.");
    EXPECT_TRUE(dock.findChild<QPlainTextEdit*>()->toPlainText().contains("no trades in range"));
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    // The dock persists "On algo stop"; never touch the user's settings.
    QTemporaryDir settingsDir;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

// A trade, order and position can arrive before the book snapshot decides the symbol's
// display unit: the dock reformats them when the unit arrives.
TEST(PaperTicket, PricesFollowALaterDisplayUnit) {
    PriceUnits::instance().clearForTest();
    PaperTradingDock dock;
    dock.setSymbol("PEPE-USD");
    dock.onTradeReceived(tick("PEPE-USD", 0.00000504));
    trading::OrderUpdate order;
    order.symbol = "PEPE-USD";
    order.orderId = "a";
    order.limitPrice = 0.00000497;
    dock.onOrderUpdated(order);
    trading::PositionUpdate position;
    position.symbol = "PEPE-USD";
    position.avgPrice = 0.000005;
    dock.onPositionUpdated(position);
    auto* last = dock.findChild<QLabel*>("paperLastPrice");
    auto* avg = dock.findChild<QLabel*>("paperAvgPrice");
    auto* log = dock.findChild<QTableWidget*>("paperOrderLog");
    ASSERT_TRUE(last && avg && log);
    ASSERT_EQ(log->rowCount(), 1);
    EXPECT_TRUE(last->text().startsWith('$'));
    PriceUnits::instance().observe("PEPE-USD", 0.00000504);
    EXPECT_EQ(last->text(), QStringLiteral("5.04 x1M"));
    EXPECT_EQ(avg->text(), QStringLiteral("5.00 x1M"));
    EXPECT_EQ(log->item(0, 4)->text(), QStringLiteral("4.97 x1M"));
    PriceUnits::instance().clearForTest();
}

namespace {
trading::OrderUpdate order(const char* id, trading::OrderStatus status, const char* algo = "") {
    trading::OrderUpdate u;
    u.orderId = id;
    u.symbol = "BTC-USD";
    u.side = trading::OrderSide::Buy;
    u.qty = 1.0;
    u.limitPrice = 90.0;
    u.status = status;
    u.algoId = algo;
    return u;
}

trading::TradingSnapshot snapshot(const char* epoch, const char* reason = "connect", std::string commandId = {}) {
    trading::TradingSnapshot s;
    s.epoch = epoch;
    s.reason = reason;
    s.commandId = std::move(commandId);
    return s;
}

QStringList rows(QTableWidget* log) {
    QStringList out;
    for (int r = 0; r < log->rowCount(); ++r) out << log->item(r, 0)->text() + ":" + log->item(r, 5)->text();
    return out;
}

QMessageBox* flattenPrompt() {
    for (auto* w : QApplication::topLevelWidgets()) {
        auto* box = qobject_cast<QMessageBox*>(w);
        if (box && box->objectName() == "paperFlattenPrompt" && box->isVisible()) return box;
    }
    return nullptr;
}

// A dock showing BTC-USD with AvendellaMM running there.
struct StopRig {
    Source source;
    PaperTradingDock dock;
    explicit StopRig(const char* policy, std::vector<trading::PositionUpdate> positions = {}) {
        setOnAlgoStop(dock, policy);
        dock.setDataSource(&source);
        dock.setSymbol("BTC-USD");
        auto state = snapshot("e1");
        state.runningAlgos = {{"AvendellaMM", "BTC-USD"}};
        state.positions = std::move(positions);
        dock.onTradingSnapshot(state);
    }
    void stop() { ASSERT_TRUE(QMetaObject::invokeMethod(&dock, "onStopAlgoClicked")); }
    // The server's acknowledgement of the last stop, after it was processed.
    void ack(std::vector<trading::PositionUpdate> positions, const char* epoch = "e1") {
        auto a = snapshot(epoch, "algo_command", source.algoCommandIds.back());
        a.positions = std::move(positions);
        emit source.tradingSnapshotReceived(a);
        QTest::qWait(10);
    }
    void reconnect() {
        emit source.connectionStatusChanged(false);
        emit source.connectionStatusChanged(true);
        QTest::qWait(10);
    }
};
} // namespace

// The server restarted (or this client missed updates): the snapshot is the truth.
TEST(PaperSnapshot, ReplacesStalePositionAndDropsGhostOrders) {
    Source source;
    PaperTradingDock dock;
    dock.setDataSource(&source);
    dock.setSymbol("BTC-USD");
    auto* log = dock.findChild<QTableWidget*>("paperOrderLog");
    ASSERT_TRUE(log);
    auto first = snapshot("e1");
    first.openOrders = {order("ord-1", trading::OrderStatus::Open)};
    first.positions = {{"BTC-USD", 2.0, 100.0, 0.0, 0.0}};
    dock.onTradingSnapshot(first);
    dock.onOrderUpdated(order("ord-2", trading::OrderStatus::Open));
    dock.onOrderUpdated(order("ord-3", trading::OrderStatus::Filled));
    ASSERT_EQ(rows(log), (QStringList{"ord-1:OPEN", "ord-2:OPEN", "ord-3:FILLED"}));

    auto same = snapshot("e1");
    same.openOrders = {order("ord-1", trading::OrderStatus::Open)};
    dock.onTradingSnapshot(same);
    EXPECT_EQ(rows(log), (QStringList{"ord-1:OPEN", "ord-3:FILLED"}))
        << "a working order the server no longer holds is a ghost; history stays";
    bool flat = false;
    for (auto* label : dock.findChildren<QLabel*>()) flat |= label->text() == QStringLiteral("0.00000000");
    EXPECT_TRUE(flat) << "a symbol missing from the snapshot is flat";

    // ord-3 is updated by the server after the drop: the row index map must still be right.
    dock.onOrderUpdated(order("ord-1", trading::OrderStatus::Canceled));
    EXPECT_EQ(rows(log), (QStringList{"ord-1:CANCELED", "ord-3:FILLED"}));

    auto restarted = snapshot("e2");
    restarted.openOrders = {order("ord-1", trading::OrderStatus::Open)};
    dock.onTradingSnapshot(restarted);
    EXPECT_EQ(rows(log), (QStringList{"ord-1:OPEN"})) << "a new server session's ids restart; old rows go";
}

// Item: "algo panel stuck UNCONFIRMED". Without a correlated acknowledgement the stop ends
// at UNCONFIRMED after the timeout; the server's algo_command snapshot settles it.
TEST(PaperSnapshot, AlgoCommandsAreAcknowledgedBySnapshot) {
    Source source;
    PaperTradingDock dock;
    setOnAlgoStop(dock, "never");
    dock.setDataSource(&source);
    dock.setSymbol("BTC-USD");
    auto* status = dock.findChild<QLabel*>("paperAlgoStatus");
    auto* start = dock.findChild<QPushButton*>("paperAlgoStart");
    auto* stop = dock.findChild<QPushButton*>("paperAlgoStop");
    ASSERT_TRUE(status && start && stop);

    start->click();
    ASSERT_EQ(source.algoCommandIds.size(), 1u);
    ASSERT_FALSE(source.algoCommandIds[0].empty());
    auto other = snapshot("e1", "algo_command", "someone-else");
    emit source.tradingSnapshotReceived(other);
    QTest::qWait(10);
    EXPECT_EQ(status->text(), "START PENDING") << "another client's acknowledgement is not ours";
    auto started = snapshot("e1", "algo_command", source.algoCommandIds[0]);
    started.runningAlgos = {{"AvendellaMM", "BTC-USD"}};
    emit source.tradingSnapshotReceived(started);
    QTest::qWait(10);
    EXPECT_EQ(status->text(), "RUNNING");
    EXPECT_TRUE(stop->isEnabled());
    EXPECT_FALSE(start->isEnabled());

    stop->click();
    ASSERT_EQ(source.algoCommandIds.size(), 2u);
    EXPECT_EQ(status->text(), "STOPPING / UNCONFIRMED");
    emit source.tradingSnapshotReceived(snapshot("e1", "algo_command", source.algoCommandIds[1]));
    QTest::qWait(10);
    EXPECT_EQ(status->text(), "STOPPED");
    EXPECT_TRUE(start->isEnabled());
    EXPECT_FALSE(stop->isEnabled());

    // A refused start (nothing running in the acknowledgement) is reported, not left pending.
    start->click();
    emit source.tradingSnapshotReceived(snapshot("e1", "algo_command", source.algoCommandIds[2]));
    QTest::qWait(10);
    EXPECT_EQ(status->text(), "STOPPED");
    EXPECT_TRUE(status->toolTip().contains("did not start"));
}

TEST(PaperSnapshot, ReconnectSnapshotRestoresAlgoState) {
    Source source;
    PaperTradingDock dock;
    dock.setDataSource(&source);
    dock.setSymbol("BTC-USD");
    auto* status = dock.findChild<QLabel*>("paperAlgoStatus");
    emit source.connectionStatusChanged(false);
    emit source.connectionStatusChanged(true);
    auto running = snapshot("e1");
    running.runningAlgos = {{"AvendellaMM", "BTC-USD"}};
    emit source.tradingSnapshotReceived(running);
    QTest::qWait(10);
    EXPECT_EQ(status->text(), "RUNNING") << "the reconnect reset must not overwrite the snapshot after it";
    emit source.connectionStatusChanged(true);
    emit source.tradingSnapshotReceived(snapshot("e2"));
    QTest::qWait(10);
    EXPECT_EQ(status->text(), "STOPPED") << "a restarted server runs no algo";
}

TEST(PaperOnAlgoStop, SettingPersistsAndDefaultsToAsk) {
    {
        QSettings settings(QSettings::defaultFormat(), QSettings::UserScope, "Sentinel", "SentinelTerminal");
        settings.remove("PaperTradingDock/onAlgoStop");
    }
    {
        PaperTradingDock dock;
        auto* combo = dock.findChild<QComboBox*>("paperOnAlgoStop");
        ASSERT_NE(combo, nullptr);
        EXPECT_EQ(combo->currentText(), "Ask");
        EXPECT_EQ(combo->count(), 3);
        setOnAlgoStop(dock, "always");
    }
    QSettings settings(QSettings::defaultFormat(), QSettings::UserScope, "Sentinel", "SentinelTerminal");
    EXPECT_EQ(settings.value("PaperTradingDock/onAlgoStop").toString(), "always");
    PaperTradingDock reopened;
    EXPECT_EQ(reopened.findChild<QComboBox*>("paperOnAlgoStop")->currentText(), "Always");
}

// The connect snapshot lands before the GUI selects a symbol; every switch keeps the account.
TEST(PaperAccount, SnapshotBeforeFirstSymbolAndSwitchAwayAndBack) {
    Source source;
    PaperTradingDock dock;
    dock.setDataSource(&source);
    auto first = snapshot("e1");
    first.openOrders = {order("ord-1", trading::OrderStatus::Open, "AvendellaMM")};
    first.positions = {{"BTC-USD", 1.0, 100.0, 0.0, 0.0}};
    first.runningAlgos = {{"AvendellaMM", "BTC-USD"}};
    dock.onTradingSnapshot(first);
    dock.setSymbol("BTC-USD");
    auto* status = dock.findChild<QLabel*>("paperAlgoStatus");
    auto* stop = dock.findChild<QPushButton*>("paperAlgoStop");
    auto* log = dock.findChild<QTableWidget*>("paperOrderLog");
    auto* avg = dock.findChild<QLabel*>("paperAvgPrice");
    EXPECT_EQ(status->text(), "RUNNING");
    EXPECT_TRUE(stop->isEnabled());
    EXPECT_EQ(rows(log), (QStringList{"ord-1:OPEN"}));
    EXPECT_EQ(avg->text(), QStringLiteral("$100.00"));

    dock.setSymbol("ETH-USD");
    EXPECT_EQ(status->text(), "RUNNING ON BTC-USD");
    EXPECT_TRUE(stop->isEnabled()) << "the algo can be stopped from any symbol";
    EXPECT_TRUE(rows(log).isEmpty());
    // BTC changes while ETH is on screen.
    dock.onOrderUpdated(order("ord-1", trading::OrderStatus::Filled, "AvendellaMM"));
    dock.onOrderUpdated(order("ord-2", trading::OrderStatus::Open, "AvendellaMM"));
    dock.onPositionUpdated({"BTC-USD", 2.0, 95.0, 0.0, 0.0});

    dock.setSymbol("BTC-USD");
    EXPECT_EQ(status->text(), "RUNNING");
    EXPECT_EQ(rows(log), (QStringList{"ord-2:OPEN"}));
    EXPECT_EQ(avg->text(), QStringLiteral("$95.00"));
    // Activity on the running symbol is still evidence after the switches.
    trading::AlgoOrderEvent activity;
    activity.algoId = "AvendellaMM";
    activity.symbol = "BTC-USD";
    dock.onAlgoOrderEvent(activity);
    EXPECT_EQ(status->text(), "RUNNING") << "a confirmed state is not downgraded by activity";
}

// Contract: the stop's acknowledgement decides the targets; Ask prompts, Yes flattens.
TEST(PaperOnAlgoStop, AskPromptsOnTheAckAndFlattensOnlyOnYes) {
    for (const auto button : {QMessageBox::Yes, QMessageBox::No}) {
        StopRig rig("ask", {{"BTC-USD", 1.0, 100.0, 0.0, 0.0}});
        rig.stop();
        EXPECT_EQ(flattenPrompt(), nullptr) << "nothing is decided at click time";
        rig.ack({{"BTC-USD", 1.0, 100.0, 0.0, 0.0}});
        auto* box = flattenPrompt();
        ASSERT_NE(box, nullptr);
        EXPECT_EQ(box->text(), QStringLiteral("Flatten all? BTC-USD"));
        box->button(button)->click();
        QTest::qWait(10);
        if (button == QMessageBox::Yes)
            EXPECT_EQ(rig.source.sent, (std::vector<std::string>{"algo:stop", "trade:FLATTEN:BTC-USD"}));
        else
            EXPECT_EQ(rig.source.sent, (std::vector<std::string>{"algo:stop"}));
    }
}

TEST(PaperOnAlgoStop, NeverKeepsAndAnAckWithNoPositionDoesNothing) {
    StopRig never("never", {{"BTC-USD", 1.0, 100.0, 0.0, 0.0}});
    never.stop();
    never.ack({{"BTC-USD", 1.0, 100.0, 0.0, 0.0}});
    EXPECT_EQ(flattenPrompt(), nullptr);
    EXPECT_EQ(never.source.sent, (std::vector<std::string>{"algo:stop"}));
    StopRig flat("ask", {{"BTC-USD", 1.0, 100.0, 0.0, 0.0}});
    flat.stop();
    flat.ack({{"BTC-USD", 0.0, 0.0, 0.0, 4.0}});
    EXPECT_EQ(flattenPrompt(), nullptr) << "no open position, no prompt";
    EXPECT_EQ(flat.source.sent, (std::vector<std::string>{"algo:stop"}));
}

// Stop names the running algo's symbol; the ack's every open position is flattened.
TEST(PaperAccount, StopUsesTheAlgoSymbolAndFlattensEveryPosition) {
    StopRig rig("always", {{"BTC-USD", 1.0, 100.0, 0.0, 0.0}});
    rig.dock.setSymbol("ETH-USD");
    rig.stop();
    ASSERT_EQ(rig.source.algoSymbols.size(), 1u);
    EXPECT_EQ(rig.source.algoSymbols[0], "BTC-USD") << "the stop is keyed to the algo, not the ticket";
    rig.ack({{"BTC-USD", 1.0, 100.0, 0.0, 0.0}, {"ETH-USD", -2.0, 3000.0, 0.0, 0.0}, {"SOL-USD", 0.0, 0.0, 0.0, 5.0}});
    EXPECT_EQ(rig.source.sent, (std::vector<std::string>{"algo:stop", "trade:FLATTEN:BTC-USD",
                                                         "trade:FLATTEN:ETH-USD"}));
}

// (a) A fill raced the stop: the cached account is flat, the ack holds a BTC position.
TEST(PaperStopOperation, AckTargetsIncludeAFillThatRacedTheStop) {
    StopRig always("always");
    always.stop();
    EXPECT_EQ(always.source.sent, (std::vector<std::string>{"algo:stop"}));
    always.ack({{"BTC-USD", 1.0, 100.0, 0.0, 0.0}});
    EXPECT_EQ(always.source.sent, (std::vector<std::string>{"algo:stop", "trade:FLATTEN:BTC-USD"}));

    StopRig ask("ask");
    QTimer failSafe; // a click-time modal prompt would block: answer it so the test fails, not hangs
    failSafe.setSingleShot(true);
    QObject::connect(&failSafe, &QTimer::timeout, [] { if (auto* b = flattenPrompt()) b->button(QMessageBox::No)->click(); });
    failSafe.start(1500);
    ask.stop();
    EXPECT_EQ(flattenPrompt(), nullptr);
    ask.ack({{"BTC-USD", 1.0, 100.0, 0.0, 0.0}});
    auto* box = flattenPrompt();
    ASSERT_NE(box, nullptr) << "Ask prompts with the ack's position";
    EXPECT_EQ(box->text(), QStringLiteral("Flatten all? BTC-USD"));
    box->button(QMessageBox::No)->click();
}

// (b) A reconnect while the prompt is open: Yes sends nothing.
TEST(PaperStopOperation, ReconnectWhilePromptOpenSendsNothing) {
    StopRig rig("ask", {{"BTC-USD", 1.0, 100.0, 0.0, 0.0}});
    QTimer failSafe;
    failSafe.setSingleShot(true);
    QObject::connect(&failSafe, &QTimer::timeout, [] { if (auto* b = flattenPrompt()) b->button(QMessageBox::Yes)->click(); });
    failSafe.start(1500);
    rig.stop();
    rig.ack({{"BTC-USD", 1.0, 100.0, 0.0, 0.0}});
    QPointer<QMessageBox> box = flattenPrompt();
    ASSERT_NE(box, nullptr);
    rig.reconnect();
    if (box && box->isVisible()) box->button(QMessageBox::Yes)->click();
    QTest::qWait(10);
    EXPECT_EQ(flattenPrompt(), nullptr) << "the reconnect closes the prompt";
    EXPECT_EQ(rig.source.sent, (std::vector<std::string>{"algo:stop"}));
}

// (c) The server restarted before the ack: no prompt, no flatten, even if an ack follows.
TEST(PaperStopOperation, EpochChangeBeforeAckCancels) {
    StopRig rig("always", {{"BTC-USD", 1.0, 100.0, 0.0, 0.0}});
    rig.stop();
    auto restarted = snapshot("e2");
    restarted.positions = {{"BTC-USD", 1.0, 100.0, 0.0, 0.0}};
    emit rig.source.tradingSnapshotReceived(restarted);
    QTest::qWait(10);
    rig.ack({{"BTC-USD", 1.0, 100.0, 0.0, 0.0}}, "e2");
    EXPECT_EQ(flattenPrompt(), nullptr);
    EXPECT_EQ(rig.source.sent, (std::vector<std::string>{"algo:stop"}));
}

// (d) An invalidated account never produces a flatten on its own; only the ack does.
TEST(PaperStopOperation, InvalidatedAccountFlattensOnlyOnTheAck) {
    StopRig rig("always", {{"BTC-USD", 1.0, 100.0, 0.0, 0.0}});
    rig.reconnect();
    rig.stop();
    EXPECT_EQ(rig.source.sent, (std::vector<std::string>{"algo:stop"})) << "no flatten from unknown state";
    rig.ack({{"BTC-USD", 1.0, 100.0, 0.0, 0.0}});
    EXPECT_EQ(rig.source.sent, (std::vector<std::string>{"algo:stop", "trade:FLATTEN:BTC-USD"}));
}

TEST(PaperStopOperation, NoAckTimeoutNoFlatten) {
    StopRig rig("always", {{"BTC-USD", 1.0, 100.0, 0.0, 0.0}});
    rig.stop();
    auto* timer = rig.dock.findChild<QTimer*>("paperAlgoTimeout");
    timer->start(1);
    QTest::qWait(20);
    rig.ack({{"BTC-USD", 1.0, 100.0, 0.0, 0.0}});
    EXPECT_EQ(rig.source.sent, (std::vector<std::string>{"algo:stop"}));
}

// The chart's manual orders and TP/SL come back after switching away and back.
TEST(PaperOverlay, SwitchAwayAndBackRestoresOrdersAndRisk) {
    PaperTradeOverlayModel model;
    model.setSymbol("BTC-USD");
    auto state = snapshot("e1");
    state.openOrders = {order("ord-1", trading::OrderStatus::Open), order("q-1", trading::OrderStatus::Open, "AvendellaMM")};
    state.positions = {{"BTC-USD", 1.0, 100.0, 0.0, 0.0}};
    state.riskOrders = {{"BTC-USD", true, 120.0, true, 90.0}};
    model.onTradingSnapshot(state);
    ASSERT_EQ(model.openOrders().size(), 1) << "manual orders only";
    ASSERT_TRUE(model.canShowRiskControls());
    model.setSymbol("ETH-USD");
    EXPECT_TRUE(model.openOrders().isEmpty());
    trading::RiskOrderUpdate moved{"BTC-USD", true, 125.0, true, 90.0};
    model.onRiskOrderUpdated(moved); // arrives while ETH is on screen
    model.setSymbol("BTC-USD");
    ASSERT_EQ(model.openOrders().size(), 1);
    EXPECT_EQ(model.openOrders()[0].toMap().value("orderId").toString(), "ord-1");
    EXPECT_TRUE(model.canShowRiskControls());
    const auto risk = model.riskState();
    EXPECT_TRUE(risk.value("hasTakeProfit").toBool());
    EXPECT_DOUBLE_EQ(risk.value("takeProfitPrice").toDouble(), 125.0);
    EXPECT_DOUBLE_EQ(risk.value("stopLossPrice").toDouble(), 90.0);
}

// Flatten, reopen, switch away and back: the chart must not bring back the old TP/SL.
// Driven by a real paper session, so the chart sees exactly what the server publishes.
TEST(PaperOverlay, FlattenReopenSwitchShowsNoStaleRisk) {
    trading::LiveTradingSession session([](const std::string&) { return 100.0; }, 0.0);
    PaperTradeOverlayModel model;
    model.setSymbol("BTC-USD");
    session.setResultCallback([&model](trading::TradingResult r, std::vector<trading::AlgoOrderEvent>) {
        for (const auto& u : r.orderUpdates) model.onOrderUpdated(u);
        for (const auto& u : r.positionUpdates) model.onPositionUpdated(u);
        for (const auto& u : r.riskOrderUpdates) model.onRiskOrderUpdated(u);
    });
    auto command = [](trading::TradeAction action, trading::OrderSide side = trading::OrderSide::Buy) {
        trading::TradeCommand c;
        c.action = action;
        c.symbol = "BTC-USD";
        c.side = side;
        c.orderType = trading::OrderType::Market;
        c.qty = 1.0;
        return c;
    };
    session.processTradeCommand(command(trading::TradeAction::PlaceOrder));
    auto risk = command(trading::TradeAction::SetAttachedRisk);
    risk.hasTakeProfit = true;
    risk.takeProfitPrice = 120.0;
    risk.hasStopLoss = true;
    risk.stopLossPrice = 90.0;
    session.processTradeCommand(risk);
    auto withRisk = session.snapshot();
    model.onTradingSnapshot(withRisk);
    ASSERT_TRUE(model.riskState().value("hasTakeProfit").toBool());

    session.processTradeCommand(command(trading::TradeAction::Flatten));
    session.processTradeCommand(command(trading::TradeAction::PlaceOrder)); // reopen, no TP/SL
    EXPECT_TRUE(session.snapshot().riskOrders.empty()) << "server state holds no TP/SL";
    model.setSymbol("ETH-USD");
    model.setSymbol("BTC-USD");
    ASSERT_TRUE(model.canShowRiskControls()) << "the reopened position is shown";
    const auto shown = model.riskState();
    EXPECT_FALSE(shown.value("hasTakeProfit").toBool()) << "the flattened position's TP must not return";
    EXPECT_FALSE(shown.value("hasStopLoss").toBool());
}

// An epoch change while the Ask prompt is open closes it unsent.
TEST(PaperStopOperation, EpochChangeWhilePromptOpenClosesItUnsent) {
    StopRig rig("ask", {{"BTC-USD", 1.0, 100.0, 0.0, 0.0}});
    rig.stop();
    rig.ack({{"BTC-USD", 1.0, 100.0, 0.0, 0.0}});
    QPointer<QMessageBox> box = flattenPrompt();
    ASSERT_NE(box, nullptr);
    auto restarted = snapshot("e2");
    restarted.positions = {{"BTC-USD", 1.0, 100.0, 0.0, 0.0}};
    emit rig.source.tradingSnapshotReceived(restarted);
    QTest::qWait(10);
    EXPECT_EQ(flattenPrompt(), nullptr) << "a new server session closes the prompt";
    if (box && box->isVisible()) box->button(QMessageBox::Yes)->click();
    QTest::qWait(10);
    EXPECT_EQ(rig.source.sent, (std::vector<std::string>{"algo:stop"}));
}
