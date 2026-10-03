#include "capture/CaptureFanout.hpp"
#include "capture/CaptureSession.hpp"
#include "metrics/MetricsRegistry.hpp"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QLocalSocket>
#include <sys/resource.h>
#include <thread>
#include <iostream>
using namespace sentinel::capture;
uint32_t u32(const char* p) {uint32_t n=0;for(int i=0;i<4;++i)n|=uint32_t(uint8_t(p[i]))<<(8*i);return n;}
double cpu() {rusage u{};getrusage(RUSAGE_SELF,&u);return u.ru_utime.tv_sec+u.ru_stime.tv_sec+(u.ru_utime.tv_usec+u.ru_stime.tv_usec)/1e6;}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    const std::string mode=argc>1?argv[1]:"idle";
    const int seconds=argc>2?std::stoi(argv[2]):65;
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
                auto j=nlohmann::json::parse(buffer.data()+8,buffer.data()+8+h);
                if(j["type"]=="tip")ready=true; if(j["type"]=="record")++received;
                if(j["type"]=="disconnect")reading=false;
                buffer.remove(0,n+4);
            }
        }
    });
    if(mode=="client")while(!ready)std::this_thread::sleep_for(std::chrono::milliseconds(1));
    WriterConfig wc;wc.root=dir.path()+"/raw";
    if(fanout)wc.onBlock=[&](auto run,auto block,auto count,auto raw){fanout->publish(0,run,block,count,raw);};
    Session session(wc,nlohmann::json::object());
    auto begin=std::chrono::steady_clock::now();auto cpuBegin=cpu();
    const int count=seconds*100;
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
    return !session.error().empty() || (mode=="client" && received!=uint64_t(count+1));
}
