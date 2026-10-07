#include "widgets/PaperTradingDock.hpp"
#include "datasources/IGridDataSource.hpp"
#include <QApplication>
#include <QDoubleSpinBox>
#include <QFocusEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <cmath>
#include <gtest/gtest.h>

namespace {
class Source final : public IGridDataSource {
public:
    bool connected = true;
    std::vector<trading::TradeCommand> trades;
    std::vector<std::string> algoActions;
    LiveOrderBook book;
    void subscribe(const QString&) override {}
    void unsubscribe(const QString&) override {}
    void requestFootprintHistory(const QString&, int64_t, int64_t, int) override {}
    void requestTpoHistory(const QString&, int64_t, int, int64_t, int, const QString&) override {}
    void cancelTpoHistory(const QString&, const QString&) override {}
    void setCandleHistoryViewport(const QString&, int64_t, qint64, qint64) override {}
    void sendTradeCommand(const trading::TradeCommand& cmd) override { trades.push_back(cmd); }
    void sendAlgoCommand(const std::string&, const std::string& action,
                         const std::string&, const trading::AlgoParams&) override { algoActions.push_back(action); }
    bool isConnectionActive() const override { return connected; }
    const LiveOrderBook& getDirectLiveOrderBook(const std::string&) const override { return book; }
};

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
} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
