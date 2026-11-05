#include "SealStore.hpp"
#include "LevelLog.hpp"
#include "Crc32.hpp"
#include "servermodel/PersistenceIo.hpp"
#include <zstd.h>
#include <fstream>
#include <map>
#include <stdexcept>
namespace sentinel::levelstore {
namespace fs=std::filesystem;
namespace {
constexpr int64_t Hour=3600000,Day=86400000;
void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
void sync(const fs::path& path,bool directory){int error=0;check(directory?persistence::syncDirectory(path,error):persistence::syncFilePath(path,error),"seal fsync failure");}
std::vector<uint8_t> load(const fs::path& path){std::ifstream in(path,std::ios::binary);check(bool(in),"seal missing");std::vector<uint8_t> result{std::istreambuf_iterator<char>(in),{}};check(!in.bad(),"seal read failure");return result;}
void bytes(ByteWriter& w,std::span<const uint8_t> data){w.varint(data.size());w.bytes.insert(w.bytes.end(),data.begin(),data.end());}
std::vector<uint8_t> bytes(ByteReader& r){const auto n=r.varint();check(n<=r.remaining(),"seal section length");std::vector<uint8_t> result(n);for(auto& b:result)b=r.u8();return result;}
std::vector<uint8_t> pack(std::span<const uint8_t> raw){std::vector<uint8_t> compressed(ZSTD_compressBound(raw.size()));auto n=ZSTD_compress(compressed.data(),compressed.size(),raw.data(),raw.size(),3);check(!ZSTD_isError(n),"seal compress");compressed.resize(n);ByteWriter w;w.fixed32(0x34534c43);w.fixed32(4);w.fixed64(raw.size());bytes(w,compressed);w.fixed32(sentinel::crc32(w.bytes.data(),w.bytes.size()));return std::move(w.bytes);}
std::vector<uint8_t> unpack(std::span<const uint8_t> data){check(data.size()>=20,"seal truncated");ByteReader tail(data.last(4));check(tail.fixed32()==sentinel::crc32(data.data(),data.size()-4),"seal CRC");ByteReader r(data.first(data.size()-4));check(r.fixed32()==0x34534c43&&r.fixed32()==4,"seal version");auto n=r.fixed64();check(n<=1024ULL*1024*1024,"seal decompression bound");auto stored=bytes(r);check(r.done(),"seal trailing bytes");std::vector<uint8_t> raw(n);auto decoded=ZSTD_decompress(raw.data(),raw.size(),stored.data(),stored.size());check(!ZSTD_isError(decoded)&&decoded==n,"seal decompression");return raw;}
std::vector<SummaryLevel> levels(const std::string& symbol,bool daily){auto v=bal1mLevels(symbol);std::erase_if(v,[&](const auto& l){return (l.durationMs>Hour)!=daily;});return v;}
}
SealStore::SealStore(fs::path root,std::string product,Point point):root_(std::move(root)),dir_(root_/product/"seals"),product_(std::move(product)),point_(std::move(point)){}
void SealStore::point(const char* name) const {if(point_)point_(name);}
void SealStore::atomic(const fs::path& path,std::span<const uint8_t> data,const char* kind){
    fs::create_directories(dir_);sync(dir_.parent_path(),true);
    const auto tmp=fs::path(path.string()+".tmp");
    {std::ofstream out(tmp,std::ios::binary|std::ios::trunc);out.exceptions(std::ios::badbit|std::ios::failbit);
        const auto half=data.size()/2;out.write(reinterpret_cast<const char*>(data.data()),half);out.flush();point("seal-mid-write");
        out.write(reinterpret_cast<const char*>(data.data()+half),data.size()-half);out.close();}
    point(kind);point("seal-written");sync(tmp,false);point("seal-file-synced");fs::rename(tmp,path);point("seal-renamed");sync(dir_,true);point("seal-directory-synced");
    check(load(path)==std::vector<uint8_t>(data.begin(),data.end()),"seal read-back bytes");point("seal-read-back");
}
SealedHour SealStore::read(int64_t hour) const {
    const auto raw=unpack(load(dir_/(std::to_string(hour)+".hour")));ByteReader r(raw);
    check(r.str(256)==product_&&r.svarint()==hour,"seal identity");const auto d2=bytes(r),summary=bytes(r);check(r.done(),"seal suffix");
    SealedHour result{decodeD2(d2),decodeSummary(summary)};
    check(result.timeline.start.atMs==hour&&result.timeline.blocks.size()==60,"seal incomplete hour");
    check(buildSummary({result.timeline},levels(product_,false))==result.summaries,"seal summary disagrees with D2");return result;
}
void SealStore::publish(const D2Hour& h){
    point("seal-hour-begin");
    check(h.start.atMs%Hour==0&&h.blocks.size()==60&&h.blocks.back().bucketStartMs==h.start.atMs+Hour-60000,"seal requires full hour");
    const auto d2=encodeD2(h);const auto restored=decodeD2(d2);check(restored==h,"D2 lossless verification failed");
    const auto summary=buildSummary({restored},levels(product_,false));
    ByteWriter w;w.str(product_);w.svarint(h.start.atMs);bytes(w,d2);bytes(w,encodeSummary(summary));const auto data=pack(w.bytes);
    const auto path=dir_/(std::to_string(h.start.atMs)+".hour");
    bool valid=false;if(fs::exists(path)){try{valid=read(h.start.atMs)==SealedHour{h,summary};}catch(const std::exception&) {}}
    if(!valid)atomic(path,data,"seal-segment-written");
    check(read(h.start.atMs)==SealedHour{h,summary},"seal read-back lossless failure");
    ByteWriter marker;marker.fixed32(4);marker.fixed64(data.size());marker.fixed32(sentinel::crc32(data.data(),data.size()));
    atomic(dir_/(std::to_string(h.start.atMs)+".verified"),marker.bytes,"seal-marked-verified");
    point("seal-hour-end");
    if((h.start.atMs+Hour)%Day==0)publishDay(h.start.atMs/Day*Day);
}
void SealStore::publishDay(int64_t day){
    point("seal-day-begin");
    std::vector<D2Hour> hours;ByteWriter manifest;manifest.str(product_);manifest.svarint(day);
    for(int i=0;i<24;++i){const auto time=day+i*Hour;const auto path=dir_/(std::to_string(time)+".hour");if(!fs::exists(path))return;
        auto h=read(time);if(!hours.empty())check(endImage(hours.back())==h.timeline.start,"seal hour discontinuity");hours.push_back(std::move(h.timeline));
        const auto data=load(path);manifest.fixed64(data.size());manifest.fixed32(sentinel::crc32(data.data(),data.size()));}
    bytes(manifest,encodeSummary(buildSummary(hours,levels(product_,true))));const auto encoded=pack(manifest.bytes);
    atomic(dir_/(std::to_string(day)+".day"),encoded,"seal-day-manifest-written");check(unpack(load(dir_/(std::to_string(day)+".day")))==manifest.bytes,"day manifest read-back");point("seal-day-end");
}
void SealStore::verifyDay(int64_t day) const {
    const auto raw=unpack(load(dir_/(std::to_string(day)+".day")));ByteReader r(raw);
    check(r.str(256)==product_&&r.svarint()==day,"day seal identity");std::vector<D2Hour> hours;
    for(int i=0;i<24;++i){const auto time=day+i*Hour;const auto data=load(dir_/(std::to_string(time)+".hour"));
        check(r.fixed64()==data.size()&&r.fixed32()==sentinel::crc32(data.data(),data.size()),"day seal hour mismatch");
        auto h=read(time);if(!hours.empty())check(endImage(hours.back())==h.timeline.start,"day continuity");hours.push_back(std::move(h.timeline));}
    const auto summary=bytes(r);check(r.done(),"day seal suffix");
    check(decodeSummary(summary)==buildSummary(hours,levels(product_,true)),"day summaries disagree with D2");
}
void SealStore::requireRetained(int64_t minute) const {const auto path=dir_/"retained";if(!fs::exists(path))return;const auto raw=unpack(load(path));ByteReader r(raw);check(minute>=r.svarint(),"C restart requires pruned log prefix; rebuild from RAWL2 into a new output root");check(r.done(),"retention marker suffix");}
void SealStore::cut(int64_t minute){
    requireRetained(minute);if(!fs::exists(dir_))return;
    const auto hour=minute/Hour*Hour,day=minute/Day*Day;
    for(const auto& e:fs::directory_iterator(dir_)){
        const auto ext=e.path().extension();if(ext!=".hour"&&ext!=".verified"&&ext!=".day"&&ext!=".tmp")continue;
        if(ext==".tmp"){fs::remove(e.path());continue;}
        const auto time=std::stoll(e.path().stem().string());if(time>=(ext==".day"?day:hour)){fs::remove(e.path());sync(dir_,true);point("seal-cut");}}
}
void SealStore::recoverRetention(){
    const auto path=dir_/"retained";if(!fs::exists(path))return;
    const auto raw=unpack(load(path));ByteReader r(raw);const auto before=r.svarint();check(r.done(),"retention marker suffix");
    prune(before,0);
}
void SealStore::prune(int64_t through,int64_t windowMs){
    check(windowMs>=0,"negative C retention");const auto before=(through-windowMs)/Day*Day;if(before<=0)return;
    // Verify every eligible hour before deleting any log. Images are pinned by
    // retaining the entire UTC day containing the configured window's boundary.
    std::vector<fs::path> removable;
    std::map<int64_t,std::vector<fs::path>> days;
    for(const auto& e:fs::directory_iterator(root_/product_))if(e.path().extension()==".lcl"){
        const auto hourNumber=std::stoll(e.path().stem().string());
        check(hourNumber>=0&&hourNumber<=INT64_MAX/Hour,"retention hour name");
        const auto hour=hourNumber*Hour;if(hour>=before)continue;
        check(LevelLogReader::hourPath(root_,product_,hour)==e.path(),"noncanonical retention path");
        days[hour/Day*Day].push_back(e.path());
    }
    for(const auto& [day,paths]:days){
        bool complete=true;
        for(const auto& path:paths){const auto hour=std::stoll(path.stem().string())*Hour;
            if(!fs::exists(dir_/(std::to_string(hour)+".verified")))complete=false;}
        if(!complete)continue;
        for(const auto& path:paths){
            const auto hour=std::stoll(path.stem().string())*Hour;
            (void)read(hour);const auto data=load(dir_/(std::to_string(hour)+".hour"));
            const auto mark=load(dir_/(std::to_string(hour)+".verified"));ByteReader marker(mark);
            check(marker.fixed32()==4&&marker.fixed64()==data.size()&&marker.fixed32()==sentinel::crc32(data.data(),data.size())&&marker.done(),"retention seal marker mismatch");
            removable.push_back(path);
        }
    }
    if(removable.empty())return;
    ByteWriter retained;retained.svarint(before);atomic(dir_/"retained",pack(retained.bytes),"seal-retention-fence");
    for(const auto& path:removable){point("seal-before-prune");check(fs::remove(path),"C log prune failed");sync(root_/product_,true);point("seal-log-pruned");}
}
}
