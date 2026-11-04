// Offline restart evidence; RAWL2 is opened read-only by the roller.
#include "roller/Roller.hpp"
#include "levelstore/LevelLog.hpp"
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
        if (std::string(argv[8])=="C") { o.levelLogRoot=out/"log"; o.levelLogFiles=files; }
        const std::string point=argv[6]; const int minute=std::stoi(argv[7]);
        bool ready=false;
        int64_t appliedMinute=-1, cpuMinute=-1, cpuBefore=-1;
        files->cut=point=="cut";
        std::map<std::string,std::vector<double>> costs;
        o.levelTiming=[&](const char* name,double ms) { costs[name].push_back(ms); };
        const auto start=Clock::now();
        o.onApplied=[&](const roller::JournalRecord& r) {
            auto ms=r.record.time.systemNs/1'000'000;
            appliedMinute=ms/60'000;
            ready=ms>=o.fromMs+int64_t(minute)*60'000;
            if (ready && point=="append") files->armed=true;

        };
        o.persistencePointForTest=[&](const char* p) {
#ifdef __APPLE__
            if (std::string_view(p)=="checkpoint-synced") {
                const auto cpu=workerCpuNs();
                if (cpu>=0 && cpuBefore>=0 && appliedMinute>cpuMinute)
                    costs["worker-cpu-per-journal-minute"].push_back(double(cpu-cpuBefore)/1e6/double(appliedMinute-cpuMinute));
                cpuBefore=cpu; cpuMinute=appliedMinute;
            }
#endif
            if (std::string_view(p)=="recovery-replayed")
                report["recoveryOpenCutReplayMs"]=std::chrono::duration<double,std::milli>(Clock::now()-start).count();
#ifndef _WIN32
            if (ready && point==p) ::kill(::getpid(),SIGKILL);
#endif
        };
        report["roll"]=roller::roll(o);
        for (auto& [name,v] : costs) {
            std::sort(v.begin(),v.end());
            if (!v.empty()) report["timingsMs"][name]={{"count",v.size()},{"p50",v[size_t(.5*(v.size()-1))]},
                {"p99",v[size_t(.99*(v.size()-1))]},{"max",v.back()}};
        }
        uint64_t bytes=0;
        if (!o.levelLogRoot.empty()) for (const auto& e : fs::recursive_directory_iterator(o.levelLogRoot))
            if (e.is_regular_file() && e.path().extension()==".lcl") bytes+=e.file_size();
        report["durableBytesPerProductDay"]=bytes; report["levelAppendBytesWritten"]=files->written;
#ifdef __APPLE__
        rusage_info_v4 usage{};
        if (proc_pid_rusage(getpid(),RUSAGE_INFO_V4,reinterpret_cast<rusage_info_t*>(&usage))==0)
            report["processDiskBytesWritten"]=usage.ri_diskio_byteswritten;
#endif
        report["pass"]=report["roll"]["days"][0]["committedThroughMs"]==o.toMs;
    } catch (const std::exception& e) { report["error"]=e.what(); }
    std::ofstream(argv[5])<<report.dump(2)<<'\n'; std::cout<<report.dump()<<'\n';
    return report["pass"].get<bool>()?0:1;
}
