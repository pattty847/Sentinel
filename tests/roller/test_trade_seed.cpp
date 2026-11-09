#include "roller/Roller.hpp"
#include <gtest/gtest.h>
#include <QTemporaryDir>
#include <fstream>
#include <iostream>

using namespace sentinel;
using namespace sentinel::roller;
using nlohmann::json;
namespace fs = std::filesystem;
namespace {
constexpr int64_t Epoch = 1'791'158'400'000;
constexpr size_t N = JournalFeed::TradeIdWindowCapacity;
json metadata() { return {{"product_metadata", {{"product_id","BTC-USD"},
    {"quote_increment","0.01"},{"base_increment","0.00000001"}}}}; }
capture::Record record(int64_t ms, const json& payload) {
    return {capture::Kind::Frame, {(Epoch+ms)*1'000'000, 0}, 1, payload.dump()};
}
json trades(const std::vector<std::string>& ids) {
    auto list = json::array();
    for (const auto& id : ids) list.push_back({{"product_id","BTC-USD"},{"trade_id",id},
        {"time","2026-10-05T00:00:00.000Z"},{"price","100000"},{"size","1"},{"side","BUY"}});
    return {{"channel","market_trades"},{"trades",std::move(list)}};
}
json snapshot() {
    return {{"channel","l2_data"},{"events",json::array({{{"type","snapshot"},{"product_id","BTC-USD"},
        {"updates",json::array({{{"side","bid"},{"price_level","99999"},{"new_quantity","2"}},
                               {{"side","offer"},{"price_level","100001"},{"new_quantity","3"}}})}}})}};
}
capture::WriterConfig config(const fs::path& root) {
    capture::WriterConfig c; c.root=QString::fromStdString(root.string()); c.fsyncBlocks=0; return c;
}
void appendIds(capture::Writer& writer, int64_t ms, size_t first, size_t count) {
    for (size_t i=0; i<count; i+=128) {
        std::vector<std::string> ids;
        for (size_t j=i; j<count && j<i+128; ++j) ids.push_back(std::to_string(first+j));
        writer.append(record(ms,trades(ids)));
    }
}
std::vector<JournalRecord> read(const fs::path& root) {
    JournalReader reader(root,"BTC-USD"); JournalRecord r; std::vector<JournalRecord> records;
    while (reader.next(r)) records.push_back(r);
    return records;
}
}
TEST(TradeSeed, CounterexampleDuplicatesAndEvictionMatchContinuous) {
    QTemporaryDir tmp; const fs::path root=tmp.path().toStdString();
    capture::Writer w(config(root),metadata());
    appendIds(w,0,1,N);
    w.sealSegment();
    w.append(record(1000,trades({"1"})));
    w.append(record(2000,trades({std::to_string(N+1)})));
    w.append(record(3000,trades({"1"})));
    w.append(record(4000,trades({"2"})));
    w.close();
    const auto records=read(root);
    JournalReader inventory(root,"BTC-USD");
    JournalFeed continuous("BTC-USD"), restored("BTC-USD");
    std::vector<std::string> expected, actual;
    continuous.onTrade=[&](const Trade& t){expected.push_back(t.trade_id);};
    restored.onTrade=[&](const Trade& t){actual.push_back(t.trade_id);};
    const size_t boundary=records.size()-3;
    for (size_t i=0;i<=boundary;++i) continuous.apply(records[i]);
    expected.clear();
    restored.importState(continuous.exportState());
    const auto before=restored.exportState();
    int seedCallbacks=0;
    restored.onSnapshot=[&](auto,auto,auto){++seedCallbacks;};
    restored.onUpdates=[&](auto,auto,auto){++seedCallbacks;};
    restored.onInvalid=[&](auto,const auto&){++seedCallbacks;};
    restored.onTick=[&](auto){++seedCallbacks;};
    restored.onConnection=[&](auto){++seedCallbacks;};
    restored.onConnectionChange=[&]{++seedCallbacks;};
    const auto stats=restored.seedTradeIds(inventory.files(),records[boundary].pos,true);
    EXPECT_EQ(restored.exportState(),before); EXPECT_TRUE(actual.empty()); EXPECT_EQ(seedCallbacks,0);
    EXPECT_EQ(stats.records,N); EXPECT_EQ(stats.segments,2u); EXPECT_FALSE(stats.degraded);
    EXPECT_EQ(restored.tradeIds(),continuous.tradeIds());
    JournalFeed fallback("BTC-USD");
    fallback.seedTradeIds(inventory.files(),records[boundary+1].pos,false);
    EXPECT_EQ(fallback.tradeIds(),continuous.tradeIds());
    std::vector<std::string> fallbackIds;
    fallback.onTrade=[&](const Trade& t){fallbackIds.push_back(t.trade_id);};
    JournalFeed copy(restored); std::vector<std::string> copied;
    copy.onTrade=[&](const Trade& t){copied.push_back(t.trade_id);};
    for (size_t i=boundary+1;i<records.size();++i) {
        continuous.apply(records[i]); restored.apply(records[i]); copy.apply(records[i]); fallback.apply(records[i]);
    }
    EXPECT_EQ(expected,(std::vector<std::string>{"2"}));
    EXPECT_EQ(actual,expected); EXPECT_EQ(copied,expected); EXPECT_EQ(fallbackIds,expected);
    EXPECT_EQ(restored.tradeIds(),continuous.tradeIds());
}
TEST(TradeSeed, DuplicateOccurrencesBoundTheScanAndEmptyIdsAgeTheWindow) {
    QTemporaryDir tmp; const fs::path root=tmp.path().toStdString();
    capture::Writer w(config(root),metadata());
    w.append(record(0,trades({"old"}))); w.sealSegment();
    for (size_t i=0;i<N/128;++i) w.append(record(1000,trades(std::vector<std::string>(128,"dup"))));
    w.append(record(2000,snapshot())); w.close();
    const auto records=read(root); JournalReader inventory(root,"BTC-USD");
    JournalFeed f("BTC-USD");
    auto stats=f.seedTradeIds(inventory.files(),records.back().pos,false);
    EXPECT_EQ(stats.records,N); EXPECT_EQ(stats.ids,1u); EXPECT_EQ(stats.segments,1u);
    EXPECT_EQ(f.tradeIds(),(std::unordered_set<std::string>{"dup"}));
    JournalRecord r=records.back(); r.record=record(3000,trades(std::vector<std::string>(128,"")));
    size_t delivered=0; f.onTrade=[&](const auto&){++delivered;};
    for (size_t i=0;i<N/128;++i) f.apply(r);
    EXPECT_EQ(delivered,N); EXPECT_EQ(f.retainedTradeIds(),0u);
    r.record=record(4000,trades({"dup","old"})); f.apply(r);
    EXPECT_EQ(delivered,N+2);
}
TEST(TradeSeed, ShortJournalAndSequenceRejectedFramesUseSeenOccurrences) {
    QTemporaryDir tmp; const fs::path root=tmp.path().toStdString();
    capture::Writer w(config(root),metadata());
    auto a=trades({"one"}); a["sequence_num"]=1; w.append(record(0,a));
    a=trades({"rejected"}); a["sequence_num"]=3; w.append(record(1000,a));
    a=trades({"rejected","two"}); a["sequence_num"]=4; w.append(record(2000,a));
    w.close(); const auto records=read(root); JournalReader inventory(root,"BTC-USD");
    JournalFeed continuous("BTC-USD"),restored("BTC-USD");
    std::vector<std::string> x,y;
    continuous.onTrade=[&](const auto& t){x.push_back(t.trade_id);};
    restored.onTrade=[&](const auto& t){y.push_back(t.trade_id);};
    continuous.apply(records[0]); continuous.apply(records[1]); x.clear();
    restored.importState(continuous.exportState());
    const auto stats=restored.seedTradeIds(inventory.files(),records[1].pos,true);
    EXPECT_EQ(stats.records,2u); EXPECT_FALSE(stats.degraded);
    continuous.apply(records[2]); restored.apply(records[2]);
    EXPECT_EQ(x,(std::vector<std::string>{"two"})); EXPECT_EQ(x,y);
}
TEST(TradeSeed, DamagedOrMissingEarlierSegmentIsBestEffort) {
    for (const bool missing : {false,true}) {
        QTemporaryDir tmp; const fs::path root=tmp.path().toStdString();
        capture::Writer w(config(root),metadata());
        w.append(record(-4000,trades({"old"}))); w.sealSegment();
        w.append(record(-3000,trades({"lost"}))); w.sealSegment();
        w.append(record(-2000,trades({"recent"}))); w.append(record(-1000,snapshot()));
        w.append(record(65000,{{"channel","heartbeats"}})); w.close();
        const auto records=read(root); JournalReader inventory(root,"BTC-USD");
        ASSERT_EQ(inventory.files().size(),4u);
        const auto damaged=inventory.files()[1].path;
        if (missing) fs::remove(damaged); // Scratch fixture only, after inventory.
        else {
            const auto scan=capture::scan(QString::fromStdString(damaged.string()));
            std::fstream f(damaged,std::ios::binary|std::ios::in|std::ios::out);
            const auto offset=scan.index[0].offset+60; f.seekg(offset); char c=0; f.read(&c,1);
            c^=0x40; f.seekp(offset); f.write(&c,1);
        }
        JournalFeed feed("BTC-USD");
        const auto stats=feed.seedTradeIds(inventory.files(),records[3].pos,false);
        EXPECT_TRUE(stats.degraded); EXPECT_EQ(stats.segments,3u);
        EXPECT_EQ(feed.tradeIds(),(std::unordered_set<std::string>{"old","recent"}));
        RollOptions o{root,root/"out","BTC-USD",Epoch,Epoch+60000};
        EXPECT_NO_THROW(roll(o));
    }
}
TEST(TradeSeed, RollAnchorAndSnapshotFallbackMatchContinuousDelivery) {
    QTemporaryDir tmp; const fs::path root=tmp.path().toStdString();
    capture::Writer w(config(root/"raw"),metadata());
    w.append(record(-2000,trades({"before-snapshot"}))); w.sealSegment();
    w.append(record(-1000,snapshot()));
    w.append(record(0,trades({"before-snapshot","before-anchor"})));
    for (int t=1;t<=965;++t) {
        if (t==901) w.append(record(t*1000,trades({"before-anchor","new"})));
        else w.append(record(t*1000,{{"channel","heartbeats"}}));
    }
    w.close();
    const auto records=read(root/"raw");
    JournalFeed continuous("BTC-USD"); std::vector<std::string> expected;
    continuous.onTrade=[&](const Trade& t){expected.push_back(t.trade_id);};
    for (const auto& r:records) {
        if (r.record.time.systemNs/1000000<Epoch-1000) { continuous.apply(r); expected.clear(); }
        else continuous.apply(r);
    }
    RollOptions o{root/"raw",root/"full","BTC-USD",Epoch,Epoch+960000};
    std::vector<std::string> actual;
    o.onFeed=[&](JournalFeed& f){f.onTrade=[&](const Trade& t){actual.push_back(t.trade_id);};};
    roll(o); EXPECT_EQ(actual,expected); EXPECT_EQ(actual,(std::vector<std::string>{"before-anchor","new"}));
    // Advance a separate output to the intraday anchor, then retry roll().
    o.outputRoot=root/"resumed"; o.anchorRoot=root/"full"; o.writeAnchors=false;
    bool stop=false; o.cancelled=[&]{return stop;};
    o.onApplied=[&](const auto& r){stop=r.record.time.systemNs/1000000>=Epoch+900000;};
    roll(o);
    actual.clear(); o.cancelled={}; o.onApplied={};
    const auto report=roll(o);
    ASSERT_TRUE(report["days"][0].contains("anchorBoundaryMs")) << report.dump();
    EXPECT_EQ(report["days"][0]["anchorBoundaryMs"],Epoch+900000);
    EXPECT_GT(report["tradeSeedDecodedRecords"].get<uint64_t>(),0u);
    EXPECT_GE(report["decodedRecords"].get<uint64_t>(),report["tradeSeedDecodedRecords"].get<uint64_t>());
    EXPECT_EQ(actual,(std::vector<std::string>{"new"}));
}
TEST(TradeSeed, FixtureCostAndParity) {
    // Read the checked-in capture fixtures only; do not access runtime journals.
    for (const auto* name : {"btc-prefix.rawl2", "btc-hour-boundary.rawl2"}) {
        QTemporaryDir tmp; const fs::path root=tmp.path().toStdString();
        const auto source=fs::path(ROLLER_FIXTURE).parent_path()/name;
        fs::copy_file(source,root/name);
        JournalReader inventory(root,"BTC-USD"); JournalRecord r,last;
        JournalFeed continuous("BTC-USD"),restored("BTC-USD");
        continuous.onTrade=[](const Trade&){};
        size_t records=0;
        while (inventory.next(r)) {continuous.apply(r);last=r;++records;}
        ASSERT_GT(records,0u);
        const auto stats=restored.seedTradeIds(inventory.files(),last.pos,true);
        EXPECT_EQ(restored.tradeIds(),continuous.tradeIds());
        EXPECT_LE(stats.records,N);
        std::cout<<"TRADE_SEED_BENCH fixture="<<name<<" bytes="<<fs::file_size(source)
                 <<" journal_records="<<records<<" seed_records="<<stats.records
                 <<" ids="<<stats.ids<<" segments="<<stats.segments<<" ms="<<stats.ms<<'\n';
    }
}
TEST(TradeSeed, FrameOrderingForeignProductsAndRunChanges) {
    QTemporaryDir tmp; const fs::path root=tmp.path().toStdString();
    {
        capture::Writer first(config(root),metadata());
        first.append(record(0,trades({"old-run","10","2","2"}))); first.close();
    }
    {
        capture::Writer second(config(root),metadata());
        auto frame=trades({"10","2","opaque",""});
        auto foreign=frame["trades"][0]; foreign["product_id"]="ETH-USD"; foreign["trade_id"]="foreign";
        frame["trades"].push_back(foreign);
        frame["events"]=json::array({{{"trades",trades({"3","opaque"})["trades"]}}});
        second.append(record(1000,frame));
        second.append(record(2000,trades({"old-run","3","4"}))); second.close();
    }
    const auto records=read(root); ASSERT_EQ(records.size(),3u);
    JournalReader inventory(root,"BTC-USD");
    JournalFeed continuous("BTC-USD"),restored("BTC-USD");
    std::vector<std::string> x,y;
    continuous.onTrade=[&](const auto& t){x.push_back(t.trade_id);};
    restored.onTrade=[&](const auto& t){y.push_back(t.trade_id);};
    continuous.apply(records[0]); continuous.apply(records[1]); x.clear();
    restored.importState(continuous.exportState());
    const auto stats=restored.seedTradeIds(inventory.files(),records[1].pos,true);
    EXPECT_EQ(stats.records,10u); EXPECT_EQ(stats.segments,2u);
    EXPECT_EQ(continuous.tradeIds(),restored.tradeIds());
    EXPECT_FALSE(restored.tradeIds().contains("foreign"));
    continuous.apply(records[2]); restored.apply(records[2]);
    EXPECT_EQ(x,(std::vector<std::string>{"4"})); EXPECT_EQ(x,y);
}
TEST(TradeSeed, RollWithoutTradeConsumerSkipsSeedAndTradeWindow) {
    QTemporaryDir tmp; const fs::path root=tmp.path().toStdString();
    capture::Writer w(config(root/"raw"),metadata());
    w.append(record(-2000,trades({"before-snapshot"})));
    w.append(record(-1000,snapshot()));
    w.append(record(0,trades({"before-snapshot","before-anchor"})));
    for (int t=1;t<=965;++t)
        w.append(record(t*1000,t==901 ? trades({"before-anchor","new"}) : json{{"channel","heartbeats"}}));
    w.close();
    RollOptions o{root/"raw",root/"out","BTC-USD",Epoch,Epoch+960000};
    const JournalFeed* history=nullptr;
    size_t observed=0;
    o.onRecorder=[&](auto*,const JournalFeed* feed,auto){history=feed;};
    o.onApplied=[&](const auto&) {
        ASSERT_NE(history,nullptr);
        EXPECT_EQ(history->retainedTradeIds(),0u);
        ++observed;
    };
    const auto fallback=roll(o);
    EXPECT_FALSE(fallback["days"][0].contains("anchorBoundaryMs"));
    EXPECT_EQ(fallback["tradeSeedDecodedRecords"],0);
    EXPECT_GT(observed,0u);
    const auto restored=roll(o);
    ASSERT_TRUE(restored["days"][0].contains("anchorBoundaryMs"));
    EXPECT_EQ(restored["days"][0]["anchorBoundaryMs"],Epoch+900000);
    EXPECT_EQ(restored["tradeSeedDecodedRecords"],0);
    // The same anchor has a nonempty seed when a trade consumer needs it.
    o.onApplied={};
    o.onFeed=[](JournalFeed& feed){feed.onTrade=[](const Trade&){};};
    const auto consumed=roll(o);
    EXPECT_GT(consumed["tradeSeedDecodedRecords"].get<uint64_t>(),0u);
}
TEST(TradeSeed, ClearingASeededWindowRecyclesNodesAcrossWraparound) {
    TradeRecordWindow window;
    for (size_t pass=0;pass<3;++pass) {
        for (size_t i=0;i<2*N;++i) EXPECT_TRUE(window.remember(std::to_string(i)));
        EXPECT_FALSE(window.remember(std::to_string(2*N-1)));
        window.clear();
        EXPECT_EQ(window.uniqueIds(),0u);
        EXPECT_TRUE(window.remember("fresh"));
        EXPECT_FALSE(window.remember("fresh"));
        window.clear();
    }
}
