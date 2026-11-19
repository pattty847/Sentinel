// Direct-recorder bounded policy regressions; included by test_book_recorder.cpp.
#include <set>

namespace {
RecorderConfig boundedConfig(const std::filesystem::path& root, int64_t scale = 100) {
    RecorderConfig c{root, double(scale), {}, {{"near", 1, 0, 0, false, AbsoluteRowBounds{0, scale}}}, 0};
    c.priceBounds = PriceBounds{0, 1};
    return c;
}
std::vector<uint8_t> recorderBytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    EXPECT_TRUE(in.is_open()) << path;
    return {std::istreambuf_iterator<char>(in), {}};
}
}

TEST_F(RecorderTest, AbsoluteWindowCoversEveryRowAtLowMiddleAndHighMids) {
    for (const int64_t scale : {100, 1000}) for (const double mid : {.02, .50, .98}) {
        SCOPED_TRACE(std::to_string(scale) + "/" + std::to_string(mid));
        QTemporaryDir own;
        auto c = boundedConfig(own.path().toStdString(), scale);
        std::vector<std::shared_ptr<const Hmc2Record>> pubs;
        c.publication = RecorderConfig::Publication::Lead;
        c.publisher = [&](auto p) { pubs.push_back(std::move(p)); };
        auto r = make(c);
        const auto pivot = std::llround(mid * scale);
        std::vector<Level> levels;
        // The exact midpoint lies in the spread. Every other grid row rests.
        for (int64_t row = 0; row <= scale; ++row)
            if (row != pivot) levels.push_back({row < pivot, double(row) / scale, 2});
        snap(*r, 0, levels);
        tick(*r, 10000);
        ASSERT_FALSE(pubs.empty());
        EXPECT_DOUBLE_EQ(pubs.back()->midOpen, mid);
        EXPECT_DOUBLE_EQ(pubs.back()->midClose, mid);
        EXPECT_EQ(pubs.back()->bidRowLo, 0);
        EXPECT_EQ(pubs.back()->askRowHi, scale);
        EXPECT_EQ(pubs.back()->entries.size(), scale);
        // Fill the last row legally; the mid moves half a tick, the window does not.
        update(*r, 30000, {{false, double(pivot) / scale, 2}});
        tick(*r, 60000);
        const auto& column = *pubs.back();
        EXPECT_EQ(column.observedMs, 60000);
        EXPECT_EQ(column.bidRowLo, 0); EXPECT_EQ(column.askRowLo, 0);
        EXPECT_EQ(column.bidRowHi, scale); EXPECT_EQ(column.askRowHi, scale);
        ASSERT_EQ(column.entries.size(), scale + 1);
        for (int64_t row = 0; row <= scale; ++row)
            value(column, row, row >= pivot, row == pivot ? 1 : 2, 2);
        EXPECT_EQ(r->stats().invalidations, 0);
    }
}

TEST_F(RecorderTest, BoundedOneSidedAndEmptySnapshotsObserveWithoutSuspectWait) {
    const std::vector<std::vector<Level>> books{{{true,.99,2}}, {{false,.01,4}}, {}};
    const double mids[]{.995,.005,.5};
    for (size_t i = 0; i < books.size(); ++i) {
        QTemporaryDir own;
        auto c = boundedConfig(own.path().toStdString());
        auto r = make(c);
        snap(*r, 0, books[i]);
        tick(*r, c.oneSidedSuspectMs + 1);
        EXPECT_TRUE(r->snapshotWait("BTC-USD").reason.empty());
        tick(*r, 120000);
        EXPECT_TRUE(r->snapshotWait("BTC-USD").reason.empty());
        EXPECT_EQ(r->stats().invalidations, 0);
        const auto rows = Hmc2Store::readRange(c.root,"BTC-USD","near",60000,kEpoch,kEpoch+120000);
        ASSERT_EQ(rows.size(), 2);
        for (const auto& row : rows) {
            EXPECT_EQ(row.observedMs, 60000);
            EXPECT_FALSE(row.flags & kPartial);
            EXPECT_DOUBLE_EQ(row.midOpen, mids[i]); EXPECT_DOUBLE_EQ(row.midClose, mids[i]);
            EXPECT_DOUBLE_EQ(row.midMin, mids[i]); EXPECT_DOUBLE_EQ(row.midMax, mids[i]);
            if (i == 0) value(row,99,false,2,2);
            else if (i == 1) value(row,1,true,4,4);
            else EXPECT_TRUE(row.entries.empty());
        }
    }
}

TEST_F(RecorderTest, BoundedTransitionsIntegrateAllThreeIntervals) {
    auto c = boundedConfig(dir.path().toStdString());
    auto r = make(c);
    snap(*r,0,{{true,.4,2},{false,.6,4}});
    update(*r,20000,{{false,.6,0},{true,.4,6}});
    update(*r,40000,{{false,.6,8},{true,.4,10}});
    tick(*r,60000);
    const auto rows = read(); ASSERT_EQ(rows.size(),1);
    EXPECT_EQ(rows[0].observedMs,60000);
    // Bid: (2*20s + 6*20s + 10*20s)/60s = 6; ask: (4+0+8)/3 = 4.
    value(rows[0],40,false,6,10); value(rows[0],60,true,4,8);
    EXPECT_DOUBLE_EQ(rows[0].midOpen,.5); EXPECT_DOUBLE_EQ(rows[0].midClose,.5);
    EXPECT_DOUBLE_EQ(rows[0].midMin,.5); EXPECT_DOUBLE_EQ(rows[0].midMax,.7);
    EXPECT_TRUE(r->snapshotWait("BTC-USD").reason.empty());
}

TEST_F(RecorderTest, BoundedEndpointMidsKeepZeroOpenAndMinimum) {
    auto c = boundedConfig(dir.path().toStdString());
    auto r = make(c);
    snap(*r,0,{{false,0,2}}); // missing bid = 0; synthetic equality is not a cross
    update(*r,20000,{{false,0,0},{true,1,4}});
    update(*r,40000,{{true,1,0}});
    tick(*r,60000);
    const auto rows = read(); ASSERT_EQ(rows.size(),1);
    EXPECT_EQ(rows[0].observedMs,60000);
    EXPECT_DOUBLE_EQ(rows[0].midOpen,0); EXPECT_DOUBLE_EQ(rows[0].midMin,0);
    EXPECT_DOUBLE_EQ(rows[0].midMax,1); EXPECT_DOUBLE_EQ(rows[0].midClose,.5);
    value(rows[0],0,true,2./3,2); value(rows[0],100,false,4./3,4);
}

TEST_F(RecorderTest, BoundedCrossedUpdatesSuspendAndCrossedSnapshotsInvalidate) {
    for (const double bid : {.6,.7}) {
        auto r = make(boundedConfig(dir.path().toStdString()));
        snap(*r,0,{{true,.4,2},{false,.6,4}});
        update(*r,10000,{{true,bid,500}});
        tick(*r,39999); EXPECT_TRUE(r->snapshotWait("BTC-USD").reason.empty());
        tick(*r,40000); EXPECT_EQ(waitOf(*r,"BTC-USD"),"recorder:unobserved suspect");
        update(*r,41000,{{true,bid,0}});
        EXPECT_TRUE(r->snapshotWait("BTC-USD").reason.empty());
        tick(*r,60000);
        const auto rows = read(); ASSERT_EQ(rows.size(),1);
        EXPECT_EQ(rows[0].observedMs,29000);
        ASSERT_EQ(rows[0].entries.size(),2);
        value(rows[0],40,false,2,2); value(rows[0],60,true,4,4);
        EXPECT_EQ(r->stats().invalidations,0);
        snap(*r,60000,{{true,bid,2},{false,.6,4}});
        EXPECT_EQ(waitOf(*r,"BTC-USD"),"recorder:unrepresentable or crossed mid");
        EXPECT_EQ(r->stats().invalidations,1);
    }
}

TEST_F(RecorderTest, BoundedOutOfRangeBatchInvalidatesAtomicallyUntilSnapshot) {
    for (const double price : {-.01, 1.01, -.00001, 1.00001, std::numeric_limits<double>::infinity()}) {
        auto r = make(boundedConfig(dir.path().toStdString()));
        snap(*r,0,{{true,.4,2},{false,.6,4}});
        update(*r,20000,{{true,.4,400},{false,price,1}});
        EXPECT_EQ(waitOf(*r,"BTC-USD"),"recorder:malformed L2");
        update(*r,30000,{{false,.6,8}}); // ignored without continuity
        tick(*r,60000);
        const auto rows = read(); ASSERT_EQ(rows.size(),1);
        EXPECT_EQ(rows[0].observedMs,20000);
        value(rows[0],40,false,2,2); value(rows[0],60,true,4,4);
        snap(*r,60000,{});
        EXPECT_TRUE(r->snapshotWait("BTC-USD").reason.empty());
    }
}

TEST_F(RecorderTest, UnboundedEmptyBooksRetainThirtySecondSuspectAndSnapshotRejection) {
    auto r = make(config()); snap(*r,0);
    update(*r,10000,{{true,99,0},{false,101,0}});
    tick(*r,39999); EXPECT_TRUE(r->snapshotWait("BTC-USD").reason.empty());
    tick(*r,40000); EXPECT_EQ(waitOf(*r,"BTC-USD"),"recorder:unobserved suspect");
    tick(*r,120000);
    const auto rows = read(); ASSERT_EQ(rows.size(),1);
    EXPECT_EQ(rows[0].observedMs,10000);
    snap(*r,120000,{});
    EXPECT_EQ(waitOf(*r,"BTC-USD"),"recorder:missing two-sided mid");
    snap(*r,120000,{{false,101,4}});
    EXPECT_EQ(waitOf(*r,"BTC-USD"),"recorder:missing two-sided mid");
    snap(*r,120000,{{true,102,2},{false,101,4}});
    EXPECT_EQ(waitOf(*r,"BTC-USD"),"recorder:unrepresentable or crossed mid");
}

TEST_F(RecorderTest, BoundedPolicyValidationRejectsAmbiguityAndInvalidBounds) {
    auto c = boundedConfig(dir.path().toStdString());
    for (const auto bounds : {PriceBounds{-1,1}, {1,1}, {1,0}, {0,.001}, {0,1e300}, {0,NAN}}) {
        auto bad = c; bad.priceBounds = bounds;
        EXPECT_THROW(make(bad),std::invalid_argument);
    }
    for (const auto bounds : {AbsoluteRowBounds{-1,100}, {100,0}}) {
        auto bad = c; bad.layers[0].absoluteRows = bounds;
        EXPECT_THROW(make(bad),std::invalid_argument);
    }
    c.layers[0].lowFrac = .95; c.layers[0].highMult = 1.05;
    EXPECT_THROW(make(c),std::invalid_argument);
    c.layers[0].absoluteRows.reset();
    EXPECT_NO_THROW(make(c)); // bounded mid and relative window are independent policies
}

TEST_F(RecorderTest, PolicyHashChangesAlwaysStartNewGenerationInBothDirections) {
    auto legacy = config(); legacy.layers[0] = {"near",1,.5,2,false};
    auto absolute = legacy;
    absolute.layers[0] = {"near",1,0,0,false,AbsoluteRowBounds{0,100}};
    auto bounded = legacy; bounded.priceBounds = PriceBounds{0,1};
    auto both = absolute; both.priceBounds = PriceBounds{0,1};
    auto wider = both; wider.layers[0].absoluteRows->high = 101;
    auto prices = both; prices.priceBounds->upper = 2;
    const std::vector<RecorderConfig> policies{legacy,absolute,legacy,bounded,legacy,both,legacy,wider,prices};
    std::vector<uint64_t> hashes;
    std::vector<std::vector<uint8_t>> originals;
    Hmc2Header header;
    for (size_t i = 0; i < policies.size(); ++i) {
        auto r = make(policies[i]);
        snap(*r,0,{{true,.4,2},{false,.6,4}}); tick(*r,60000);
        EXPECT_EQ(r->stats().diskErrors,0);
        const auto rows = read(); ASSERT_EQ(rows.size(),1);
        header = rows[0].header; hashes.push_back(header.configHash);
        const auto path = Hmc2Store::filePath(legacy.root,header,kEpoch,uint32_t(i));
        ASSERT_TRUE(std::filesystem::exists(path));
        originals.push_back(recorderBytes(path));
        for (size_t n = 0; n < i; ++n)
            EXPECT_EQ(recorderBytes(Hmc2Store::filePath(legacy.root,header,kEpoch,uint32_t(n))),originals[n]);
    }
    EXPECT_EQ(hashes[0],hashes[2]); EXPECT_EQ(hashes[0],hashes[4]); EXPECT_EQ(hashes[0],hashes[6]);
    std::set<uint64_t> distinct{hashes[0],hashes[1],hashes[3],hashes[5],hashes[7],hashes[8]};
    EXPECT_EQ(distinct.size(),6);
}

TEST_F(RecorderTest, BoundedOneSidedStateAndLeadContinueIdentically) {
    for (const auto& initial : std::vector<std::vector<Level>>{{{true,.99,2}},{{false,0,4}},{}}) {
        QTemporaryDir source, destination;
        auto c = boundedConfig(source.path().toStdString());
        c.deterministicResume = true;
        auto r = make(c); snap(*r,0,initial); tick(*r,90000);
        const auto state = r->exportState(); ASSERT_FALSE(state.empty());
        // Copy the durable prefix; import resumes the still-open second minute.
        std::filesystem::copy(c.root / "BTC-USD", std::filesystem::path(destination.path().toStdString()) / "BTC-USD",
                              std::filesystem::copy_options::recursive);
        c.root = destination.path().toStdString();
        auto restored = make(c); restored->importState(state);
        EXPECT_EQ(restored->exportState(),state);
        std::vector<std::shared_ptr<const Hmc2Record>> leadPubs;
        auto lead = r->forkLead([&](auto p) { leadPubs.push_back(std::move(p)); },500);
        for (auto* recorder : {r.get(),restored.get(),lead.get()}) {
            tick(*recorder,100000);
            EXPECT_TRUE(recorder->snapshotWait("BTC-USD").reason.empty());
            snap(*recorder,110000,{{true,.4,6},{false,.6,8}});
            tick(*recorder,180000);
        }
        EXPECT_EQ(r->exportState(),restored->exportState());
        const auto rows = Hmc2Store::readRange(c.root,"BTC-USD","near",60000,kEpoch,kEpoch+180000);
        ASSERT_EQ(rows.size(),3);
        const auto relative = Hmc2Store::filePath({},rows[0].header,kEpoch);
        EXPECT_EQ(recorderBytes(std::filesystem::path(source.path().toStdString()) / relative),recorderBytes(c.root / relative));
        ASSERT_FALSE(leadPubs.empty());
        EXPECT_EQ(leadPubs.back()->observedMs,60000);
        EXPECT_EQ(leadPubs.back()->bidRowLo,0); EXPECT_EQ(leadPubs.back()->askRowHi,100);
        if (!initial.empty() && initial.front().price == 0) {
            const auto forkedMinute = std::find_if(leadPubs.begin(),leadPubs.end(),[](const auto& p) {
                return p->bucketStartMs == kEpoch + 60000 && p->midClose == .5;
            });
            ASSERT_NE(forkedMinute,leadPubs.end()); // publication after the +110s snapshot
            EXPECT_DOUBLE_EQ((*forkedMinute)->midOpen,0);
            EXPECT_DOUBLE_EQ((*forkedMinute)->midMin,0);
        }
        // A different midpoint or inclusion policy cannot adopt the state.
        QTemporaryDir wrongRoot; c.root = wrongRoot.path().toStdString();
        c.priceBounds.reset(); auto wrong = make(c);
        EXPECT_THROW(wrong->importState(state),std::runtime_error);
    }
}

TEST_F(RecorderTest, BoundedHourlyRollupPreservesEmptyCoverageAndRecoversAfterRestart) {
    constexpr int64_t minute = 60000, hour = 60 * minute;
    auto c = boundedConfig(dir.path().toStdString());
    c.layers[0].hourlyRollup = true;
    {
        auto r = make(c);
        snap(*r,0,{{false,0,2}});
        snap(*r,minute / 2,{});
        tick(*r,30 * minute);
        EXPECT_EQ(r->stats().diskErrors,0);
    }
    {
        auto r = make(c); // rebuild the first half-hour from persisted minutes
        snap(*r,30 * minute,{});
        tick(*r,59 * minute);
        snap(*r,59 * minute,{{true,1,4}});
        tick(*r,hour);
        EXPECT_EQ(r->stats().diskErrors,0);
    }
    const auto minutes = read();
    ASSERT_EQ(minutes.size(),60);
    for (size_t i = 0; i < minutes.size(); ++i) {
        SCOPED_TRACE(i);
        EXPECT_EQ(minutes[i].bucketStartMs,int64_t(i) * minute);
        EXPECT_EQ(minutes[i].observedMs,minute);
        EXPECT_EQ(minutes[i].bidRowLo,0); EXPECT_EQ(minutes[i].bidRowHi,100);
        EXPECT_EQ(minutes[i].askRowLo,0); EXPECT_EQ(minutes[i].askRowHi,100);
        if (i > 0 && i < 59) {
            EXPECT_TRUE(minutes[i].entries.empty());
            EXPECT_DOUBLE_EQ(minutes[i].midOpen,.5); EXPECT_DOUBLE_EQ(minutes[i].midClose,.5);
        }
    }
    const auto hours = read(hour);
    ASSERT_EQ(hours.size(),1);
    const auto& h = hours.front();
    EXPECT_EQ(h.bucketStartMs,0); EXPECT_EQ(h.observedMs,hour);
    EXPECT_EQ(h.flags & (kPartial | kApproximateCoverage),0u);
    EXPECT_EQ(h.header.configHash,minutes.front().header.configHash);
    EXPECT_EQ(h.bidRowLo,0); EXPECT_EQ(h.bidRowHi,100);
    EXPECT_EQ(h.askRowLo,0); EXPECT_EQ(h.askRowHi,100);
    EXPECT_DOUBLE_EQ(h.midOpen,0); EXPECT_DOUBLE_EQ(h.midMin,0);
    EXPECT_DOUBLE_EQ(h.midClose,1); EXPECT_DOUBLE_EQ(h.midMax,1);
    ASSERT_EQ(h.coverage.size(),2);
    for (size_t side = 0; side < 2; ++side) {
        EXPECT_EQ(h.coverage[side].isAsk,bool(side));
        EXPECT_EQ(h.coverage[side].lo,0); EXPECT_EQ(h.coverage[side].hi,100);
        EXPECT_EQ(h.coverage[side].coveredMs,hour);
    }
    ASSERT_EQ(h.entries.size(),2);
    value(h,0,true,decodeSize(encodeSize(1)) / 60,2); // size 2 for half of one minute
    value(h,100,false,decodeSize(encodeSize(4)) / 60,4);
    for (const auto& entry : h.entries) EXPECT_EQ(entry.coveredMs,hour);
    const auto hourPath = Hmc2Store::filePath(c.root,h.header,kEpoch);
    const auto expected = recorderBytes(hourPath);

    // Simulate a crash after minute commits but before the hour append, using
    // only the durable minute prefix in a fresh scratch root.
    QTemporaryDir recoveredRoot;
    auto recovered = c; recovered.root = recoveredRoot.path().toStdString();
    std::filesystem::create_directories(recovered.root / "BTC-USD");
    std::filesystem::copy(c.root / "BTC-USD/near-60000",recovered.root / "BTC-USD/near-60000",
                          std::filesystem::copy_options::recursive);
    EXPECT_FALSE(std::filesystem::exists(Hmc2Store::filePath(recovered.root,h.header,kEpoch)));
    for (const auto& cfg : {c,recovered}) {
        auto r = make(cfg);
        snap(*r,hour + 10000,{}); // preserve existing hour, or recover missing previous hour
        EXPECT_EQ(r->stats().diskErrors,0);
        EXPECT_EQ(recorderBytes(Hmc2Store::filePath(cfg.root,h.header,kEpoch)),expected);
    }
}

TEST_F(RecorderTest, StateImportRejectsUnboundedAndRelativeSources) {
    for (const bool unbounded : {true,false}) for (const bool initialized : {false,true}) {
        SCOPED_TRACE(unbounded ? "unbounded -> bounded" : "relative -> absolute");
        SCOPED_TRACE(initialized);
        QTemporaryDir sourceRoot, targetRoot;
        auto sourceConfig = boundedConfig(sourceRoot.path().toStdString());
        if (unbounded) sourceConfig.priceBounds.reset();
        else sourceConfig.layers[0] = {"near",1,.5,2,false};
        auto source = make(sourceConfig);
        if (initialized) {
            snap(*source,0,{{true,.4,2},{false,.6,4}});
            tick(*source,10000);
        }
        const auto state = source->exportState();
        auto target = make(boundedConfig(targetRoot.path().toStdString()));
        const auto before = target->exportState();
        EXPECT_THROW(target->importState(state),std::runtime_error);
        EXPECT_EQ(target->exportState(),before); // refusal must not partially install state
    }
}

#include <nlohmann/json.hpp>
#include "Crc32.hpp"
TEST_F(RecorderTest, BoundedStateRejectsCorruptPolicyPriceAndMidBeforeInstallation) {
    auto c = boundedConfig(dir.path().toStdString());
    auto r = make(c); snap(*r,0,{{false,0,2}}); tick(*r,10000);
    const auto bytes = r->exportState();
    const auto original = nlohmann::json::from_cbor(bytes.begin(),bytes.end()-4);
    for (const std::string field : {"policy", "price", "seen", "mid", "minimum"}) {
        SCOPED_TRACE(field);
        auto j = original;
        auto& symbol = j["symbols"]["BTC-USD"];
        if (field == "policy") j["policy"]["priceBounds"][1] = 2;
        if (field == "price") symbol["asks"][0][0] = 101;
        if (field == "seen") symbol["midSeen"] = false;
        if (field == "mid") symbol["mid"] = .5;
        if (field == "minimum") symbol["midMin"] = -1;
        auto corrupt = nlohmann::json::to_cbor(j);
        const auto crc = sentinel::crc32(corrupt.data(),corrupt.size());
        for (unsigned n = 0; n < 4; ++n) corrupt.push_back(uint8_t(crc >> (n*8)));
        QTemporaryDir own; c.root = own.path().toStdString();
        auto restored = make(c);
        EXPECT_THROW(restored->importState(corrupt),std::runtime_error);
        EXPECT_TRUE(restored->snapshotWait("BTC-USD").reason.empty());
        EXPECT_EQ(restored->stats().columnsWritten,0);
        EXPECT_NO_THROW(restored->importState(bytes));
        EXPECT_EQ(restored->exportState(),bytes);
    }
}

TEST_F(RecorderTest, BoundedInvalidStateStillRetainsZeroMidPresence) {
    auto c = boundedConfig(dir.path().toStdString());
    auto r = make(c); snap(*r,0,{{false,0,2}});
    r->onInvalid("BTC-USD",kEpoch+10000,"gap"); r->drain();
    const auto bytes = r->exportState();
    auto j = nlohmann::json::from_cbor(bytes.begin(),bytes.end()-4);
    j["symbols"]["BTC-USD"]["midSeen"] = false;
    auto corrupt = nlohmann::json::to_cbor(j);
    const auto crc = sentinel::crc32(corrupt.data(),corrupt.size());
    for (unsigned n = 0; n < 4; ++n) corrupt.push_back(uint8_t(crc >> (n*8)));
    QTemporaryDir own; c.root = own.path().toStdString(); auto restored = make(c);
    EXPECT_THROW(restored->importState(corrupt),std::runtime_error);
    restored->importState(bytes);
    for (auto* recorder : {r.get(),restored.get()}) {
        snap(*recorder,20000,{{true,1,4}}); tick(*recorder,60000);
    }
    EXPECT_EQ(r->exportState(),restored->exportState());
    const auto rows = read(); ASSERT_EQ(rows.size(),1);
    EXPECT_EQ(rows[0].observedMs,50000);
    EXPECT_DOUBLE_EQ(rows[0].midOpen,0); EXPECT_DOUBLE_EQ(rows[0].midMin,0);
}
