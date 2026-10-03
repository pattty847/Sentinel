#include "BookRecorder.hpp"
#include "roller/Roller.hpp"
#include "capture/RawCapture.hpp"
#include "marketdata/dispatch/BookParser.hpp"
#include <QTemporaryDir>
#include <QCryptographicHash>
#include <QFile>
#include <gtest/gtest.h>
#include <filesystem>

namespace {
void recordLive(const std::filesystem::path& root) {
    recording::RecorderConfig c;
    c.root=root; c.priceScale=100;c.sizeScale.floor=1e-8;
    c.layers={{"near",100,.95,1.05,false},{"deep",500,.25,4,true}};
    int64_t local=0,lastTick=0;
    {
        recording::BookRecorder recorder(c,[&]{return local;});
        sentinel::capture::RecordReader reader(QStringLiteral(ROLLER_FIXTURE)); sentinel::capture::Record r;
        while(reader.next(r)) {
            local=r.time.systemNs/1'000'000;
            if(!lastTick)lastTick=local;
            while(lastTick+250<=local){lastTick+=250;recorder.onTick(lastTick);}
            using sentinel::capture::Kind;
            if(r.kind==Kind::TransportDown || r.kind==Kind::BookInvalidated) recorder.onInvalid("BTC-USD",local,"recorded fixture");
            if(r.kind!=Kind::Frame)continue;
            auto j=nlohmann::json::parse(r.payload);
            if(j.value("channel","")!="l2_data")continue;
            const auto exchange=std::chrono::duration_cast<std::chrono::milliseconds>(Cpp20Utils::parseISO8601(j.at("timestamp").get<std::string>()).time_since_epoch()).count();
            for(const auto& e:j.at("events")) {
                if(e.value("product_id","")!="BTC-USD")continue;
                std::vector<recording::Level> levels;
                if(e.value("type","")=="snapshot") {
                    std::vector<OrderBookLevel>bids,asks;
                    ASSERT_EQ(sentinel::dispatch::parseSnapshot(e,bids,asks),0);
                    for(const auto& l:bids)levels.push_back({true,l.price,l.size});
                    for(const auto& l:asks)levels.push_back({false,l.price,l.size});
                    recorder.onSnapshot("BTC-USD",exchange,std::move(levels));
                }else if(e.value("type","")=="update") {
                    std::vector<BookLevelUpdate> updates;ASSERT_TRUE(sentinel::dispatch::parseUpdates(e,updates));
                    for(const auto& l:updates)levels.push_back({l.isBid,l.price,l.quantity});
                    if(!levels.empty())recorder.onUpdates("BTC-USD",exchange,std::move(levels));
                }
            }
            // Real producer cannot outrun this deterministic fixture's worker.
            recorder.drainForTest();
        }
        recorder.drainForTest();EXPECT_EQ(recorder.stats().queueDrops,0);EXPECT_EQ(recorder.stats().diskErrors,0);
    }
}
}
TEST(RollerLiveFixture, RecordedInputPreservesLiveBytes) {
    QTemporaryDir dir(QStringLiteral(ROLLER_TEST_ROOT "/live-XXXXXX"));
    ASSERT_TRUE(dir.isValid());
    const std::filesystem::path root=dir.path().toStdString();
    recordLive(root);
    std::vector<std::filesystem::path> files;
    for(const auto& e:std::filesystem::recursive_directory_iterator(root))if(e.path().extension()==".hmc2")files.push_back(e.path());
    std::sort(files.begin(),files.end()); ASSERT_EQ(files.size(),2);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for(const auto& p:files){QFile f(QString::fromStdString(p.string()));ASSERT_TRUE(f.open(QIODevice::ReadOnly));hash.addData(f.readAll());}
    EXPECT_EQ(hash.result().toHex().toStdString(),"676013266163d8f4266c92df7e9126231d2d318264d0120a0e60a7a588b8e027");
}

TEST(RollerLiveFixture, ControlledTickScheduleStaysWithinBands) {
    namespace fs=std::filesystem;using namespace sentinel::roller;
    QTemporaryDir dir(QStringLiteral(ROLLER_TEST_ROOT "/ticks-XXXXXX"));ASSERT_TRUE(dir.isValid());
    const fs::path root=dir.path().toStdString();recordLive(root/"legacy");
    fs::create_directories(root/"raw");fs::copy_file(ROLLER_FIXTURE,root/"raw"/"btc.rawl2");
    const auto from=parseTime("2026-10-01T04:53:00Z"),to=from+180'000;
    roll({root/"raw",root/"journal","BTC-USD",from,to});
    uint64_t totalDifferent=0;
    for(const auto* layer:{"near","deep"}) {
        const auto report=diff(root/"legacy",root/"journal","BTC-USD",layer,from,to);
        ASSERT_EQ(report["qualifying"],2);
        for(const auto& minute:report["minutes"]) if(minute["qualifies"].get<bool>()) {
            const auto& d=minute.at("difference");
            EXPECT_TRUE(d["midsEqual"].get<bool>());EXPECT_TRUE(d["boundsEqual"].get<bool>());
            EXPECT_EQ(d["onlyA"],0);EXPECT_EQ(d["onlyB"],0);EXPECT_EQ(d["peakCodes"],0);
            const auto entries=d["entriesA"].get<uint64_t>();ASSERT_GT(entries,1000);
            totalDifferent+=d["twapCodes"].get<uint64_t>();
            EXPECT_LE(d["twapCodes"].get<double>()/entries,.01);
            EXPECT_LE(d["maxTwapCodeDelta"].get<int>(),32);
            uint64_t histogramCount=0,different=0;int maxDelta=0;
            for(const auto& [delta,n]:d["twapCodeDeltaHistogram"].items()) {
                histogramCount+=n.get<uint64_t>();if(std::stoi(delta))different+=n.get<uint64_t>();
                maxDelta=std::max(maxDelta,std::abs(std::stoi(delta)));
            }
            EXPECT_EQ(histogramCount,entries);EXPECT_EQ(different,d["twapCodes"].get<uint64_t>());
            EXPECT_EQ(maxDelta,d["maxTwapCodeDelta"].get<int>());
            for(const auto* side:{"bid","ask"}) {
                const auto& total=d["totalTwap"][side];ASSERT_FALSE(total["relativeDelta"].is_null());
                EXPECT_LE(std::abs(total["relativeDelta"].get<double>()),.0001);
            }
        }
    }
    EXPECT_GT(totalDifferent,0);
}
