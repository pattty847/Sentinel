// Offline restart evidence; RAWL2 is opened read-only by the roller.
#include "roller/Roller.hpp"
#include "levelstore/LevelLog.hpp"
#include "levelstore/SealStore.hpp"
#include <boost/multiprecision/cpp_int.hpp>
#include <sstream>
#include <nlohmann/json.hpp>
#include <chrono>
#include <fstream>
#include <iostream>
#include <atomic>
#include <algorithm>
#ifndef _WIN32
#include <unistd.h>
#include <signal.h>
#endif
#ifdef __APPLE__
#include <libproc.h>
#include <sys/resource.h>
#include <mach/mach.h>
#include <cstring>
#endif
namespace fs=std::filesystem;
using namespace sentinel;
using nlohmann::json;
using Clock=std::chrono::steady_clock;
#ifdef __APPLE__
int64_t workerCpuNs() {
    thread_act_array_t threads; mach_msg_type_number_t n=0;
    if (task_threads(mach_task_self(),&threads,&n)!=KERN_SUCCESS) return -1;
    int64_t result=-1;
    for (mach_msg_type_number_t i=0;i<n;++i) {
        thread_extended_info_data_t info{}; mach_msg_type_number_t count=THREAD_EXTENDED_INFO_COUNT;
        if (thread_info(threads[i],THREAD_EXTENDED_INFO,reinterpret_cast<thread_info_t>(&info),&count)==KERN_SUCCESS &&
            std::strcmp(info.pth_name,"book-recorder")==0) result=int64_t(info.pth_user_time+info.pth_system_time);
        mach_port_deallocate(mach_task_self(),threads[i]);
    }
    vm_deallocate(mach_task_self(),reinterpret_cast<vm_address_t>(threads),n*sizeof(thread_act_t));
    return result;
}
#endif
struct Files : levelstore::LevelLogFiles {
    std::atomic<bool> armed{false};
    bool cut=false;
    uint64_t written=0;
    size_t write(std::FILE* f,std::span<const uint8_t> b) override {
        if (armed.exchange(false)) {
            LevelLogFiles::write(f,b.first(std::max(size_t(1),b.size()/2)));
#ifndef _WIN32
            ::kill(::getpid(),SIGKILL);
#endif
        }
        written+=b.size(); return LevelLogFiles::write(f,b);
    }
    void truncate(const fs::path& p,uint64_t n) override {
        LevelLogFiles::truncate(p,n);
#ifndef _WIN32
        if (cut) ::kill(::getpid(),SIGKILL);
#endif
    }
};
int main(int argc,char** argv) {
    if (argc!=9) { std::cerr<<"RAW OUT PRODUCT DAY REPORT POINT MINUTE MODE(C|null)\n"; return 2; }
    json report={{"pass",false}};
    try {
        const auto raw=fs::canonical(argv[1]); const auto out=fs::absolute(argv[2]).lexically_normal();
        if ((out.string()+"/").starts_with(raw.string()+"/") || (raw.string()+"/").starts_with(out.string()+"/"))
            throw std::runtime_error("RAW and output overlap");
        fs::create_directories(out);
        roller::RollOptions o{raw,out/"hmc2",argv[3],roller::parseTime(argv[4]),roller::parseTime(argv[4])+86'400'000};
        auto files=std::make_shared<Files>();
        if (std::string(argv[8])!="null") { o.levelLogRoot=out/"log"; o.levelLogFiles=files; }
        o.levelSeals=std::string(argv[8])!="log";
        const std::string point=argv[6]; const int minute=std::stoi(argv[7]);
        bool ready=false;
        const bool audit=point=="audit";
        std::ofstream source;
        int64_t sourceHour=-1;
        uint64_t sourceCount=0;
        std::atomic<int64_t> auditNs{0};
        const auto auditRoot=fs::path(std::string(argv[5])+".source");
        if(audit)fs::create_directories(auditRoot);
        bool savedSeed=false;
        std::map<std::string,Clock::time_point> sealStarts;

        int64_t appliedMinute=-1, cpuMinute=-1, cpuBefore=-1;
        files->cut=point=="cut";
        std::map<std::string,std::vector<double>> costs;
        json minuteSamples=json::array();
        o.levelTiming=[&](const char* name,double ms) {
            costs[name].push_back(ms);
            if (std::string_view(name)=="checkpoint-total") {
                json sample={{"minute",appliedMinute-o.fromMs/60'000},{"fenceMs",ms}};
#ifdef __APPLE__
                rusage usage{}; getrusage(RUSAGE_SELF,&usage); sample["peakRssBytes"]=usage.ru_maxrss;
#endif
                minuteSamples.push_back(std::move(sample));
            }
        };
        if (point=="measure-prefix") o.cancelled=[&] { return appliedMinute>=o.fromMs/60'000+minute; };
        const auto start=Clock::now();
        o.onApplied=[&](const roller::JournalRecord& r) {
            auto ms=r.record.time.systemNs/1'000'000;
            appliedMinute=ms/60'000;
            ready=ms>=o.fromMs+int64_t(minute)*60'000;
            if (ready && point=="append") files->armed=true;

        };
        o.persistencePointForTest=[&](const char* p) {
            for(const std::string kind:{"seal-hour","seal-day"}) {
                if(std::string(p)==kind+"-begin")sealStarts[kind]=Clock::now();
                if(std::string(p)==kind+"-end")costs[kind].push_back(std::chrono::duration<double,std::milli>(Clock::now()-sealStarts.at(kind)).count());
            }
#ifdef __APPLE__
            if (std::string_view(p)=="checkpoint-written") {
                const auto cpu=workerCpuNs();
                if (cpu>=0 && cpuBefore>=0 && appliedMinute>cpuMinute)
                    costs["worker-cpu-per-journal-minute"].push_back(double(cpu-cpuBefore)/1e6/double(appliedMinute-cpuMinute));
                cpuBefore=cpu; cpuMinute=appliedMinute;
            }
#endif
            if(std::string_view(p)=="checkpoint-written" && audit && !savedSeed && appliedMinute>=o.fromMs/60000+1365) {
                // Called after a durable, drained fence; copying is outside worker CPU.
                const auto seed=fs::path(std::string(argv[5])+".seed");
                fs::copy(out,seed,fs::copy_options::recursive);savedSeed=true;
            }
            if (std::string_view(p)=="recovery-replayed")
                report["recoveryOpenCutReplayMs"]=std::chrono::duration<double,std::milli>(Clock::now()-start).count();
#ifndef _WIN32
            if (ready && point==p) ::kill(::getpid(),SIGKILL);
#endif
        };
        recording::BookRecorder* recorder=nullptr;
        levelstore::LevelRecorder* consumer=nullptr;
        std::vector<uint8_t> finalState;
        o.onRecorder=[&](auto* p,auto*,int64_t) { recorder=p; };
        o.onLevelRecorder=[&](auto* p) {
            if (!p && consumer && recorder) {
                try {
                    const auto bytes=recorder->exportState();
                    finalState=consumer->exportState(recorder->checkpointClock(o.product),bytes);
                } catch (...) { finalState.clear(); }
            }
            consumer=p;
            if(p && audit)p->exactInputForTest=[&](int64_t clock,const recording::ExactSourceLevel& level){
                const auto begin=Clock::now();const auto hour=clock/3600000*3600000;
                if(hour!=sourceHour){source.close();source.open(auditRoot/(std::to_string(hour)+".txt"),std::ios::app);source.exceptions(std::ios::badbit|std::ios::failbit);sourceHour=hour;}
                source<<clock<<' '<<level.isBid<<' '<<level.priceText<<' '<<level.sizeText<<'\n';++sourceCount;
                auditNs+=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-begin).count();
            };
        };
        report["roll"]=roller::roll(o);
        if (!o.levelLogRoot.empty()) {
            if (finalState.empty()) throw std::runtime_error("missing live final C state");
            std::ofstream state(out/"final.state",std::ios::binary);
            state.write(reinterpret_cast<const char*>(finalState.data()),finalState.size());
            report["finalLiveStateBytes"]=finalState.size();
        }
        source.close();
        report["sourceCaptureWorkerMs"]=double(auditNs.load())/1e6;
        report["rollWallMs"]=std::chrono::duration<double,std::milli>(Clock::now()-start).count();
        if(audit){
            levelstore::SealStore seals(out/"log",o.product);uint64_t compared=0,blocks=0;
            // Independent decimal oracle: unbounded integer digits, then cross
            // multiplication; never parse or compare a binary floating value.
            const auto exact=[](const std::string& text,int64_t units,uint32_t digits){
                boost::multiprecision::cpp_int n=0,encoded=units;uint32_t places=0;bool dot=false;
                for(char c:text){if(c=='.'){if(dot)throw std::runtime_error("source grammar");dot=true;continue;}
                    if(c<'0'||c>'9')throw std::runtime_error("source grammar");n=n*10+(c-'0');if(dot)++places;}
                for(uint32_t i=0;i<digits;++i)n*=10;for(uint32_t i=0;i<places;++i)encoded*=10;
                if(n!=encoded)throw std::runtime_error("source/D2 decimal mismatch");
            };
            std::map<int64_t,levelstore::D2Hour> logged;
            levelstore::BookImage image;bool have=false;
            levelstore::LevelLogReader(out/"log",o.product).read([&](const auto& record){
                if(auto* start=std::get_if<levelstore::BookImage>(&record.value)){image=*start;have=true;return;}
                const auto& b=std::get<levelstore::MinuteBlock>(record.value);const auto hour=b.bucketStartMs/3600000*3600000;
                auto& h=logged[hour];if(h.blocks.empty()){if(!have)throw std::runtime_error("no log image");h.start=image;}
                h.blocks.push_back(b);if(h.blocks.size()==60)image=levelstore::endImage(h);
            });
            uint64_t d2Bytes=0,summaryBytes=0;
            for(auto& [hour,h]:logged){const auto decoded=seals.read(hour);if(decoded.timeline!=h)throw std::runtime_error("log/D2 mismatch");
                d2Bytes+=levelstore::encodeD2(decoded.timeline).size();summaryBytes+=levelstore::encodeSummary(decoded.summaries).size();
                std::ifstream expected(auditRoot/(std::to_string(hour)+".txt"));
                for(const auto& b:decoded.timeline.blocks){++blocks;for(const auto& input:b.inputs){int64_t clock;bool bid;std::string price,qty;
                    if(!(expected>>clock>>bid>>price>>qty))throw std::runtime_error("missing source update");
                    const auto& msg=b.messages.at(input.message);
                    if(clock!=msg.timeMs||bid!=input.isBid)throw std::runtime_error("source/D2 order");
                    exact(price,input.units,msg.priceDigits);exact(qty,input.size,msg.sizeDigits);++compared;}}
                std::string extra;if(expected>>extra)throw std::runtime_error("unencoded source update");
            }
            seals.verifyDay(o.fromMs);
            if(compared!=sourceCount||blocks!=1440)throw std::runtime_error("source audit coverage");
            report["exactSourceUpdatesCompared"]=compared;report["losslessMinutesCompared"]=blocks;
            report["D2BytesBeforeOuterCompression"]=d2Bytes;report["hourSummaryBytesBeforeOuterCompression"]=summaryBytes;
        }
        report["minuteSamples"]=minuteSamples;
        for (auto& [name,v] : costs) {
            std::sort(v.begin(),v.end());
            if (!v.empty()) report["timingsMs"][name]={{"count",v.size()},{"p50",v[size_t(.5*(v.size()-1))]},
                {"p99",v[size_t(.99*(v.size()-1))]},{"max",v.back()}};
        }
        uint64_t bytes=0;
        if (!o.levelLogRoot.empty()) for (const auto& e : fs::recursive_directory_iterator(o.levelLogRoot))
            if (e.is_regular_file() && e.path().extension()==".lcl") bytes+=e.file_size();
        report["bytesByExtension"]=json::object();
        for(const auto& e:fs::recursive_directory_iterator(out))if(e.is_regular_file()) {
            const auto ext=e.path().extension().string();report["bytesByExtension"][ext]=report["bytesByExtension"].value(ext,uint64_t(0))+e.file_size();
        }
        report["durableBytesPerProductDay"]=bytes; report["levelAppendBytesWritten"]=files->written;
#ifdef __APPLE__
        rusage peak{}; getrusage(RUSAGE_SELF,&peak); report["peakRssBytes"]=peak.ru_maxrss;
        rusage_info_v4 usage{};
        if (proc_pid_rusage(getpid(),RUSAGE_INFO_V4,reinterpret_cast<rusage_info_t*>(&usage))==0)
            report["processDiskBytesWritten"]=usage.ri_diskio_byteswritten;
#endif
        report["pass"]=point=="measure-prefix" || report["roll"]["days"][0]["committedThroughMs"]==o.toMs;
    } catch (const std::exception& e) { report["error"]=e.what(); }
    std::ofstream(argv[5])<<report.dump(2)<<'\n'; std::cout<<report.dump()<<'\n';
    return report["pass"].get<bool>()?0:1;
}
