#include "BookRecorder.hpp"
#include "capture/RawCapture.hpp"
#include "marketdata/dispatch/BookParser.hpp"
#include <QTemporaryDir>
#include <QCryptographicHash>
#include <QFile>
#include <gtest/gtest.h>
#include <filesystem>

TEST(RollerLiveFixture, RecordedInputPreservesLiveBytes) {
    QTemporaryDir dir(QStringLiteral(ROLLER_TEST_ROOT "/live-XXXXXX"));
    recording::RecorderConfig c;
    c.root=dir.path().toStdString(); c.priceScale=100;c.sizeScale.floor=1e-8;
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
    std::vector<std::filesystem::path> files;
    for(const auto& e:std::filesystem::recursive_directory_iterator(c.root))if(e.path().extension()==".hmc2")files.push_back(e.path());
    std::sort(files.begin(),files.end()); ASSERT_EQ(files.size(),2);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for(const auto& p:files){QFile f(QString::fromStdString(p.string()));ASSERT_TRUE(f.open(QIODevice::ReadOnly));hash.addData(f.readAll());}
    EXPECT_EQ(hash.result().toHex().toStdString(),"676013266163d8f4266c92df7e9126231d2d318264d0120a0e60a7a588b8e027");
}
