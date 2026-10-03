#include "capture/CaptureFanout.hpp"
#include "capture/CaptureSession.hpp"
#include "metrics/MetricsRegistry.hpp"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QLocalSocket>
#include <sys/resource.h>
#include <thread>
#include <iostream>
#include <algorithm>
using namespace sentinel::capture;
uint32_t u32(const char* p) {uint32_t n=0;for(int i=0;i<4;++i)n|=uint32_t(uint8_t(p[i]))<<(8*i);return n;}
int64_t nowNs() {return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
double cpu() {rusage u{};getrusage(RUSAGE_SELF,&u);return u.ru_utime.tv_sec+u.ru_stime.tv_sec+(u.ru_utime.tv_usec+u.ru_stime.tv_usec)/1e6;}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    const std::string mode=argc>1?argv[1]:"idle";
    const int seconds=argc>2?std::stoi(argv[2]):65;
    const int count=seconds*100;
    std::vector<std::atomic<int64_t>> appendTimes(count+1);
    std::vector<int64_t> latency; latency.reserve(count+1);
    QTemporaryDir dir("/tmp/sfb-XXXXXX"); sentinel::metrics::MetricsRegistry metrics;
    std::unique_ptr<CaptureFanout> fanout;
    if(mode!="baseline") {FanoutConfig cfg;cfg.socketPath=dir.path()+"/f.sock";fanout=std::make_unique<CaptureFanout>(cfg,std::vector<std::string>{"BTC-USD"},metrics,nullptr);}
    std::atomic<bool> reading{true}, ready{false}; std::atomic<uint64_t> received{0};
    std::thread reader;
    if(mode=="client") reader=std::thread([&] {
        QLocalSocket socket;socket.connectToServer(fanout->path());if(!socket.waitForConnected(3000)) std::abort();
        socket.write("{\"type\":\"hello\",\"version\":1,\"product\":\"BTC-USD\"}\n");socket.waitForBytesWritten();
        QByteArray buffer;
        while(reading || socket.bytesAvailable()) {
            socket.waitForReadyRead(10);buffer+=socket.readAll();
            while(buffer.size()>=4 && buffer.size()>=qsizetype(u32(buffer.data())+4)) {
                const auto n=u32(buffer.data()), h=u32(buffer.data()+4);
                const auto receivedNs=nowNs();
                auto j=nlohmann::json::parse(buffer.data()+8,buffer.data()+8+h);
                if(j["type"]=="tip")ready=true; if(j["type"]=="record") {auto n=received.load(); if(n>uint64_t(count))std::abort(); latency.push_back(receivedNs-appendTimes[n].load(std::memory_order_acquire)); ++received;}
                if(j["type"]=="disconnect")reading=false;
                buffer.remove(0,n+4);
            }
        }
    });
    if(mode=="client")while(!ready)std::this_thread::sleep_for(std::chrono::milliseconds(1));
    WriterConfig wc;wc.root=dir.path()+"/raw";
    if(fanout)wc.onJournal=[&](const JournalEvent& e){fanout->publish(0,e);};
    size_t appended=0; SessionHooks hooks;
    hooks.beforeWriterOperation=[&](const auto&,auto op,const auto*) {
        if(op=="append") appendTimes.at(appended++).store(nowNs(),std::memory_order_release);
    };
    Session session(wc,nlohmann::json::object(),64*1024*1024,hooks);
    auto begin=std::chrono::steady_clock::now();auto cpuBegin=cpu();
    for(int i=0;i<count;++i) {
        if(!session.submit({Kind::Frame,Stamp::now(),1,std::string(1500,'x')}))return 2;
        std::this_thread::sleep_until(begin+std::chrono::milliseconds((i+1)*10));
    }
    session.close();
    if(mode=="client") {
        auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
        while(received<uint64_t(count+1)&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto cost=cpu()-cpuBegin;
    rusage usage{};getrusage(RUSAGE_SELF,&usage);
    std::cout<<"mode="<<mode<<" seconds="<<seconds<<" records="<<count<<" cpu_seconds="<<cost<<" cpu_percent="<<100*cost/seconds<<" peak_rss_bytes="<<usage.ru_maxrss<<" received="<<received<<" disk_pool_bytes="<<session.queuedBytes()<<"\n";
    std::cout<<metrics.render();
    if(fanout)fanout->stop(); reading=false;if(reader.joinable())reader.join();
    if(!latency.empty()) {
        std::sort(latency.begin(),latency.end());
        std::cout << "append_to_receive_samples=" << latency.size() << " p50_us=" << latency[latency.size()/2]/1000.0
                  << " p95_us=" << latency[(latency.size()-1)*95/100]/1000.0 << " max_us=" << latency.back()/1000.0 << "\n";
    }
    return !session.error().empty() || (mode=="client" && received!=uint64_t(count+1));
}
