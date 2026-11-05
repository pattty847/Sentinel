// Read-only RAWL2 v1/v2 numeric-domain evidence tool. Not a production reader.
#include "levelstore/ExactDecimal.hpp"
#include "Crc32.hpp"
#include <nlohmann/json.hpp>
#include <zstd.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
using namespace sentinel::levelstore;
using nlohmann::json;
namespace fs = std::filesystem;
namespace {
uint64_t le(std::string_view s, size_t pos, size_t count) {
    if (pos + count > s.size()) throw std::runtime_error("truncated integer");
    uint64_t n = 0;
    for (size_t i = 0; i < count; ++i) n |= uint64_t(uint8_t(s[pos+i])) << (8*i);
    return n;
}
std::string read(std::ifstream& in, size_t n) {
    if (n > 64 * 1024 * 1024) throw std::runtime_error("oversized frame");
    std::string b(n, '\0'); in.read(b.data(), n);
    if (size_t(in.gcount()) != n) throw std::runtime_error("truncated read");
    return b;
}
bool greater(ExactDecimal a, ExactDecimal b) {
    if (!a.mantissa || !b.mantissa) return a.mantissa > b.mantissa;
    auto x=std::to_string(a.mantissa), y=std::to_string(b.mantissa);
    const int64_t ax=int64_t(x.size())-a.digits, by=int64_t(y.size())-b.digits;
    if (ax!=by) return ax>by;
    const size_t n=std::max(x.size(),y.size());x.resize(n,'0');y.resize(n,'0');return x>y;
}
struct Field {
    uint64_t values=0, missing=0, integerLiterals=0, otherTypes=0, invalid=0, overflow=0, offGrid=0, scaledOverflow=0;
    size_t maxPlaces=0,maxIntegerDigits=0,maxSignificant=0;
    ExactDecimal largest;
    int64_t maxUnits=0;
    json examples=json::array();
    void add(const json& update, const char* key, ExactDecimal increment, const std::string& position) {
        auto it=update.find(key);
        if (it==update.end()) {++missing;return;}
        if (it->is_number_integer()) {++integerLiterals;return;}
        if (!it->is_string()) {++otherTypes;return;}
        ++values; const auto& text=it->get_ref<const std::string&>();auto d=parseDecimal(text);
        if(d.status!=DecimalStatus::Exact) {
            if(d.status==DecimalStatus::Invalid) ++invalid;else ++overflow;
            if(examples.size()<16) examples.push_back({{"text",text},{"position",position},{"cause",d.status==DecimalStatus::Invalid?"invalid":"overflow"}});
            return;
        }
        const auto dot=text.find('.');auto whole=std::string_view(text).substr(0,dot);
        const auto first=whole.find_first_not_of('0');
        maxIntegerDigits=std::max(maxIntegerDigits,first==whole.npos?size_t(1):whole.size()-first);
        maxPlaces=std::max(maxPlaces,dot==text.npos?size_t(0):text.size()-dot-1);
        maxSignificant=std::max(maxSignificant,std::to_string(d.value.mantissa).size());
        if(greater(d.value,largest)) largest=d.value;
        if(!onDecimalGrid(d.value,increment)) {
            ++offGrid;
            if(examples.size()<16) examples.push_back({{"text",text},{"position",position},{"cause","off-grid"}});
        }
        auto u=decimalAtScale(d.value,std::max(increment.digits,d.value.digits));
        if(!u) ++scaledOverflow;else maxUnits=std::max(maxUnits,*u);
    }
    json result() const {
        return {{"values",values},{"missing",missing},{"integerLiterals",integerLiterals},{"otherTypes",otherTypes},
          {"invalid",invalid},{"overflow",overflow},{"offGrid",offGrid},{"scaledOverflow",scaledOverflow},
          {"maxDecimalPlaces",maxPlaces},{"maxIntegerDigits",maxIntegerDigits},{"maxSignificantDigits",maxSignificant},
          {"largest",decimalText(largest)},{"maxUnitsAtApplicableOrFinerScale",maxUnits},{"examples",examples}};
    }
};
struct Product {
    uint64_t files=0,blocks=0,records=0,malformed=0,updates=0,foreign=0;
    Field price,size;std::set<std::string> days,versions,increments;
    json result() const {
        return {{"files",files},{"blocks",blocks},{"records",records},{"malformedJson",malformed},{"updates",updates},
          {"foreignEvents",foreign},{"productDays",days.size()},{"versions",versions},{"increments",increments},
          {"price",price.result()},{"size",size.result()}};
    }
};
void survey(const fs::path& path, const std::string& product, Product& out) {
    std::ifstream in(path,std::ios::binary);const auto prefix=read(in,16);
    if(prefix.substr(0,7)!="RAWL2\r\n" || (prefix[7]!=1&&prefix[7]!=2)) throw std::runtime_error("magic/version");
    const auto rawHeader=read(in,le(prefix,8,4));
    if(sentinel::crc32(rawHeader.data(),rawHeader.size())!=le(prefix,12,4)) throw std::runtime_error("header CRC");
    const auto header=json::parse(rawHeader);const auto& meta=header.at("product_metadata");
    const auto ptext=meta.at("price_increment").get<std::string>(), qtext=meta.at("base_increment").get<std::string>();
    const auto pi=parseDecimal(ptext),qi=parseDecimal(qtext);
    if(pi.status!=DecimalStatus::Exact||qi.status!=DecimalStatus::Exact||!pi.value.mantissa||!qi.value.mantissa) throw std::runtime_error("increment");
    out.increments.insert(ptext+" / "+qtext);out.versions.insert(std::to_string(int(prefix[7])));out.days.insert(path.parent_path().string());++out.files;
    while(in.peek()!=EOF) {
        const auto magic=read(in,4);
        if(magic=="IDX1") return; // Domain survey does not validate the closing index.
        if(magic!="BLK1") throw std::runtime_error("block magic");
        const auto h=magic+read(in,44);
        if(sentinel::crc32(h.data(),44)!=le(h,44,4)) throw std::runtime_error("block header CRC");
        const size_t rawBytes=le(h,8,4);if(rawBytes>32*1024*1024) throw std::runtime_error("raw size");
        const auto compressed=read(in,le(h,4,4));std::string raw(rawBytes,'\0');
        const auto n=ZSTD_decompress(raw.data(),raw.size(),compressed.data(),compressed.size());
        if(ZSTD_isError(n)||n!=rawBytes) throw std::runtime_error("zstd decode");
        if(sentinel::crc32(raw.data(),raw.size())!=le(h,40,4)) throw std::runtime_error("raw CRC");
        ++out.blocks;size_t at=0;uint64_t records=0;
        while(at<raw.size()) {
            const auto length=le(raw,at,4),kind=le(raw,at+4,4);
            if(length<28||length+4>raw.size()-at) throw std::runtime_error("record framing");
            const auto payload=std::string_view(raw).substr(at+32,length-28);at+=length+4;++out.records;++records;
            if(kind!=1) continue;
            const auto j=json::parse(payload,nullptr,false);
            if(j.is_discarded()) {++out.malformed;continue;}
            if(!j.is_object()||j.value("channel","")!="l2_data"||!j.contains("events")||!j["events"].is_array()) continue;
            for(const auto& ev:j["events"]) {
                if(!ev.is_object()) continue;
                if(ev.value("product_id","")!=product) {++out.foreign;continue;}
                if(!ev.contains("updates")||!ev["updates"].is_array()) continue;
                const std::string position=path.string()+":"+std::to_string(le(h,32,8))+":"+std::to_string(records-1);
                for(const auto& update:ev["updates"]) {
                    if(!update.is_object()) {++out.malformed;continue;}
                    ++out.updates;out.price.add(update,"price_level",pi.value,position);out.size.add(update,"new_quantity",qi.value,position);
                }
            }
        }
        if(records!=le(h,12,4)) throw std::runtime_error("record count");
    }
}
}
int main(int argc,char** argv) {
    if(argc!=3) return 2;
    const auto start=std::chrono::steady_clock::now();const fs::path root=argv[1],dest=argv[2];
    std::vector<fs::path> paths;for(const auto& e:fs::recursive_directory_iterator(root))
        if(e.is_regular_file()&&e.path().extension()==".rawl2"&&!e.path().filename().string().starts_with("._")) paths.push_back(e.path());
    std::sort(paths.begin(),paths.end());std::map<std::string,Product> products;json errors=json::array();size_t done=0;
    for(const auto& path:paths) {
        const auto product=fs::relative(path,root).begin()->string();
        try {survey(path,product,products[product]);}
        catch(const std::exception& e) {errors.push_back({{"file",path.string()},{"reason",e.what()}});}
        ++done;
        if(done%24==0||done==paths.size()) {
            json j={{"filesInventoried",paths.size()},{"filesAttempted",done},{"errors",errors},{"seconds",std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count()}};
            for(const auto& [name,p]:products) j["products"][name]=p.result();
            std::ofstream(dest.string()+".tmp")<<j.dump(2)<<'\n';fs::rename(dest.string()+".tmp",dest);
            std::cout<<done<<"/"<<paths.size()<<" files; "<<j["seconds"]<<" seconds; "<<errors.size()<<" file errors"<<std::endl;
        }
    }
}
