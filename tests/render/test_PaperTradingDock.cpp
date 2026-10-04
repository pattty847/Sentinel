#include "widgets/PaperTradingDock.hpp"
#include "datasources/IGridDataSource.hpp"
#include <QApplication>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QPushButton>
#include <QScrollBar>
#include <QTableWidget>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <gtest/gtest.h>

namespace {
class Source final : public IGridDataSource {
public:
    std::vector<trading::TradeCommand> trades;
    std::vector<std::string> algoActions;
    LiveOrderBook book;
    void subscribe(const QString&) override {}
    void unsubscribe(const QString&) override {}
    void requestHeatmapHistory(const QString&, int64_t, int64_t, int) override {}
    void registerRecordingView(const recording::LiveView&) override {}
    void requestRecordingHeatmapHistory(const protocol::recordingwire::Request&) override {}
    void requestFootprintHistory(const QString&, int64_t, int64_t, int) override {}
    void requestTpoHistory(const QString&, int64_t, int, int64_t, int, const QString&) override {}
    void cancelTpoHistory(const QString&, const QString&) override {}
    void setCandleHistoryViewport(const QString&, int64_t, qint64, qint64) override {}
    void sendTradeCommand(const trading::TradeCommand& cmd) override { trades.push_back(cmd); }
    void sendAlgoCommand(const std::string&, const std::string& action,
                         const std::string&, const trading::AlgoParams&) override { algoActions.push_back(action); }
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
    EXPECT_DOUBLE_EQ(price->value(), 0.399);
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
    timer->start(1);
    QTest::qWait(20);
    EXPECT_EQ(status->text(), "UNCONFIRMED");
    emit source.connectionStatusChanged(false);
    QTest::qWait(10);
    EXPECT_EQ(status->text(), "UNAVAILABLE");
    dock.setSymbol("PEPE-USD");
    EXPECT_EQ(status->text(), "UNAVAILABLE");
}
} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
