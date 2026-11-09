#include "CandleWireFixture.hpp"
#include "flow/FlowCodec.hpp"
#include "flow/FlowCompose.hpp"
#include "flow/FlowScales.hpp"
#include "roller/JournalFeed.hpp"
#include "roller/Roller.hpp"
#include <QTimeZone>
#include <gtest/gtest.h>

struct FlowLiveTestAccess {
  static void tick(ServerDataModel &m) { m.publishFlow(); }
  static void stopTimer(ServerDataModel &m) { m.m_flowTimer.stop(); }
  static auto &series(ServerDataModel &m, const std::string &symbol,
                      int64_t day, int64_t tf) {
    return m.m_flowProducts.at(symbol).days.at(day).series.at(tf);
  }
  static size_t subscriptions(ServerDataModel &m) {
    return m.m_flowSubscriptions.size();
  }
  static size_t seriesCount(ServerDataModel &m, int64_t day) {
    return m.m_flowProducts.at("BTC-USD").days.at(day).series.size();
  }
  static size_t hourCount(ServerDataModel &m, int64_t day) {
    return m.m_flowProducts.at("BTC-USD").days.at(day).hours.size();
  }
  static uint64_t missingGridDrops(ServerDataModel &m) { return m.m_flowMissingGridTrades; }
  static uint64_t totalDrops(ServerDataModel &m) { return m.m_flowDroppedTrades; }
  static uint64_t hourShrinkRefusals(ServerDataModel &m) { return m.m_flowHourShrinkRefusals; }
  static const auto& hour(ServerDataModel &m, int64_t day, int64_t start) {
    return m.m_flowProducts.at("BTC-USD").days.at(day).hours.at(start);
  }
  static void discardEarlyMinutesAndRecompose(ServerDataModel &m, int64_t day,
                                               int64_t hourStart) {
    auto &product = m.m_flowProducts.at("BTC-USD");
    auto &state = product.days.at(day);
    auto &minute = state.series.at(60'000);
    const int64_t keep = hourStart + 59 * 60'000;
    minute.builder.dropBefore(keep);
    for (auto &slot : minute.bucketSlots)
      if (slot.start >= hourStart && slot.start < keep) slot = {};
    m.updateHour(state, product, hourStart);
  }
  static const auto& coarse(ServerDataModel &m, int64_t day, int64_t tf, int64_t start) {
    return m.m_flowProducts.at("BTC-USD").days.at(day).coarse.at({tf, start});
  }
};

struct FlowLiveWireTestAccess {
  static void subscribe(Session &session, const Json &request) {
    session.handleFlowSubscribe(request);
  }
  static void unsubscribe(Session &session, const Json &request) {
    session.handleFlowUnsubscribe(request);
  }
  static size_t views(const Session &session) {
    return session.flowViews_.size();
  }
  static bool active(const Session &session, uint64_t id) {
    return session.flowViews_.at(id)->active.load();
  }
  static bool queuedFrameDropped(Session &session, uint64_t id) {
    auto view = session.flowViews_.at(id);
    session.write_queue_.push_back({"queued-flow", false, false, false, {}, view});
    session.pendingWriteBytes_.fetch_add(11);
    session.handleFlowUnsubscribe({{"sub", id}});
    session.internal_async_write();
    return session.write_queue_.empty() && session.pendingWriteBytes_.load() == 0;
  }
};

namespace {
constexpr int64_t Day = 1'791'158'400'000;
Json metadata() {
  return {{"product_id", "BTC-USD"},
          {"quote_increment", "0.01"},
          {"base_increment", "0.00000001"}};
}
void grid(ServerDataModel &model) {
  model.onFlowGrid("BTC-USD", Day,
                   sentinel::roller::deriveGrid(metadata(), 100000),
                   metadata());
}
Trade trade(int64_t ms, std::string id, double size = 1,
            AggressorSide side = AggressorSide::Buy) {
  return {std::chrono::system_clock::time_point(std::chrono::milliseconds(ms)),
          "BTC-USD",
          std::move(id),
          side,
          100000,
          size};
}
sentinel::flow::Trade flowTrade(const Trade &t) {
  return {t.trade_id, tradeTimestampMs(t), t.price, t.size,
          t.side == AggressorSide::Buy    ? sentinel::flow::Side::Buy
          : t.side == AggressorSide::Sell ? sentinel::flow::Side::Sell
                                          : sentinel::flow::Side::Unknown};
}
sentinel::flow::Bytes bytes(const Json &frame) {
  const auto data = QByteArray::fromBase64(
      QByteArray::fromStdString(frame.at("bar").get<std::string>()));
  return {data.begin(), data.end()};
}
class FlowLive : public testing::Test {
protected:
  int argc = 1;
  char name[16] = "flow-live";
  char *argv[2] = {name, nullptr};
  QCoreApplication app{argc, argv};
  ServerConfig config;
  std::unique_ptr<ServerDataModel> model;
  std::vector<Json> frames;
  void SetUp() override {
    config.recording.enabled = false;
    config.rollerShadow.enabled = false;
    config.rollerShadow.products = {"BTC-USD"};
    config.chart.timeframesMs = {100, 1000, 60000, 300000, 3600000, 86400000};
    config.recording.livePublishMs = 250;
    model = std::make_unique<ServerDataModel>(config);
    FlowLiveTestAccess::stopTimer(*model);
    grid(*model);
  }
  auto subscribe(int64_t tf = 60000, uint64_t id = 1) {
    auto sub = std::make_shared<FlowSubscription>();
    sub->sub = id;
    sub->symbol = "BTC-USD";
    sub->timeframeMs = tf;
    sub->publish = [this](std::string payload) {
      frames.push_back(Json::parse(payload));
    };
    model->subscribeFlow(sub);
    return sub;
  }
  void tick(int64_t journalMs) {
    model->onFeedWatermark("BTC-USD", journalMs);
    FlowLiveTestAccess::tick(*model);
  }
};

TEST_F(FlowLive, CoalescesChangesClosesOnceAndRevisesLateTrade) {
  auto sub = subscribe();
  for (int i = 0; i < 100; ++i)
    model->onTrade(trade(Day + 10000 + i, std::to_string(i)));
  EXPECT_TRUE(frames.empty());
  tick(Day + 11000);
  ASSERT_EQ(frames.size(), 1u);
  EXPECT_EQ(sentinel::flow::decodeFlowBar(bytes(frames.back())).count, 100u);
  tick(Day + 12000);
  EXPECT_EQ(frames.size(), 1u);
  tick(Day + 60999);
  EXPECT_EQ(frames.size(), 1u);
  tick(Day + 61000);
  ASSERT_EQ(frames.size(), 2u);
  EXPECT_EQ(frames.back()["closed"], true);
  EXPECT_EQ(frames.back()["revision"], 0);
  model->onTrade(trade(Day + 30000, "late"));
  tick(Day + 62000);
  ASSERT_EQ(frames.size(), 3u);
  EXPECT_EQ(frames.back()["revision"], 1);
  EXPECT_EQ(sentinel::flow::decodeFlowBar(bytes(frames.back())).count, 101u);
  model->onTrade(trade(Day + 30000, "late")); // resend never dirties a bar
  tick(Day + 63000);
  EXPECT_EQ(frames.size(), 3u);
  sub->active.store(false);
  model->onTrade(trade(Day + 64000, "after-unsubscribe"));
  tick(Day + 65000);
  EXPECT_EQ(frames.size(), 3u);
}

TEST_F(FlowLive, SecondRetentionDropsOldTradesAndCannotGrowWithoutTimer) {
  auto sub = subscribe(1000);
  for (int i = 0; i < 3600; ++i) {
    model->onTrade(trade(Day + i * 1000, std::to_string(i)));
    tick(Day + (i + 1) * 1000);
  }
  auto &series = FlowLiveTestAccess::series(*model, "BTC-USD", Day, 1000);
  EXPECT_LE(series.builder.bars().size(), 1800u);
  const auto before = series.builder.bars().size();
  const auto drops = series.builder.droppedRejectCount() + series.capacityDrops;
  model->onTrade(trade(Day + 1000, "expired"));
  EXPECT_EQ(series.builder.bars().size(), before);
  EXPECT_EQ(series.builder.droppedRejectCount() + series.capacityDrops,
            drops + 1);
  // With no publication opportunity, capacity remains bounded too.
  for (int i = 3600; i < 9000; ++i)
    model->onTrade(trade(Day + i * 1000, std::to_string(i)));
  EXPECT_LE(series.builder.bars().size(), 1802u);
  EXPECT_GT(series.capacityDrops, 0u);
}

TEST_F(FlowLive, SubscribedChartTimeframeIsRemovedAndInitialSubscribeResends) {
  auto minute = subscribe();
  auto chart = subscribe(300000, 2);
  model->onTrade(trade(Day + 10000, "1"));
  tick(Day + 11000);
  ASSERT_EQ(frames.size(), 2u);
  EXPECT_EQ(FlowLiveTestAccess::seriesCount(*model, Day), 2u);
  auto resend = subscribe(60000, 3);
  tick(Day + 12000);
  ASSERT_EQ(frames.size(), 3u);
  EXPECT_EQ(frames.back()["sub"], 3);
  chart->active.store(false);
  tick(Day + 13000);
  EXPECT_EQ(FlowLiveTestAccess::seriesCount(*model, Day), 2u);
}

TEST_F(FlowLive, LiveHourEqualsJournalBuildWithReconnectAndLateRevision) {
  using namespace sentinel;
  using namespace sentinel::roller;
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  capture::WriterConfig cfg;
  cfg.root = dir.path();
  cfg.fsyncBlocks = 0;
  capture::Writer writer(cfg, {{"product_metadata", metadata()}});
  auto append = [&](int64_t received, int64_t exchange, const std::string &id,
                    uint64_t connection = 1) {
    Json print = {{"product_id", "BTC-USD"},
                  {"trade_id", id},
                  {"time", QDateTime::fromMSecsSinceEpoch(
                               exchange, QTimeZone(QTimeZone::UTC))
                               .toString(Qt::ISODateWithMs)
                               .toStdString()},
                  {"price", "100000"},
                  {"size", "1.25"},
                  {"side", "BUY"}};
    Json message = {{"channel", "market_trades"},
                    {"trades", Json::array({print})}};
    writer.append({capture::Kind::Frame,
                   {received * 1'000'000, 0},
                   connection,
                   message.dump()});
  };
  append(Day + 10000, Day + 10000, "1");
  append(Day + 61000, Day + 60000, "2");
  append(Day + 62000, Day + 10000, "1", 2); // reconnect resend
  append(Day + 63000, Day + 30000, "late", 2);
  for (int i = 2; i < 60; ++i)
    append(Day + i * 60000 + 1000, Day + i * 60000, std::to_string(i + 1), 2);
  writer.close();
  auto sub = subscribe();
  const auto scales = flow::flowScales(sentinel::roller::deriveGrid(metadata(), 100000), metadata());
  flow::FlowBarBuilder store(60000, scales.nativeTick, scales.priceScale,
                            scales.baseScale, scales.quoteScale);
  JournalFeed stored("BTC-USD"), live("BTC-USD");
  stored.onTrade = [&](const Trade &t) { store.add(flowTrade(t)); };
  live.onTrade = [&](const Trade &t) { model->onTrade(t); };
  JournalReader reader(dir.path().toStdString(), "BTC-USD");
  JournalRecord record;
  while (reader.next(record)) {
    stored.apply(record);
    live.apply(record);
    tick(record.record.time.systemNs / 1'000'000);
  }
  // Exercise the model's own bounded dedup too, independently of JournalFeed.
  model->onTrade(trade(Day + 59000 * 60, "60", 1.25, AggressorSide::Sell));
  tick(Day + 3'601'000);
  auto &liveBuilder =
      FlowLiveTestAccess::series(*model, "BTC-USD", Day, 60000).builder;
  const auto expected = store.bars();
  const auto actual = liveBuilder.bars();
  ASSERT_EQ(expected.size(), 60u);
  ASSERT_EQ(actual.size(), expected.size());
  for (size_t i = 0; i < expected.size(); ++i) {
    const auto wire = flow::encodeFlowBar(actual[i]);
    EXPECT_EQ(flow::encodeFlowBar(flow::decodeFlowBar(wire)),
              flow::encodeFlowBar(expected[i]));
  }
  EXPECT_EQ(liveBuilder.revisionAt(Day), 1u);
  EXPECT_EQ(store.revisionAt(Day), 0u);
  auto revised = std::find_if(frames.begin(), frames.end(), [](const Json &j) {
    return j["bucket_start_ms"] == Day && j["revision"] == 1;
  });
  ASSERT_NE(revised, frames.end());
  EXPECT_EQ(bytes(*revised), flow::encodeFlowBar(expected.front()));
}

TEST_F(FlowLive, RollOptionsGridCallbackInitializesLiveBuilder) {
  using namespace sentinel::roller;
  QTemporaryDir journal, derived;
  ASSERT_TRUE(journal.isValid());
  ASSERT_TRUE(derived.isValid());
  sentinel::capture::WriterConfig cfg;
  cfg.root = journal.path();
  cfg.fsyncBlocks = 0;
  sentinel::capture::Writer writer(cfg, {{"product_metadata", metadata()}});
  Json snapshot = {
      {"channel", "l2_data"},
      {"events",
       Json::array({{{"type", "snapshot"},
                     {"product_id", "BTC-USD"},
                     {"updates", Json::array({{{"side", "bid"},
                                               {"price_level", "99999"},
                                               {"new_quantity", "2"}},
                                              {{"side", "offer"},
                                               {"price_level", "100001"},
                                               {"new_quantity", "3"}}})}}})}};
  writer.append({sentinel::capture::Kind::Frame,
                 {(Day + 1000) * 1'000'000, 0},
                 1,
                 snapshot.dump()});
  writer.close();
  ServerDataModel live(config);
  FlowLiveTestAccess::stopTimer(live);
  RollOptions options{journal.path().toStdString(),
                      derived.path().toStdString(), "BTC-USD", Day,
                      Day + 60'000};
  options.dryRun = true;
  int grids = 0;
  options.onGrid = [&](int64_t day, const Grid &recorded, const Json &header) {
    ++grids;
    live.onFlowGrid("BTC-USD", day, recorded, header);
  };
  roll(options);
  EXPECT_EQ(grids, 1);
  live.onTrade(trade(Day + 2000, "grid"));
  const auto bars =
      FlowLiveTestAccess::series(live, "BTC-USD", Day, 60000).builder.bars();
  ASSERT_EQ(bars.size(), 1u);
  EXPECT_DOUBLE_EQ(bars.front().nativeTick, 1);
  EXPECT_EQ(bars.front().priceScale, 100);
}

TEST_F(FlowLive, MidHourSubscribeComposesMinutesAndMarksMissingCoverage) {
  model->onTrade(trade(Day + 10 * 60'000, "ten"));
  tick(Day + 10 * 60'000 + 1000);
  model->onTrade(trade(Day + 11 * 60'000, "eleven"));
  tick(Day + 11 * 60'000 + 1000);
  auto sub = subscribe(3'600'000);
  tick(Day + 11 * 60'000 + 2000);
  ASSERT_FALSE(frames.empty());
  const auto first = frames.back();
  EXPECT_EQ(first["timeframe_ms"], 3'600'000);
  EXPECT_EQ(first["complete"], false);
  auto minutes = FlowLiveTestAccess::series(*model, "BTC-USD", Day, 60'000).builder.bars();
  const auto composed = sentinel::flow::composeFlowBars(minutes, 3'600'000);
  EXPECT_EQ(bytes(first), sentinel::flow::encodeFlowBar(composed));
  sub->active.store(false);
  tick(Day + 11 * 60'000 + 3000);
  frames.clear();
  auto again = subscribe(3'600'000, 2);
  tick(Day + 11 * 60'000 + 4000);
  ASSERT_EQ(frames.size(), 1u);
  EXPECT_EQ(frames[0]["complete"], false);
  EXPECT_EQ(bytes(frames[0]), sentinel::flow::encodeFlowBar(composed));
}

TEST_F(FlowLive, DailySubscriptionKeepsOnlyMinuteAndSecondBuilders) {
  auto sub = subscribe(86'400'000);
  for (int hour = 0; hour < 8; ++hour) {
    for (int i = 0; i < 100; ++i)
      model->onTrade(trade(Day + hour * 3'600'000 + i, std::to_string(hour * 100 + i)));
    tick(Day + hour * 3'600'000 + 1000);
  }
  EXPECT_EQ(FlowLiveTestAccess::seriesCount(*model, Day), 2u);
  EXPECT_LE(FlowLiveTestAccess::hourCount(*model, Day), 24u);
  EXPECT_EQ(FlowLiveTestAccess::coarse(*model, Day, 86'400'000, Day).bar.count, 800u);
  EXPECT_EQ(frames.back()["complete"], true);
}

TEST_F(FlowLive, MissingGridTradeIsCounted) {
  ServerDataModel bare(config);
  FlowLiveTestAccess::stopTimer(bare);
  bare.onTrade(trade(Day + 1000, "no-grid"));
  EXPECT_EQ(FlowLiveTestAccess::missingGridDrops(bare), 1u);
  EXPECT_EQ(FlowLiveTestAccess::totalDrops(bare), 1u);
  grid(bare);
  bare.onTrade(trade(Day + 2000, "grid"));
  EXPECT_EQ(FlowLiveTestAccess::missingGridDrops(bare), 1u);
}

TEST_F(FlowLive, GridCanChangeBeforeTradesButNotAfterwards) {
  auto alternate = sentinel::roller::deriveGrid(metadata(), 100000, {{"near_tick", 2.0}});
  model->onFlowGrid("BTC-USD", Day, alternate, metadata());
  model->onTrade(trade(Day + 1000, "first"));
  auto before = FlowLiveTestAccess::series(*model, "BTC-USD", Day, 60'000).builder.bars();
  ASSERT_EQ(before.size(), 1u);
  EXPECT_DOUBLE_EQ(before.front().nativeTick, 2);
  grid(*model);
  model->onTrade(trade(Day + 2000, "second"));
  auto after = FlowLiveTestAccess::series(*model, "BTC-USD", Day, 60'000).builder.bars();
  ASSERT_EQ(after.size(), 1u);
  EXPECT_DOUBLE_EQ(after.front().nativeTick, 2);
  EXPECT_EQ(after.front().count, 2u);
}

TEST_F(FlowLive, MidnightUsesEachDaysGridAndDoesNotMixBuckets) {
  auto next = sentinel::roller::deriveGrid(metadata(), 100000, {{"near_tick", 2.0}});
  model->onFlowGrid("BTC-USD", Day + 86'400'000, next, metadata());
  auto sub = subscribe();
  model->onTrade(trade(Day + 86'399'000, "old"));
  model->onTrade(trade(Day + 86'401'000, "new"));
  tick(Day + 86'402'000);
  const auto old = FlowLiveTestAccess::series(*model, "BTC-USD", Day, 60'000).builder.bars();
  const auto fresh = FlowLiveTestAccess::series(*model, "BTC-USD", Day + 86'400'000, 60'000).builder.bars();
  ASSERT_EQ(old.size(), 1u);
  ASSERT_EQ(fresh.size(), 1u);
  EXPECT_EQ(old[0].startMs, Day + 86'340'000);
  EXPECT_EQ(fresh[0].startMs, Day + 86'400'000);
  EXPECT_DOUBLE_EQ(old[0].nativeTick, 1);
  EXPECT_DOUBLE_EQ(fresh[0].nativeTick, 2);
}

TEST_F(FlowLive, MidnightRetainsClosedHoursForLateDailyRevisionWithinGrace) {
  model->onFlowGrid("BTC-USD", Day + 86'400'000,
                    sentinel::roller::deriveGrid(metadata(), 100000), metadata());
  auto daily = subscribe(86'400'000);
  for (int hour = 0; hour < 24; ++hour) {
    model->onTrade(trade(Day + hour * 3'600'000, std::to_string(hour)));
    tick(Day + (hour + 1) * 3'600'000 + 1000);
  }
  EXPECT_EQ(FlowLiveTestAccess::hourCount(*model, Day), 24u);
  model->onTrade(trade(Day + 86'401'000, "next-day"));
  tick(Day + 86'402'000);
  EXPECT_EQ(FlowLiveTestAccess::hourCount(*model, Day), 24u);
  EXPECT_EQ(FlowLiveTestAccess::hourCount(*model, Day + 86'400'000), 0u);
  model->onTrade(trade(Day + 23 * 3'600'000 + 1000, "late"));
  tick(Day + 86'403'000);
  const auto& revised = FlowLiveTestAccess::coarse(*model, Day, 86'400'000, Day);
  EXPECT_EQ(revised.bar.count, 25u);
  EXPECT_GE(revised.revision, 1u);
  EXPECT_EQ(FlowLiveTestAccess::hourCount(*model, Day), 24u);
}

TEST_F(FlowLive, WatermarkJumpCannotShrinkHourOrLoseItsClose) {
  auto hourSub = subscribe(3'600'000);
  auto daySub = subscribe(86'400'000, 2);
  const int64_t hourStart = Day + 10 * 3'600'000;
  for (int minute = 0; minute < 60; ++minute) {
    model->onTrade(trade(hourStart + minute * 60'000, std::to_string(minute)));
    tick(hourStart + minute * 60'000 + 1000);
  }
  model->onFeedWatermark("BTC-USD", hourStart + 3'600'000 + 121'000);
  FlowLiveTestAccess::tick(*model);
  EXPECT_EQ(FlowLiveTestAccess::hour(*model, Day, hourStart).bar.count, 60u);
  EXPECT_EQ(FlowLiveTestAccess::coarse(*model, Day, 86'400'000, Day).bar.count, 60u);
  const auto closed = std::find_if(frames.begin(), frames.end(), [&](const Json &frame) {
    return frame["timeframe_ms"] == 3'600'000 &&
           frame["bucket_start_ms"] == hourStart && frame["closed"] == true;
  });
  ASSERT_NE(closed, frames.end());
  EXPECT_EQ(sentinel::flow::decodeFlowBar(bytes(*closed)).count, 60u);
}

TEST_F(FlowLive, LateMinuteBeforeWatermarkCannotShrinkClosedHour) {
  auto hourSub = subscribe(3'600'000);
  const int64_t hourStart = Day + 10 * 3'600'000;
  for (int minute = 0; minute < 60; ++minute) {
    model->onTrade(trade(hourStart + minute * 60'000, std::to_string(minute)));
    tick(hourStart + minute * 60'000 + 1000);
  }
  tick(hourStart + 3'601'000);
  model->onTrade(trade(hourStart + 3'599'800, "late"));
  model->onFeedWatermark("BTC-USD", hourStart + 3'600'000 + 61'200);
  FlowLiveTestAccess::tick(*model);
  const auto &closed = FlowLiveTestAccess::hour(*model, Day, hourStart);
  EXPECT_EQ(closed.bar.count, 61u);
  EXPECT_GE(closed.revision, 1u);
  EXPECT_EQ(closed.sourcePartCount, 60u);
}

TEST_F(FlowLive, CachedClosedHourRefusesShorterSource) {
  auto sub = subscribe(3'600'000);
  for (int minute = 0; minute < 60; ++minute) {
    model->onTrade(trade(Day + minute * 60'000, std::to_string(minute)));
    tick(Day + minute * 60'000 + 1000);
  }
  tick(Day + 3'601'000);
  ASSERT_EQ(FlowLiveTestAccess::hour(*model, Day, Day).bar.count, 60u);
  FlowLiveTestAccess::discardEarlyMinutesAndRecompose(*model, Day, Day);
  EXPECT_EQ(FlowLiveTestAccess::hourShrinkRefusals(*model), 1u);
  EXPECT_EQ(FlowLiveTestAccess::hour(*model, Day, Day).bar.count, 60u);
}

TEST_F(FlowLive, EmptyMinutesRemainCoveredWhenHourCloses) {
  auto sub = subscribe(3'600'000);
  model->onTrade(trade(Day, "only-trade"));
  tick(Day + 1000);
  tick(Day + 3'601'000);
  const auto closed = std::find_if(frames.begin(), frames.end(), [](const Json &frame) {
    return frame["timeframe_ms"] == 3'600'000 && frame["closed"] == true;
  });
  ASSERT_NE(closed, frames.end());
  EXPECT_EQ((*closed)["complete"], true);
  EXPECT_EQ(sentinel::flow::decodeFlowBar(bytes(*closed)).count, 1u);
}

TEST_F(FlowLive, CapacityDropClearsPreviouslyCompleteDailyBar) {
  auto sub = subscribe(86'400'000);
  model->onTrade(trade(Day, "first"));
  tick(Day + 1000);
  ASSERT_EQ(frames.back()["complete"], true);
  for (int minute = 1; minute <= 62; ++minute)
    model->onTrade(trade(Day + minute * 60'000, std::to_string(minute)));
  tick(Day + 62 * 60'000 + 1000);
  EXPECT_GT(FlowLiveTestAccess::series(*model, "BTC-USD", Day, 60'000).capacityDrops, 0u);
  EXPECT_EQ(frames.back()["complete"], false);
}

TEST_F(FlowLive, RejectedTradeClearsPreviouslyCompleteDailyBar) {
  auto sub = subscribe(86'400'000);
  model->onTrade(trade(Day, "first"));
  tick(Day + 1000);
  ASSERT_EQ(frames.back()["complete"], true);
  model->onTrade(trade(Day + 1000, "invalid-size", 0));
  tick(Day + 2000);
  ASSERT_EQ(frames.size(), 2u);
  EXPECT_EQ(frames.back()["complete"], false);
  EXPECT_EQ(sentinel::flow::decodeFlowBar(bytes(frames.back())).count, 1u);
}

TEST_F(FlowLive, MissingGridDropMarksLaterHourIncomplete) {
  ServerDataModel bare(config);
  FlowLiveTestAccess::stopTimer(bare);
  bare.onTrade(trade(Day, "missing"));
  grid(bare);
  std::vector<Json> seen;
  auto sub = std::make_shared<FlowSubscription>();
  sub->sub = 1; sub->symbol = "BTC-USD"; sub->timeframeMs = 3'600'000;
  sub->publish = [&](std::string payload) { seen.push_back(Json::parse(payload)); };
  bare.subscribeFlow(sub);
  bare.onTrade(trade(Day, "present"));
  bare.onFeedWatermark("BTC-USD", Day + 1000);
  FlowLiveTestAccess::tick(bare);
  ASSERT_EQ(seen.size(), 1u);
  EXPECT_EQ(seen.front()["complete"], false);
  EXPECT_EQ(FlowLiveTestAccess::missingGridDrops(bare), 1u);
}

TEST_F(FlowLive, DailyHourCompositionMatchesDirectBuildWithSweeps) {
  using namespace sentinel::flow;
  auto sub = subscribe(86'400'000);
  const auto scales = flowScales(sentinel::roller::deriveGrid(metadata(), 100000), metadata());
  FlowBarBuilder direct(86'400'000, scales.nativeTick, scales.priceScale,
                        scales.baseScale, scales.quoteScale);
  for (int hour : {0, 2, 5, 8}) {
    for (int i = 0; i < 4; ++i) {
      auto t = trade(Day + hour * 3'600'000 + i * 1000,
                     std::to_string(hour) + "-" + std::to_string(i),
                     1.0 + i * 0.25, i % 2 ? AggressorSide::Sell : AggressorSide::Buy);
      t.price = 100000 + (hour % 3) * 3 + i;
      model->onTrade(t);
      ASSERT_EQ(direct.add(flowTrade(t)), FlowBarBuilder::AddResult::Accepted);
    }
    tick(Day + hour * 3'600'000 + 5000);
  }
  const auto &composite = FlowLiveTestAccess::coarse(*model, Day, 86'400'000, Day).bar;
  const auto expected = direct.bars().front();
  ASSERT_GT(expected.rows.size(), 1u);
  ASSERT_EQ(expected.notable.size(), NotableCapacity);
  const auto wire = encodeFlowBar(composite);
  EXPECT_EQ(encodeFlowBar(decodeFlowBar(wire)), encodeFlowBar(expected));
}

TEST_F(FlowLive, StalledPublishStillSendsExpiredCoarseClose) {
  auto sub = subscribe(300'000);
  model->onTrade(trade(Day, "first"));
  tick(Day + 1000);
  frames.clear();
  model->onFeedWatermark("BTC-USD", Day + 421'000);
  FlowLiveTestAccess::tick(*model);
  ASSERT_EQ(frames.size(), 1u);
  EXPECT_EQ(frames.front()["bucket_start_ms"], Day);
  EXPECT_EQ(frames.front()["closed"], true);
}

TEST_F(FlowLive, RealTimerBoundsBurstAndStopsAfterUnsubscribe) {
  model = std::make_unique<ServerDataModel>(config);
  grid(*model);
  auto sub = subscribe();
  for (int i = 0; i < 200; ++i)
    model->onTrade(trade(Day + 10000, std::to_string(i)));
  ASSERT_TRUE(spinUntil([&] { return !frames.empty(); }));
  ASSERT_EQ(frames.size(), 1u);
  EXPECT_EQ(sentinel::flow::decodeFlowBar(bytes(frames[0])).count, 200u);
  FlowLiveTestAccess::tick(*model);
  EXPECT_EQ(frames.size(), 1u); // unchanged content is not re-sent
  sub->active.store(false);
  model->onTrade(trade(Day + 20000, "cancelled"));
  FlowLiveTestAccess::tick(*model);
  EXPECT_EQ(frames.size(), 1u);
}

TEST_F(FlowLive, WireSubscribeReplaceAndUnsubscribe) {
  net::io_context io;
  ssl::context tls{ssl::context::tls_server};
  auto session =
      std::make_shared<Session>(tcp::socket(io), tls, *model, nullptr);
  FlowLiveWireTestAccess::subscribe(
      *session,
      {{"sub", uint64_t{7}}, {"symbol", "BTC-USD"}, {"timeframe_ms", 60000}});
  ASSERT_TRUE(spinUntil(
      [&] { return FlowLiveTestAccess::subscriptions(*model) == 1; }));
  EXPECT_EQ(FlowLiveWireTestAccess::views(*session), 1u);
  model->onTrade(trade(Day + 10000, "wire"));
  FlowLiveTestAccess::tick(*model);
  EXPECT_GT(RecordingServerStopTest::pendingWriteBytes(*session), 0u);
  FlowLiveWireTestAccess::subscribe(
      *session,
      {{"sub", uint64_t{7}}, {"symbol", "BTC-USD"}, {"timeframe_ms", 300000}});
  ASSERT_TRUE(spinUntil([&] {
    FlowLiveTestAccess::tick(*model);
    return FlowLiveTestAccess::subscriptions(*model) == 1;
  }));
  EXPECT_TRUE(FlowLiveWireTestAccess::active(*session, 7));
  FlowLiveWireTestAccess::unsubscribe(*session, {{"sub", uint64_t{7}}});
  EXPECT_EQ(FlowLiveWireTestAccess::views(*session), 0u);
  FlowLiveTestAccess::tick(*model);
  EXPECT_EQ(FlowLiveTestAccess::subscriptions(*model), 0u);
}

TEST_F(FlowLive, RefusedSubscriptionsDoNotConsumeConnectionSlots) {
  net::io_context io;
  ssl::context tls{ssl::context::tls_server};
  auto session = std::make_shared<Session>(tcp::socket(io), tls, *model, nullptr);
  for (uint64_t id = 1; id <= 32; ++id)
    FlowLiveWireTestAccess::subscribe(*session,
        {{"sub", id}, {"symbol", "BTC-USD"}, {"timeframe_ms", 2000}});
  ASSERT_TRUE(spinUntil([&] {
    return !FlowLiveWireTestAccess::active(*session, 32);
  }));
  FlowLiveWireTestAccess::subscribe(*session,
      {{"sub", uint64_t{33}}, {"symbol", "BTC-USD"}, {"timeframe_ms", 60000}});
  ASSERT_TRUE(spinUntil([&] { return FlowLiveTestAccess::subscriptions(*model) == 1; }));
  EXPECT_EQ(FlowLiveWireTestAccess::views(*session), 1u);
  EXPECT_TRUE(FlowLiveWireTestAccess::active(*session, 33));
}

TEST_F(FlowLive, UnsubscribeDropsAlreadyQueuedFlowFrame) {
  net::io_context io;
  ssl::context tls{ssl::context::tls_server};
  auto session = std::make_shared<Session>(tcp::socket(io), tls, *model, nullptr);
  FlowLiveWireTestAccess::subscribe(*session,
      {{"sub", uint64_t{7}}, {"symbol", "BTC-USD"}, {"timeframe_ms", 60000}});
  EXPECT_TRUE(FlowLiveWireTestAccess::queuedFrameDropped(*session, 7));
  EXPECT_EQ(FlowLiveWireTestAccess::views(*session), 0u);
}

} // namespace
