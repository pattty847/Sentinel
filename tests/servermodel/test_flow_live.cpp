#include "CandleWireFixture.hpp"
#include "FlowLiveTestAccess.hpp"
#include "flow/FlowCodec.hpp"
#include "flow/FlowCompose.hpp"
#include "flow/FlowScales.hpp"
#include "roller/JournalFeed.hpp"
#include "roller/Roller.hpp"
#include "servermodel/FlowStore.hpp"
#include "servermodel/FlowChunkStore.hpp"
#include "../roller/fixtures/FlowJournal.hpp"
#include "roller/JournalReader.hpp"
#include "Crc32.hpp"
#include <QTimeZone>
#include <gtest/gtest.h>
#include <fstream>


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
  static void holdWrites(Session &session) { session.flowReplayHoldForTest_ = true; }
  static size_t replayBytes(const Session &session, uint64_t id) {
    return session.flowReplays_.at(id).queuedBytes;
  }
  static size_t pendingReplayFrames(const Session &session, uint64_t id) {
    return std::count_if(session.write_queue_.begin(), session.write_queue_.end(),
                         [id](const auto& w) { return w.flowReplay && w.flowSubscription &&
                                                       w.flowSubscription->sub == id; });
  }
  static size_t heldReplayFrameBytes(const Session &session, uint64_t id) {
    return session.flowReplays_.at(id).pendingFrame.size();
  }
  static void seedReplay(Session &session, uint64_t id) {
    auto view = std::make_shared<FlowSubscription>();
    view->sub = id; view->symbol = "BTC-USD"; view->timeframeMs = 60'000;
    session.flowViews_[id] = view;
    auto& state = session.flowReplays_[id];
    state.view = view; state.awaitingFrame = true;
  }
  static void submitReplay(Session &session, uint64_t id, std::string frame) {
    session.flowReplays_.at(id).pendingFrame = std::move(frame);
    session.pumpFlowReplay(id);
  }
  static void completeWrite(Session &session) { session.on_write_complete({}, 0); }
  static void dropCancelled(Session &session) { session.internal_async_write(); }
  static void replayUpdate(Session &session, uint64_t id, Json frame) {
    session.flowUpdate(session.flowViews_.at(id), frame.dump());
  }
  static size_t coalescedUpdates(const Session &session, uint64_t id) {
    return session.flowReplays_.at(id).updates.size();
  }
  static void finishReplay(Session &session, uint64_t id) {
    auto& state = session.flowReplays_.at(id);
    state.sourceDone = true; state.awaitingFrame = false;
    session.pumpFlowReplay(id);
  }
  static std::vector<Json> queuedFlow(Session &session, uint64_t id) {
    std::vector<Json> out;
    for (const auto& write : session.write_queue_)
      if (write.flowSubscription && write.flowSubscription->sub == id)
        out.push_back(Json::parse(write.payload));
    return out;
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
    config.chart.timeframesMs = {100, 1000, 60000, 300000, 3600000, 14'400'000, 86400000};
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

TEST_F(FlowLive, RetainedClosedMinuteThreeHoursOldDropsAfterSealPoint) {
  QTemporaryDir temp;
  ASSERT_TRUE(temp.isValid());
  const auto base = std::filesystem::path(temp.path().toStdString());
  std::filesystem::create_directory(base / "journal");
  std::filesystem::create_directory(base / "hmc");
  recording::FlowStore::Options options;
  options.root = base / "flow";
  options.journalRoot = base / "journal";
  options.hmc2Root = base / "hmc";
  options.symbols = {"BTC-USD"};
  options.hourThrough = [](const std::string&) { return Day; };
  recording::FlowStore store(std::move(options));
  model->attachFlowStore(&store);
  auto sub = subscribe();
  model->onTrade(trade(Day, "first"));
  tick(Day + 61'000);
  ASSERT_FALSE(frames.empty());
  const auto initial = frames.back();
  ASSERT_EQ(initial["bucket_start_ms"], Day);
  EXPECT_EQ(initial["revision"], 0);
  EXPECT_EQ(initial["complete"], true);
  tick(Day + 3 * 3'600'000 + 60'000);
  const auto before = frames.size();
  model->onTrade(trade(Day + 30'000, "late"));
  tick(Day + 3 * 3'600'000 + 61'000);
  EXPECT_EQ(frames.size(), before);
  EXPECT_EQ(FlowLiveTestAccess::lateDrops(*model), 1u);
  sentinel::metrics::MetricsRegistry metrics;
  model->registerMetrics(metrics);
  EXPECT_NE(metrics.render().find("sentinel_flow_live_late_trades_total{instrument=\"coinbase:BTC-USD\"} 1"),
            std::string::npos);
  sub->active.store(false);
  model->attachFlowStore(nullptr);
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

TEST_F(FlowLive, MidnightReconnectAndRestartHoldMatchesSealedMinute) {
  namespace ff = flow_fixture;
  namespace r = sentinel::roller;
  namespace c = sentinel::flow::chunks;
  for (const auto reconnect : {-2000, 2000}) for (const bool restart : {false, true}) {
    SCOPED_TRACE("reconnect=" + std::to_string(reconnect) + " restart=" + std::to_string(restart));
    QTemporaryDir temp; const auto root = std::filesystem::path(temp.path().toStdString());
    const auto raw = root / "raw", hmc = root / "hmc";
    std::filesystem::create_directory(raw); std::filesystem::create_directory(hmc);
    ff::Journal journal(raw); journal.snapshot(-ff::Minute);
    if (reconnect < 0) journal.reconnect(reconnect);
    journal.trades(100, {ff::trade(50, "midnight-1", 100000)});
    journal.trades(900, {ff::trade(800, "midnight-2", 100001)});
    if (reconnect > 0) journal.reconnect(reconnect);
    journal.trades(3000, {ff::trade(2500, "after-reconnect", 100002)});
    journal.heartbeat(5000);
    journal.heartbeat(ff::Hour + config.flow.sealWindowMs); journal.close();

    ServerDataModel live(config); FlowLiveTestAccess::stopTimer(live);
    if (!restart) live.onFlowGrid("BTC-USD", ff::Epoch - 86'400'000,
        r::deriveGrid(metadata(), 100000), metadata());
    r::JournalReader reader(raw, "BTC-USD"); r::JournalFeed feed("BTC-USD");
    int64_t maximum = INT64_MIN;
    feed.onTrade = [&](const Trade &t) {
      live.flowTradePosted(t.product_id); live.onFlowTrade(t, maximum);
    };
    bool deliveredGrid = false;
    r::JournalRecord record;
    while (reader.next(record)) {
      maximum = std::max(maximum, record.record.time.systemNs / 1'000'000);
      feed.apply(record); live.flowRecordApplied("BTC-USD", maximum);
      if (!deliveredGrid && maximum >= ff::Epoch + 5000) {
        EXPECT_EQ(FlowLiveTestAccess::heldTrades(live), 3u);
        EXPECT_TRUE(std::filesystem::is_empty(hmc)); // restart has no new-day grid on disk
        // Deliver the real roller callback after the provisional trades, as at
        // rollover while the previous day's durable recorder is draining.
        r::RollOptions options{raw, hmc, "BTC-USD", ff::Epoch, ff::Epoch + ff::Minute};
        options.onGrid = [&](int64_t day, const r::Grid &g, const Json &meta) {
          live.onFlowGrid("BTC-USD", day, g, meta);
        };
        r::roll(options); deliveredGrid = true;
      }
      FlowLiveTestAccess::tick(live);
    }
    ASSERT_TRUE(deliveredGrid);
    EXPECT_EQ(FlowLiveTestAccess::heldBytes(live), 0u);
    EXPECT_EQ(FlowLiveTestAccess::missingGridDrops(live), 0u);
    auto liveBar = FlowLiveTestAccess::series(live, "BTC-USD", ff::Epoch, ff::Minute).builder.snapshotBar(ff::Epoch);
    ASSERT_TRUE(liveBar); EXPECT_EQ(liveBar->count, 3u); liveBar->notable.clear();
    recording::FlowStore::Options o; o.journalRoot = raw; o.hmc2Root = hmc; o.root = root / "fld";
    o.symbols = {"BTC-USD"}; o.lowerPriority = false;
    o.hourThrough = [](const std::string&) { return ff::Epoch + ff::Hour; };
    recording::FlowStore source(o);
    recording::FlowChunkStore::Options co; co.root = root / "flc"; co.lowerPriority = false;
    co.rules.sealWindowMs = config.flow.sealWindowMs;
    recording::FlowChunkStore store(source, co);
    const auto archive = source.archive("BTC-USD"); ASSERT_TRUE(archive.catalog);
    const auto grid = source.gridFor("BTC-USD", ff::Epoch, *archive.catalog);
    const auto sealed = store.buildHour("BTC-USD", ff::Epoch, grid, *archive.catalog);
    ASSERT_TRUE(sealed.section.sealed);
    const auto bars = c::toBars(c::decode(sealed.section.payload, ff::Minute, ff::Epoch, 60, false,
        uint64_t(1) << sealed.section.stepExponent));
    ASSERT_EQ(bars.size(), 1u); EXPECT_EQ(*liveBar, bars.front());
    std::cout << "midnight reconnect_ms=" << reconnect << " restart=" << restart
              << " held=3 dropped=0 live_equals_sealed=1\n";
  }
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

TEST_F(FlowLive, LegacyDailyKeepsGrowingWhenNoHourSealsPastRetention) {
  auto daily = subscribe(86'400'000);
  for (int hour = 0; hour <= 8; ++hour) {
    model->onTrade(trade(Day + hour * 3'600'000, std::to_string(hour)));
    tick(Day + hour * 3'600'000 + 2 * 60'000);
    ASSERT_FALSE(frames.empty());
    const auto bar = sentinel::flow::decodeFlowBar(bytes(frames.back()));
    EXPECT_EQ(bar.count, uint32_t(hour + 1)) << hour;
    EXPECT_EQ(bar.baseVolume, int64_t(hour + 1) * 100'000'000) << hour;
    EXPECT_FALSE(frames.back().contains("coverage"));
  }
  EXPECT_GT(FlowLiveTestAccess::retiredRevisionMinutes(*model, Day), 0u);
  const auto& minutes = FlowLiveTestAccess::series(*model, "BTC-USD", Day, 60'000).builder.bars();
  int64_t retainedVolume = 0;
  for (const auto& bar : minutes)
    if (bar.startMs >= Day + 2 * 3'600'000) retainedVolume += bar.baseVolume;
  const auto closedVolume = FlowLiveTestAccess::hour(*model, Day, Day).bar.baseVolume +
      FlowLiveTestAccess::hour(*model, Day, Day + 3'600'000).bar.baseVolume;
  EXPECT_EQ(sentinel::flow::decodeFlowBar(bytes(frames.back())).baseVolume,
            closedVolume + retainedVolume);
  daily->active.store(false);
}

TEST_F(FlowLive, EmptyBuildingCarrierIsSentOnlyToAvailabilityAwareSession) {
  model->onTrade(trade(Day + 3'600'000, "first-live"));
  tick(Day + 3'600'000 + 1'000);
  auto replay = [&](bool aware) {
    auto sub = std::make_shared<FlowSubscription>();
    sub->sub = aware ? 902 : 901; sub->symbol = "BTC-USD";
    sub->timeframeMs = 60'000;
    if (aware) std::atomic_store_explicit(&sub->sealedHours,
        std::make_shared<const std::set<int64_t>>(), std::memory_order_release);
    std::vector<Json> delivered;
    size_t count = 0;
    sub->startReplay = [&](int64_t, uint64_t n) { count = n; };
    sub->replayFrame = [&](std::string payload, bool done) {
      if (!done) delivered.push_back(Json::parse(payload));
    };
    model->subscribeFlow(sub);
    for (size_t i = 0; i <= count; ++i) model->nextFlowReplayFrame(sub);
    sub->active.store(false);
    return delivered;
  };
  const auto legacy = replay(false);
  const auto aware = replay(true);
  const auto target = Day + 59 * 60'000;
  const auto at = [&](const auto& frames) {
    return std::find_if(frames.begin(), frames.end(), [&](const Json& frame) {
      return frame.at("bucket_start_ms").template get<int64_t>() == target;
    });
  };
  EXPECT_EQ(at(legacy), legacy.end());
  for (const auto& frame : legacy)
    EXPECT_GT(sentinel::flow::decodeFlowBar(bytes(frame)).count, 0u);
  const auto built = at(aware);
  ASSERT_NE(built, aware.end());
  EXPECT_EQ((*built).at("coverage"), "building");
  EXPECT_EQ(sentinel::flow::decodeFlowBar(bytes(*built)).count, 0u);
}

TEST_F(FlowLive, MissingGridTradeIsHeldUntilAuthoritativeGrid) {
  ServerDataModel bare(config);
  FlowLiveTestAccess::stopTimer(bare);
  bare.onTrade(trade(Day + 1000, "no-grid"));
  EXPECT_EQ(FlowLiveTestAccess::missingGridDrops(bare), 0u);
  EXPECT_EQ(FlowLiveTestAccess::totalDrops(bare), 0u);
  EXPECT_EQ(FlowLiveTestAccess::heldTrades(bare), 1u);
  grid(bare);
  bare.onTrade(trade(Day + 2000, "grid"));
  EXPECT_EQ(FlowLiveTestAccess::missingGridDrops(bare), 0u);
  EXPECT_EQ(FlowLiveTestAccess::heldBytes(bare), 0u);
  EXPECT_EQ(FlowLiveTestAccess::series(bare, "BTC-USD", Day, 60000).builder.bars().front().count, 2u);
}

TEST_F(FlowLive, MissingGridHoldIsBoundedByBytesCountAndTime) {
  ServerDataModel bare(config); FlowLiveTestAccess::stopTimer(bare);
  auto large = trade(Day, std::string(1024 * 1024, 'x'));
  for (int n = 0; n < 8; ++n) FlowLiveTestAccess::add(bare, large);
  EXPECT_LT(FlowLiveTestAccess::heldBytes(bare), 4u * 1024 * 1024);
  EXPECT_EQ(FlowLiveTestAccess::heldTrades(bare), 3u);
  EXPECT_EQ(FlowLiveTestAccess::missingGridDrops(bare), 5u);
  FlowLiveTestAccess::expireHeld(bare);
  EXPECT_EQ(FlowLiveTestAccess::missingGridDrops(bare), 8u);
  EXPECT_EQ(FlowLiveTestAccess::heldBytes(bare), 0u);
  for (int n = 0; n < 8200; ++n) FlowLiveTestAccess::add(bare, trade(Day, std::to_string(n)));
  EXPECT_EQ(FlowLiveTestAccess::heldTrades(bare), 8192u);
  EXPECT_EQ(FlowLiveTestAccess::missingGridDrops(bare), 16u);
  FlowLiveTestAccess::expireHeld(bare);
  EXPECT_EQ(FlowLiveTestAccess::heldTrades(bare), 0u);
}

TEST_F(FlowLive, HeldAdmissionUsesArrivalWatermarkAndCountsLateOnlyOnce) {
  ServerDataModel bare(config); FlowLiveTestAccess::stopTimer(bare);
  bare.flowTradePosted("BTC-USD"); bare.onFlowTrade(trade(Day + 100, "on-time"), Day + 200);
  bare.flowTradePosted("BTC-USD");
  bare.onFlowTrade(trade(Day + 200, "late"), Day + 60000 + config.flow.sealWindowMs);
  grid(bare);
  const auto bars = FlowLiveTestAccess::series(bare, "BTC-USD", Day, 60000).builder.bars();
  ASSERT_EQ(bars.size(), 1u); EXPECT_EQ(bars.front().count, 1u);
  EXPECT_EQ(FlowLiveTestAccess::lateDrops(bare), 1u);
  EXPECT_EQ(FlowLiveTestAccess::missingGridDrops(bare), 0u);
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
  EXPECT_EQ(revised.bar.count, 24u);
  EXPECT_EQ(revised.revision, 0u);
  EXPECT_EQ(FlowLiveTestAccess::lateDrops(*model), 1u);
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
  std::atomic_store_explicit(&sub->sealedHours,
      std::make_shared<const std::set<int64_t>>(), std::memory_order_release);
  for (int minute = 1; minute <= 422; ++minute)
    model->onTrade(trade(Day + minute * 60'000, std::to_string(minute)));
  tick(Day + 422 * 60'000 + 1000);
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
  FlowLiveTestAccess::expireHeld(bare);
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
  FlowLiveWireTestAccess::holdWrites(*session);
  FlowLiveWireTestAccess::subscribe(
      *session,
      {{"sub", uint64_t{7}}, {"symbol", "BTC-USD"}, {"timeframe_ms", 60000}});
  ASSERT_TRUE(spinUntil(
      [&] { return FlowLiveTestAccess::subscriptions(*model) == 1; }));
  EXPECT_EQ(FlowLiveWireTestAccess::views(*session), 1u);
  model->onTrade(trade(Day + 10000, "wire"));
  FlowLiveTestAccess::tick(*model);
  EXPECT_TRUE(spinUntil([&] {
    io.restart(); io.poll();
    return RecordingServerStopTest::pendingWriteBytes(*session) > 0;
  }));
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

TEST_F(FlowLive, ReplayAdmissionUsesThresholdAndAcceptsOversizedSingleBar) {
  net::io_context io;
  ssl::context tls{ssl::context::tls_server};
  auto session = std::make_shared<Session>(tcp::socket(io), tls, *model, nullptr);
  FlowLiveWireTestAccess::holdWrites(*session);
  constexpr size_t threshold = 1024U * 1024U;
  FlowLiveWireTestAccess::seedReplay(*session, 11);
  FlowLiveWireTestAccess::submitReplay(*session, 11, std::string(600'000, 'a'));
  FlowLiveWireTestAccess::submitReplay(*session, 11, std::string(600'000, 'b'));
  EXPECT_EQ(FlowLiveWireTestAccess::replayBytes(*session, 11), 1'200'000u);
  FlowLiveWireTestAccess::submitReplay(*session, 11, std::string(100'000, 'c'));
  EXPECT_EQ(FlowLiveWireTestAccess::pendingReplayFrames(*session, 11), 2u);
  EXPECT_EQ(FlowLiveWireTestAccess::heldReplayFrameBytes(*session, 11), 100'000u);
  FlowLiveWireTestAccess::completeWrite(*session);
  EXPECT_EQ(FlowLiveWireTestAccess::replayBytes(*session, 11), 700'000u);
  EXPECT_EQ(FlowLiveWireTestAccess::pendingReplayFrames(*session, 11), 2u);
  FlowLiveWireTestAccess::unsubscribe(*session, {{"sub", uint64_t{11}}});
  FlowLiveWireTestAccess::dropCancelled(*session);
  EXPECT_EQ(RecordingServerStopTest::pendingWriteBytes(*session), 0u);

  sentinel::flow::FlowBar bar;
  bar.startMs = Day; bar.timeframeMs = 60'000; bar.nativeTick = 1;
  bar.priceScale = 100; bar.baseScale = 100'000'000; bar.quoteScale = 100'000'000;
  bar.open = bar.high = bar.low = bar.close = 1;
  constexpr int64_t each = 1'000'000'000'000;
  bar.buyVol = bar.sellVol = 65'535 * each;
  bar.baseVolume = bar.buyVol + bar.sellVol;
  bar.quoteVolume = bar.baseVolume;
  bar.maxRowVol = 2 * each;
  bar.count = 2 * 65'535;
  for (uint32_t row = 0; row < 65'535; ++row)
    bar.rows.push_back({uint16_t(row), each, each, 1, 1});
  const auto wire = sentinel::flow::encodeFlowBar(bar);
  const auto base64 = QByteArray(reinterpret_cast<const char*>(wire.data()), wire.size()).toBase64();
  const auto oversized = Json{{"type", "flow_bar"}, {"sub", 12}, {"bar", base64.toStdString()}}.dump();
  ASSERT_GT(oversized.size(), threshold);
  FlowLiveWireTestAccess::seedReplay(*session, 12);
  FlowLiveWireTestAccess::submitReplay(*session, 12, oversized);
  EXPECT_EQ(FlowLiveWireTestAccess::replayBytes(*session, 12), oversized.size());
  EXPECT_LT(FlowLiveWireTestAccess::replayBytes(*session, 12), threshold + oversized.size());
  FlowLiveWireTestAccess::submitReplay(*session, 12, std::string(50'000, 'd'));
  EXPECT_EQ(FlowLiveWireTestAccess::pendingReplayFrames(*session, 12), 1u);
  EXPECT_EQ(FlowLiveWireTestAccess::heldReplayFrameBytes(*session, 12), 50'000u);
}

TEST_F(FlowLive, ReplayCoalescesUpdatesBehindBarrierAndUnsubscribeCancelsQueue) {
  net::io_context io;
  ssl::context tls{ssl::context::tls_server};
  auto session = std::make_shared<Session>(tcp::socket(io), tls, *model, nullptr);
  FlowLiveWireTestAccess::holdWrites(*session);
  FlowLiveWireTestAccess::seedReplay(*session, 77);
  FlowLiveWireTestAccess::replayUpdate(*session, 77,
      {{"type", "flow_bar"}, {"sub", 77}, {"bucket_start_ms", Day},
       {"revision", 1}, {"complete", false}});
  FlowLiveWireTestAccess::replayUpdate(*session, 77,
      {{"type", "flow_bar"}, {"sub", 77}, {"bucket_start_ms", Day},
       {"revision", 1}, {"complete", true}}); // equal revision, complete wins
  FlowLiveWireTestAccess::replayUpdate(*session, 77,
      {{"type", "flow_bar"}, {"sub", 77}, {"bucket_start_ms", Day},
       {"revision", 2}, {"complete", true}}); // newer revision wins
  FlowLiveWireTestAccess::replayUpdate(*session, 77,
      {{"type", "flow_bar"}, {"sub", 77}, {"bucket_start_ms", Day},
       {"revision", 1}, {"complete", true}}); // lower revision loses
  FlowLiveWireTestAccess::replayUpdate(*session, 77,
      {{"type", "flow_bar"}, {"sub", 77}, {"bucket_start_ms", Day},
       {"revision", 2}, {"complete", false}}); // equal revision, incomplete loses
  FlowLiveWireTestAccess::replayUpdate(*session, 77,
      {{"type", "flow_bar"}, {"sub", 77}, {"bucket_start_ms", Day + 60'000},
       {"revision", 0}, {"complete", false}});
  EXPECT_EQ(FlowLiveWireTestAccess::coalescedUpdates(*session, 77), 2u);
  EXPECT_TRUE(FlowLiveWireTestAccess::queuedFlow(*session, 77).empty());
  FlowLiveWireTestAccess::finishReplay(*session, 77);
  const auto queued = FlowLiveWireTestAccess::queuedFlow(*session, 77);
  ASSERT_EQ(queued.size(), 3u);
  EXPECT_EQ(queued[0]["type"], "flow_live_ready");
  EXPECT_EQ(queued[0]["buckets"], 0);
  EXPECT_EQ(queued[1]["revision"], 2);
  EXPECT_EQ(queued[1]["complete"], true);
  EXPECT_EQ(queued[2]["bucket_start_ms"], Day + 60'000);
  FlowLiveWireTestAccess::unsubscribe(*session, {{"sub", uint64_t{77}}});
  FlowLiveWireTestAccess::dropCancelled(*session);
  EXPECT_EQ(RecordingServerStopTest::pendingWriteBytes(*session), 0u);
}

TEST_F(FlowLive, SealedStoreAndLiveModelShareSealMembershipAndReplayOnlyOpenHours) {
  namespace ff = flow_fixture;
  namespace chunks = sentinel::flow::chunks;
  config.chart.timeframesMs.push_back(15 * ff::Minute);
  QTemporaryDir temp;
  ASSERT_TRUE(temp.isValid());
  const auto base = std::filesystem::path(temp.path().toStdString());
  std::filesystem::create_directory(base / "raw");
  std::filesystem::create_directory(base / "hmc");
  {
    ff::Journal journal(base / "raw");
    journal.snapshot(-ff::Minute);
    journal.trades(1'000, {ff::trade(0, "first", 100000)});
    journal.heartbeat(2 * ff::Minute); // closes the first minute before its late trade
    journal.trades(6 * ff::Minute - 1, {ff::trade(200, "before-seal", 100001)});
    journal.trades(6 * ff::Minute, {ff::trade(300, "at-seal", 100002)});
    journal.trades(16 * ff::Minute + 100,
                   {ff::trade(16 * ff::Minute, "quarter-first", 100003)});
    journal.heartbeat(18 * ff::Minute);
    journal.reconnect(20 * ff::Minute);
    journal.trades(20 * ff::Minute + 1, {ff::trade(400, "resend", 100003)}, true);
    journal.trades(21 * ff::Minute + 59'000,
                   {ff::trade(16 * ff::Minute + 200, "quarter-late", 100004)});
    journal.trades(ff::Hour - 1'000,
                   {ff::trade(ff::Hour - ff::Minute + 100, "last-minute", 100004)});
    journal.trades(ff::Hour + 1'000, {ff::trade(ff::Hour + 100, "hour-one", 100004)});
    // This first record at the last minute's seal point is in the next
    // hourly RAWL2 file; its own trade must be excluded.
    journal.trades(ff::Hour + 5 * ff::Minute,
                   {ff::trade(ff::Hour - ff::Minute + 200, "last-minute-at-seal", 100005)});
    journal.trades(2 * ff::Hour + 1'000, {ff::trade(2 * ff::Hour + 100, "open", 100005)});
    journal.heartbeat(2 * ff::Hour + 5 * ff::Minute);
    // The receive clock steps back. M stays at 02:05 and this open-hour
    // minute still admits its trade.
    journal.trades(2 * ff::Hour + 4 * ff::Minute,
                   {ff::trade(2 * ff::Hour + 4 * ff::Minute, "clock-back", 100006)});
    journal.close();
  }

  ServerDataModel live(config);
  FlowLiveTestAccess::stopTimer(live);
  recording::FlowStore::Options sourceOptions;
  sourceOptions.root = base / "fld";
  sourceOptions.journalRoot = base / "raw";
  sourceOptions.hmc2Root = base / "hmc";
  sourceOptions.symbols = {"BTC-USD"};
  sourceOptions.lowerPriority = false;
  sourceOptions.nowMs = [] { return ff::Epoch + 30LL * 86'400'000; };
  int64_t sourceThrough = ff::Epoch + ff::Hour;
  sourceOptions.hourThrough = [&](const std::string&) { return sourceThrough; };
  recording::FlowStore sources(std::move(sourceOptions));
  live.attachFlowStore(&sources);
  live.onFlowGrid("BTC-USD", ff::Epoch,
                  sentinel::roller::deriveGrid(metadata(), 100000), metadata());
  sentinel::roller::JournalReader reader(base / "raw", "BTC-USD");
  sentinel::roller::JournalFeed feed("BTC-USD");
  int64_t maximum = INT64_MIN;
  feed.onTrade = [&](const Trade& value) {
    live.flowTradePosted(value.product_id);
    live.onFlowTrade(value, maximum);
  };
  sentinel::roller::JournalRecord record;
  while (reader.next(record)) {
    maximum = std::max(maximum, record.record.time.systemNs / 1'000'000);
    feed.apply(record); // same receive-before-trades ordering as the roller tap
    live.flowRecordApplied("BTC-USD", maximum);
    FlowLiveTestAccess::tick(live);
  }
  EXPECT_EQ(maximum, ff::Epoch + 2 * ff::Hour + 5 * ff::Minute);
  EXPECT_EQ(FlowLiveTestAccess::lateDrops(live), 3u);
  auto liveFirst = FlowLiveTestAccess::series(live, "BTC-USD", ff::Epoch, ff::Minute)
                       .builder.snapshotBar(ff::Epoch);
  ASSERT_TRUE(liveFirst);
  liveFirst->notable.clear();

  // Today's GUI never subscribes to chunk_availability. Its emitted 4h/1d
  // frames must stay complete and retain the sealed-hour volume.
  std::map<int64_t, std::vector<Json>> legacyFrames;
  auto legacy = [&](int64_t tf, uint64_t id) {
    auto sub = std::make_shared<FlowSubscription>();
    sub->sub = id; sub->symbol = "BTC-USD"; sub->timeframeMs = tf;
    EXPECT_FALSE(sub->sealedHours);
    sub->publish = [&, tf](std::string payload) {
      const auto frame = Json::parse(payload);
      if (frame.at("bucket_start_ms") != ff::Epoch) return;
      legacyFrames[tf].push_back(frame);
    };
    live.subscribeFlow(sub);
    return sub;
  };
  auto dailyLegacy = legacy(86'400'000, 801);
  auto fourLegacy = legacy(4 * ff::Hour, 802);
  FlowLiveTestAccess::tick(live);
  ASSERT_FALSE(legacyFrames[86'400'000].empty());
  ASSERT_FALSE(legacyFrames[4 * ff::Hour].empty());
  const auto initialDaily = legacyFrames[86'400'000].back();
  const auto initialFour = legacyFrames[4 * ff::Hour].back();
  EXPECT_TRUE(initialDaily.at("complete").get<bool>());
  EXPECT_TRUE(initialFour.at("complete").get<bool>());
  ASSERT_GT(initialDaily.at("revision").get<uint32_t>(), 0u);
  ASSERT_GT(initialFour.at("revision").get<uint32_t>(), 0u);
  FlowSubscription quarter;
  quarter.symbol = "BTC-USD"; quarter.timeframeMs = 15 * ff::Minute;
  const auto beforeQuarter = FlowLiveTestAccess::composed(live, quarter, ff::Epoch + 15 * ff::Minute);
  ASSERT_TRUE(beforeQuarter);
  ASSERT_GT(beforeQuarter->revision, 0u);
  // Stage the archive: hour 01 has its seal-point record in the 02 file.
  // Exposing that file only after the first pass makes the two hour seals
  // independent while the live model has already applied the full stream.
  const auto openHourFile = base / "raw/BTC-USD/2027/01/01/02.rawl2";
  const auto parkedHourFile = base / "02.rawl2.parked";
  ASSERT_TRUE(std::filesystem::exists(openHourFile));
  std::filesystem::rename(openHourFile, parkedHourFile);
  ASSERT_EQ(sources.runPass().failed, 0u); // FLD1 may lag FLC1 in production.

  recording::FlowChunkStore::Options options;
  options.root = base / "flc";
  options.lowerPriority = false;
  options.rules.sealWindowMs = config.flow.sealWindowMs;
  options.onIndexChange = [&](const std::string& id, std::optional<int64_t> day, uint64_t serial) {
    live.onFlowChunkIndexChange(id, day, serial);
  };
  options.onDayPublished = [&](const std::string& id, int64_t day, uint64_t serial,
                               std::vector<sentinel::flow::FlowBar> bars, std::vector<int64_t> hours) {
    live.onFlowChunkDayPublished(id, day, serial, std::move(bars), std::move(hours));
  };
  recording::FlowChunkStore store(sources, std::move(options));
  ASSERT_EQ(store.runPass().failed, 0u);
  ASSERT_TRUE(FlowLiveTestAccess::sealed(live, ff::Epoch));
  EXPECT_FALSE(FlowLiveTestAccess::sealed(live, ff::Epoch + ff::Hour));
  FlowLiveTestAccess::tick(live);
  const auto afterQuarter = FlowLiveTestAccess::composed(live, quarter, ff::Epoch + 15 * ff::Minute);
  ASSERT_TRUE(afterQuarter);
  EXPECT_EQ(afterQuarter->bar.count, beforeQuarter->bar.count);
  EXPECT_GE(afterQuarter->revision, beforeQuarter->revision);
  std::map<int64_t, uint32_t> firstSealRevisions;
  for (const auto [tf, before] : std::vector<std::pair<int64_t, Json>>{
           {86'400'000, initialDaily}, {4 * ff::Hour, initialFour}}) {
    const auto& after = legacyFrames[tf].back();
    const auto oldBar = sentinel::flow::decodeFlowBar(bytes(before));
    const auto newBar = sentinel::flow::decodeFlowBar(bytes(after));
    EXPECT_EQ(newBar.count, oldBar.count);
    EXPECT_EQ(newBar.baseVolume, oldBar.baseVolume);
    EXPECT_EQ(newBar.quoteVolume, oldBar.quoteVolume);
    EXPECT_TRUE(after.at("complete").get<bool>());
    EXPECT_FALSE(after.contains("coverage"));
    const auto revision = after.at("revision").get<uint32_t>();
    EXPECT_GE(revision, before.at("revision").get<uint32_t>());
    firstSealRevisions[tf] = revision;
  }
  sourceThrough = ff::Epoch + 2 * ff::Hour;
  std::filesystem::rename(parkedHourFile, openHourFile);
  ASSERT_EQ(sources.runPass().failed, 0u);
  ASSERT_EQ(store.runPass().failed, 0u);
  ASSERT_TRUE(FlowLiveTestAccess::sealed(live, ff::Epoch + ff::Hour));
  EXPECT_FALSE(FlowLiveTestAccess::sealed(live, ff::Epoch + 2 * ff::Hour));
  EXPECT_EQ(FlowLiveTestAccess::sealMismatches(live), 0u);
  FlowLiveTestAccess::tick(live);
  EXPECT_GE(FlowLiveTestAccess::retiredRevisionMinutes(live, ff::Epoch), 4u);
  for (const auto [tf, before] : std::vector<std::pair<int64_t, Json>>{
           {86'400'000, initialDaily}, {4 * ff::Hour, initialFour}}) {
    ASSERT_GT(legacyFrames[tf].size(), 1u);
    const auto& after = legacyFrames[tf].back();
    const auto oldBar = sentinel::flow::decodeFlowBar(bytes(before));
    const auto newBar = sentinel::flow::decodeFlowBar(bytes(after));
    EXPECT_EQ(newBar.count, oldBar.count);
    EXPECT_EQ(newBar.baseVolume, oldBar.baseVolume);
    EXPECT_EQ(newBar.quoteVolume, oldBar.quoteVolume);
    EXPECT_GE(after.at("revision").get<uint32_t>(), before.at("revision").get<uint32_t>());
    EXPECT_GE(after.at("revision").get<uint32_t>(), firstSealRevisions.at(tf));
    EXPECT_TRUE(after.at("complete").get<bool>());
    EXPECT_FALSE(after.contains("coverage"));
  }
  const auto sealedDailyRevision = legacyFrames[86'400'000].back().at("revision").get<uint32_t>();
  const auto sealedFourRevision = legacyFrames[4 * ff::Hour].back().at("revision").get<uint32_t>();
  live.onFeedWatermark("BTC-USD", maximum + 1'000);
  FlowLiveTestAccess::tick(live); // close the still-revisable 02:04 minute
  live.flowTradePosted("BTC-USD");
  live.onFlowTrade(trade(ff::Epoch + 2 * ff::Hour + 4 * ff::Minute + 100,
                         "post-seal-late"), maximum + 1'000);
  FlowLiveTestAccess::tick(live);
  EXPECT_GT(legacyFrames[86'400'000].back().at("revision").get<uint32_t>(), sealedDailyRevision);
  EXPECT_GT(legacyFrames[4 * ff::Hour].back().at("revision").get<uint32_t>(), sealedFourRevision);
  EXPECT_EQ(sentinel::flow::decodeFlowBar(bytes(legacyFrames[86'400'000].back())).count, 9u);
  dailyLegacy->active.store(false);
  fourLegacy->active.store(false);
  const auto firstPayload = store.read("coinbase:BTC-USD", ff::Minute, ff::Epoch);
  ASSERT_TRUE(firstPayload);
  const auto sealedFirst = chunks::toBars(chunks::decode(firstPayload->bytes, ff::Minute,
      ff::Epoch, 60, false, uint64_t(1) << firstPayload->stepExponent));
  ASSERT_FALSE(sealedFirst.empty());
  EXPECT_EQ(*liveFirst, sealedFirst.front());
  FlowLiveTestAccess::tick(live); // disk-sealed RAM minutes are pruned
  EXPECT_FALSE(FlowLiveTestAccess::series(live, "BTC-USD", ff::Epoch, ff::Minute)
                   .builder.snapshotBar(ff::Epoch));

  const auto availability = store.snapshot("coinbase:BTC-USD");
  ASSERT_TRUE(availability);
  auto sealedHours = std::make_shared<std::set<int64_t>>();
  for (const auto& range : availability->levels[0])
    for (auto hour = range.fromMs; hour < range.toMs; hour += ff::Hour)
      sealedHours->insert(hour);
  EXPECT_TRUE(sealedHours->contains(ff::Epoch));
  EXPECT_TRUE(sealedHours->contains(ff::Epoch + ff::Hour));
  EXPECT_FALSE(sealedHours->contains(ff::Epoch + 2 * ff::Hour));
  for (const auto [tf, expected] : std::vector<std::pair<int64_t, size_t>>{
           {ff::Minute, 6}, {5 * ff::Minute, 2}, {ff::Hour, 1}, {86'400'000, 1}}) {
    auto sub = std::make_shared<FlowSubscription>();
    sub->sub = uint64_t(tf); sub->symbol = "BTC-USD"; sub->timeframeMs = tf;
    std::atomic_store_explicit(&sub->sealedHours,
        std::shared_ptr<const std::set<int64_t>>(sealedHours), std::memory_order_release);
    size_t announced = SIZE_MAX, delivered = 0; bool ready = false;
    sub->startReplay = [&](int64_t, uint64_t count) { announced = count; };
    sub->replayFrame = [&](std::string payload, bool done) {
      if (done) ready = true;
      else {
        const auto frame = Json::parse(payload);
        EXPECT_EQ(frame["bucket_start_ms"].get<int64_t>() / tf * tf,
                  frame["bucket_start_ms"].get<int64_t>());
        ++delivered;
      }
    };
    live.subscribeFlow(sub);
    ASSERT_EQ(announced, expected) << tf;
    for (size_t i = 0; i <= announced; ++i) live.nextFlowReplayFrame(sub);
    EXPECT_TRUE(ready); EXPECT_EQ(delivered, expected);
    if (tf == 86'400'000) {
      const auto daily = FlowLiveTestAccess::composed(live, *sub, ff::Epoch);
      ASSERT_TRUE(daily);
      EXPECT_EQ(daily->bar.count, 9u);
      EXPECT_FALSE(daily->building); // the observed forming minute is covered
      const auto hourOnePayload = store.read("coinbase:BTC-USD", ff::Minute, ff::Epoch + ff::Hour);
      ASSERT_TRUE(hourOnePayload);
      auto parts = sealedFirst;
      auto hourOne = chunks::toBars(chunks::decode(hourOnePayload->bytes, ff::Minute,
          ff::Epoch + ff::Hour, 60, false, uint64_t(1) << hourOnePayload->stepExponent));
      parts.insert(parts.end(), hourOne.begin(), hourOne.end());
      auto open = FlowLiveTestAccess::series(live, "BTC-USD", ff::Epoch, ff::Minute).builder.bars();
      parts.insert(parts.end(), open.begin(), open.end());
      EXPECT_EQ(daily->bar, sentinel::flow::composeFlowBars(parts, 86'400'000));
      // Composition uses the model's decoded sealed bars even if this
      // session has not adopted the new availability yet.
      FlowSubscription hour;
      hour.symbol = "BTC-USD"; hour.timeframeMs = ff::Hour;
      std::atomic_store_explicit(&hour.sealedHours,
          std::shared_ptr<const std::set<int64_t>>(sealedHours), std::memory_order_release);
      const auto covered = FlowLiveTestAccess::composed(live, hour, ff::Epoch + ff::Hour);
      ASSERT_TRUE(covered);
      EXPECT_FALSE(covered->building);
      sealedHours->erase(ff::Epoch + ff::Hour);
      const auto hole = FlowLiveTestAccess::composed(live, *sub, ff::Epoch);
      ASSERT_TRUE(hole);
      EXPECT_FALSE(hole->building);
      const auto hourHole = FlowLiveTestAccess::composed(live, hour, ff::Epoch + ff::Hour);
      ASSERT_TRUE(hourHole);
      EXPECT_FALSE(hourHole->building);
      sealedHours->insert(ff::Epoch + ff::Hour);
    }
    sub->active.store(false);
  }
  // A fresh process loads the already sealed hours before its first live
  // trade. The open hour remains live-owned; older unsealed gaps are building.
  ServerDataModel restarted(config);
  FlowLiveTestAccess::stopTimer(restarted);
  recording::FlowStore::Options restartSourcesOptions;
  restartSourcesOptions.root = base / "fld";
  restartSourcesOptions.journalRoot = base / "raw";
  restartSourcesOptions.hmc2Root = base / "hmc";
  restartSourcesOptions.symbols = {"BTC-USD"};
  restartSourcesOptions.lowerPriority = false;
  restartSourcesOptions.nowMs = [] { return ff::Epoch + 30LL * 86'400'000; };
  restartSourcesOptions.hourThrough = [](const std::string&) { return ff::Epoch + 2 * ff::Hour; };
  recording::FlowStore restartSources(std::move(restartSourcesOptions));
  restarted.attachFlowStore(&restartSources);
  restarted.onFlowGrid("BTC-USD", ff::Epoch,
                       sentinel::roller::deriveGrid(metadata(), 100000), metadata());
  recording::FlowChunkStore::Options restartOptions;
  restartOptions.root = base / "flc";
  restartOptions.lowerPriority = false;
  restartOptions.rules.sealWindowMs = config.flow.sealWindowMs;
  restartOptions.onIndexChange = [&](const std::string& id, std::optional<int64_t> day, uint64_t serial) {
    restarted.onFlowChunkIndexChange(id, day, serial);
  };
  restartOptions.onDayPublished = [&](const std::string& id, int64_t day, uint64_t serial,
                                      std::vector<sentinel::flow::FlowBar> bars, std::vector<int64_t> hours) {
    restarted.onFlowChunkDayPublished(id, day, serial, std::move(bars), std::move(hours));
  };
  recording::FlowChunkStore reopened(restartSources, std::move(restartOptions));
  reopened.initialize();
  ASSERT_TRUE(FlowLiveTestAccess::sealed(restarted, ff::Epoch));
  ASSERT_TRUE(FlowLiveTestAccess::sealed(restarted, ff::Epoch + ff::Hour));
  restarted.flowTradePosted("BTC-USD");
  restarted.onFlowTrade(trade(ff::Epoch + 2 * ff::Hour + 4 * ff::Minute, "restart"),
                        ff::Epoch + 2 * ff::Hour + 5 * ff::Minute);
  FlowLiveTestAccess::tick(restarted);
  auto afterRestart = std::make_shared<FlowSubscription>();
  afterRestart->sub = 990; afterRestart->symbol = "BTC-USD"; afterRestart->timeframeMs = ff::Minute;
  std::atomic_store_explicit(&afterRestart->sealedHours,
      std::shared_ptr<const std::set<int64_t>>(sealedHours), std::memory_order_release);
  size_t restartedBuckets = SIZE_MAX;
  afterRestart->startReplay = [&](int64_t, uint64_t count) { restartedBuckets = count; };
  restarted.subscribeFlow(afterRestart);
  EXPECT_EQ(restartedBuckets, 6u);
  // A store epoch may clear decoded bars before its replacement day arrives.
  // The old client composes retained closed hours; a new client sees a hole.
  live.onFlowChunkIndexChange("coinbase:BTC-USD", ff::Epoch, 1'000'000);
  const auto legacyGap = FlowLiveTestAccess::composed(live, *dailyLegacy, ff::Epoch);
  ASSERT_TRUE(legacyGap);
  EXPECT_EQ(legacyGap->bar.count, 9u);
  EXPECT_FALSE(legacyGap->building);
  FlowSubscription invalidatedDay;
  invalidatedDay.symbol = "BTC-USD"; invalidatedDay.timeframeMs = 86'400'000;
  std::atomic_store_explicit(&invalidatedDay.sealedHours,
      std::shared_ptr<const std::set<int64_t>>(sealedHours), std::memory_order_release);
  const auto gap = FlowLiveTestAccess::composed(live, invalidatedDay, ff::Epoch);
  ASSERT_TRUE(gap);
  EXPECT_TRUE(gap->building);
  EXPECT_FALSE(gap->complete);
  restarted.attachFlowStore(nullptr);
  live.attachFlowStore(nullptr);
}

TEST_F(FlowLive, SealedHourNotificationCountsEachDifferentLiveFinalMinute) {
  model->onTrade(trade(Day + 100, "warmup")); // partly observed, excluded from comparison
  model->onTrade(trade(Day + 60'100, "live"));
  model->onTrade(trade(Day + 120'100, "live-second"));
  tick(Day + 8 * 60'000);
  auto different = FlowLiveTestAccess::series(*model, "BTC-USD", Day, 60'000)
                       .builder.snapshotBar(Day + 60'000);
  ASSERT_TRUE(different);
  auto second = FlowLiveTestAccess::series(*model, "BTC-USD", Day, 60'000)
                    .builder.snapshotBar(Day + 120'000);
  ASSERT_TRUE(second);
  different->count += 1;
  different->notable.clear();
  second->count += 1;
  second->notable.clear();
  model->onFlowChunkIndexChange("coinbase:BTC-USD", Day, 1);
  model->onFlowChunkDayPublished("coinbase:BTC-USD", Day, 1,
                                 {*different, *second}, {Day});
  EXPECT_EQ(FlowLiveTestAccess::sealMismatches(*model), 2u);
  sentinel::metrics::MetricsRegistry metrics;
  model->registerMetrics(metrics);
  EXPECT_NE(metrics.render().find("sentinel_flow_live_seal_mismatch_total{instrument=\"coinbase:BTC-USD\"} 2"),
            std::string::npos);
  FlowLiveTestAccess::tick(*model);
  EXPECT_EQ(FlowLiveTestAccess::sealMismatches(*model), 2u);
}

TEST_F(FlowLive, ReorderedRunsCountTheDifferentLiveFinalAgainstSealedArchive) {
  namespace ff = flow_fixture;
  QTemporaryDir temp;
  ASSERT_TRUE(temp.isValid());
  const auto base = std::filesystem::path(temp.path().toStdString());
  std::filesystem::create_directory(base / "raw");
  std::filesystem::create_directory(base / "hmc");
  // Capture sees the first run through its seal point. A restarted run then
  // arrives with an earlier receive clock, so live rejects its old trade.
  {
    ff::Journal first(base / "raw");
    first.snapshot(-20 * ff::Minute);
    first.trades(ff::Minute, {ff::trade(0, "first-run", 100000)});
    first.heartbeat(6 * ff::Minute);
    first.close();
  }
  {
    ff::Journal second(base / "raw");
    second.snapshot(-30 * ff::Minute);
    second.trades(ff::Minute, {ff::trade(200, "second-run", 100001)});
    second.heartbeat(ff::Hour + 5 * ff::Minute);
    second.close();
  }
  // Fixture writer stamps host start time. Make the headers follow the
  // simulated receive clock, as the real catalog does for these two runs.
  sentinel::roller::JournalReader catalog(base / "raw", "BTC-USD");
  std::map<std::string, int64_t> created, starts;
  for (const auto& file : catalog.files()) {
    created.try_emplace(file.run, file.runStartedNs);
    starts.try_emplace(file.run, file.openedNs);
  }
  for (const auto& file : catalog.files()) {
    std::ifstream input(file.path, std::ios::binary);
    std::vector<uint8_t> raw{std::istreambuf_iterator<char>(input), {}};
    ASSERT_GE(raw.size(), 16u);
    uint32_t length = 0;
    for (unsigned i = 0; i < 4; ++i) length |= uint32_t(raw[8 + i]) << (8 * i);
    ASSERT_GE(raw.size(), 16u + length);
    auto header = Json::parse(raw.begin() + 16, raw.begin() + 16 + length);
    header["run_started_system_ns"] = starts.at(file.run);
    const auto text = header.dump();
    ASSERT_EQ(text.size(), length);
    std::copy(text.begin(), text.end(), raw.begin() + 16);
    const auto crc = sentinel::crc32(text.data(), text.size());
    for (unsigned i = 0; i < 4; ++i) raw[12 + i] = uint8_t(crc >> (8 * i));
    std::ofstream output(file.path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(raw.data()), raw.size());
  }
  sentinel::roller::JournalReader reader(base / "raw", "BTC-USD");
  std::vector<sentinel::roller::JournalRecord> records;
  sentinel::roller::JournalRecord record;
  while (reader.next(record)) records.push_back(std::move(record));
  ASSERT_EQ(created.size(), 2u);
  // Catalog order is restarted run first. Process the same records in actual
  // capture arrival order for the live path.
  ASSERT_GT(created.at(records.front().pos.run), created.at(records.back().pos.run));
  std::stable_sort(records.begin(), records.end(), [&](const auto& a, const auto& b) {
    return created.at(a.pos.run) < created.at(b.pos.run);
  });

  ServerDataModel live(config);
  FlowLiveTestAccess::stopTimer(live);
  live.onFlowGrid("BTC-USD", ff::Epoch,
                  sentinel::roller::deriveGrid(metadata(), 100000), metadata());
  sentinel::roller::JournalFeed feed("BTC-USD");
  int64_t maximum = INT64_MIN;
  feed.onTrade = [&](const Trade& value) {
    live.flowTradePosted(value.product_id);
    live.onFlowTrade(value, maximum);
  };
  for (const auto& item : records) {
    maximum = std::max(maximum, item.record.time.systemNs / 1'000'000);
    feed.apply(item);
    live.flowRecordApplied("BTC-USD", maximum);
  }
  EXPECT_EQ(FlowLiveTestAccess::lateDrops(live), 1u);
  auto final = FlowLiveTestAccess::series(live, "BTC-USD", ff::Epoch, ff::Minute)
                   .builder.snapshotBar(ff::Epoch);
  ASSERT_TRUE(final);
  EXPECT_EQ(final->count, 1u);

  recording::FlowStore::Options sourceOptions;
  sourceOptions.root = base / "fld";
  sourceOptions.journalRoot = base / "raw";
  sourceOptions.hmc2Root = base / "hmc";
  sourceOptions.symbols = {"BTC-USD"};
  sourceOptions.lowerPriority = false;
  sourceOptions.nowMs = [] { return ff::Epoch + 30LL * 86'400'000; };
  sourceOptions.hourThrough = [](const std::string&) { return ff::Epoch + ff::Hour; };
  recording::FlowStore sources(std::move(sourceOptions));
  recording::FlowChunkStore::Options options;
  options.root = base / "flc";
  options.lowerPriority = false;
  options.rules.sealWindowMs = config.flow.sealWindowMs;
  options.onIndexChange = [&](const std::string& id, std::optional<int64_t> day, uint64_t serial) {
    live.onFlowChunkIndexChange(id, day, serial);
  };
  options.onDayPublished = [&](const std::string& id, int64_t day, uint64_t serial,
                               std::vector<sentinel::flow::FlowBar> bars, std::vector<int64_t> hours) {
    live.onFlowChunkDayPublished(id, day, serial, std::move(bars), std::move(hours));
  };
  recording::FlowChunkStore store(sources, std::move(options));
  ASSERT_EQ(store.runPass().failed, 0u);
  ASSERT_TRUE(FlowLiveTestAccess::sealed(live, ff::Epoch));
  EXPECT_EQ(FlowLiveTestAccess::sealMismatches(live), 1u);
}

TEST_F(FlowLive, PendingTradeGateWaitsForPostedTradesBeforeReadingRecordMaximum) {
  model->flowTradePosted("BTC-USD");
  model->flowRecordApplied("BTC-USD", Day + 7 * 60'000);
  FlowLiveTestAccess::tick(*model);
  EXPECT_EQ(FlowLiveTestAccess::receiveMax(*model), INT64_MIN);
  model->onFlowTrade(trade(Day, "pending"), Day);
  EXPECT_EQ(FlowLiveTestAccess::receiveMax(*model), Day);
  FlowLiveTestAccess::tick(*model);
  EXPECT_EQ(FlowLiveTestAccess::receiveMax(*model), Day + 7 * 60'000);
}

TEST_F(FlowLive, NoStoreStillRetainsSixHoursOfMinuteSlots) {
  auto sub = subscribe();
  auto& series = FlowLiveTestAccess::series(*model, "BTC-USD", Day, 60'000);
  ASSERT_EQ(series.bucketSlots.size(), 422u);
  for (int i = 0; i < 130; ++i) {
    model->onTrade(trade(Day + i * 60'000, std::to_string(i)));
    tick(Day + i * 60'000 + 1'000);
  }
  EXPECT_EQ(series.capacityDrops, 0u);
  EXPECT_EQ(series.builder.bars().size(), 130u);
  sub->active.store(false);
}

TEST_F(FlowLive, FutureExchangeBucketPublishesAfterReceiveMaximumCatchesUp) {
  auto sub = subscribe();
  model->flowTradePosted("BTC-USD");
  model->onFlowTrade(trade(Day + 2 * 60'000, "future"), Day + 60'000);
  FlowLiveTestAccess::tick(*model);
  EXPECT_TRUE(frames.empty());
  model->flowRecordApplied("BTC-USD", Day + 2 * 60'000);
  FlowLiveTestAccess::tick(*model);
  ASSERT_EQ(frames.size(), 1u);
  EXPECT_EQ(frames[0]["bucket_start_ms"], Day + 2 * 60'000);
  sub->active.store(false);
}

TEST_F(FlowLive, CurrentObservedFormingMinuteDoesNotMakeCoarseBucketBuilding) {
  model->onTrade(trade(Day, "first"));
  tick(Day + 60'000);
  FlowSubscription sub;
  sub.symbol = "BTC-USD"; sub.timeframeMs = 300'000;
  const auto composed = FlowLiveTestAccess::composed(*model, sub, Day);
  ASSERT_TRUE(composed);
  EXPECT_EQ(composed->bar.count, 1u);
  EXPECT_FALSE(composed->building);
  EXPECT_TRUE(composed->complete);
}

TEST_F(FlowLive, AvailabilityAheadOfDecodedSealMarksMissingHourBuilding) {
  model->onTrade(trade(Day, "old"));
  model->onTrade(trade(Day + 60 * 60'000, "open"));
  tick(Day + 61 * 60'000);
  FlowLiveTestAccess::discardMinute(*model, Day, Day);
  FlowSubscription sub;
  sub.symbol = "BTC-USD"; sub.timeframeMs = 4 * 3'600'000;
  auto sealed = std::make_shared<const std::set<int64_t>>(std::set<int64_t>{Day});
  std::atomic_store_explicit(&sub.sealedHours, sealed, std::memory_order_release);
  const auto composed = FlowLiveTestAccess::composed(*model, sub, Day);
  ASSERT_TRUE(composed);
  EXPECT_EQ(composed->bar.count, 1u);
  EXPECT_TRUE(composed->building);
  EXPECT_FALSE(composed->complete);
}

TEST_F(FlowLive, DiskSealedHourDoesNotAcceptAnotherLiveMinuteTrade) {
  model->onTrade(trade(Day, "first"));
  tick(Day + 60'000);
  const auto before = FlowLiveTestAccess::series(*model, "BTC-USD", Day, 60'000)
                          .builder.snapshotBar(Day);
  ASSERT_TRUE(before);
  model->onFlowChunkIndexChange("coinbase:BTC-USD", Day, 1);
  model->onFlowChunkDayPublished("coinbase:BTC-USD", Day, 1, {*before}, {Day});
  model->flowTradePosted("BTC-USD");
  model->onFlowTrade(trade(Day + 1'000, "after-disk-seal"), Day + 61'000);
  const auto after = FlowLiveTestAccess::series(*model, "BTC-USD", Day, 60'000)
                         .builder.snapshotBar(Day);
  ASSERT_TRUE(after);
  EXPECT_EQ(after->count, before->count);
  EXPECT_EQ(FlowLiveTestAccess::lateDrops(*model), 0u);
}

TEST_F(FlowLive, Fld1MustSealBeforeFlc1PrunesLiveMinutes) {
  QTemporaryDir temp;
  ASSERT_TRUE(temp.isValid());
  const auto base = std::filesystem::path(temp.path().toStdString());
  std::filesystem::create_directory(base / "raw");
  std::filesystem::create_directory(base / "hmc");
  recording::FlowStore::Options options;
  options.root = base / "fld"; options.journalRoot = base / "raw";
  options.hmc2Root = base / "hmc"; options.symbols = {"BTC-USD"};
  options.lowerPriority = false;
  options.hourThrough = [](const std::string&) { return Day; };
  recording::FlowStore store(std::move(options));
  model->attachFlowStore(&store);
  model->onTrade(trade(Day, "first"));
  tick(Day + 7 * 60'000);
  const auto before = FlowLiveTestAccess::series(*model, "BTC-USD", Day, 60'000)
                          .builder.snapshotBar(Day);
  ASSERT_TRUE(before);
  model->onFlowChunkIndexChange("coinbase:BTC-USD", Day, 1);
  model->onFlowChunkDayPublished("coinbase:BTC-USD", Day, 1, {*before}, {Day});
  tick(Day + 8 * 60'000);
  EXPECT_TRUE(FlowLiveTestAccess::series(*model, "BTC-USD", Day, 60'000)
                  .builder.snapshotBar(Day));
  model->attachFlowStore(nullptr);
}

TEST_F(FlowLive, IncompatibleSealedAndLiveGridsBecomeBuildingWithoutThrowing) {
  model->onTrade(trade(Day, "sealed"));
  model->onTrade(trade(Day + 60 * 60'000, "open"));
  tick(Day + 61 * 60'000);
  auto disk = FlowLiveTestAccess::series(*model, "BTC-USD", Day, 60'000)
                  .builder.snapshotBar(Day);
  ASSERT_TRUE(disk);
  disk->baseScale *= 10;
  model->onFlowChunkIndexChange("coinbase:BTC-USD", Day, 1);
  model->onFlowChunkDayPublished("coinbase:BTC-USD", Day, 1, {*disk}, {Day});
  FlowSubscription sub;
  sub.symbol = "BTC-USD"; sub.timeframeMs = 4 * 3'600'000;
  auto sealed = std::make_shared<const std::set<int64_t>>(std::set<int64_t>{Day});
  std::atomic_store_explicit(&sub.sealedHours, sealed, std::memory_order_release);
  const auto composed = FlowLiveTestAccess::composed(*model, sub, Day);
  ASSERT_TRUE(composed);
  EXPECT_TRUE(composed->building);
  EXPECT_FALSE(composed->complete);
  EXPECT_EQ(composed->bar.count, 0u);
}

} // namespace
