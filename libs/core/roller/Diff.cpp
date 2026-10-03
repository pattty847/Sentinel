#include "ShadowRoller.hpp"
#include <map>
#include <set>
#include <tuple>
namespace sentinel::roller {
using recording::Hmc2Record;
namespace {
bool same(const Hmc2Record& a,const Hmc2Record& b,bool strictJournal) {
    const auto key = [strictJournal](const Hmc2Record& r) {
        return std::tuple(r.header.priceScale,r.header.rowTickUnits,r.header.sizeScale.floor,r.header.sizeScale.codesPerOctave,
            r.observedMs,strictJournal ? r.flags : r.flags & ~recording::kLateEvents,r.bidRowLo,r.bidRowHi,r.askRowLo,r.askRowHi,
            r.midOpen,r.midClose,r.midMin,r.midMax);
    };
    if (key(a) != key(b) || a.entries.size() != b.entries.size() || a.coverage.size() != b.coverage.size()) return false;
    for (size_t i=0;i<a.entries.size();++i) {
        const auto& x=a.entries[i]; const auto& y=b.entries[i];
        if (std::tie(x.row,x.isAsk,x.twapCode,x.peakCode,x.coveredMs) != std::tie(y.row,y.isAsk,y.twapCode,y.peakCode,y.coveredMs)) return false;
    }
    for (size_t i=0;i<a.coverage.size();++i) {
        const auto& x=a.coverage[i]; const auto& y=b.coverage[i];
        if (std::tie(x.lo,x.hi,x.isAsk,x.coveredMs) != std::tie(y.lo,y.hi,y.isAsk,y.coveredMs)) return false;
    }
    return true;
}
}
nlohmann::json diff(const std::filesystem::path& a,const std::filesystem::path& b,
                    const std::string& product,const std::string& layer,int64_t from,int64_t to,int64_t tf,bool strictJournal) {
    if (tf != 60'000 && tf != 3'600'000) throw std::runtime_error("diff supports minute/hour levels");
    const auto left = recording::Hmc2Store::readRange(a,product,layer,tf,from,to);
    const auto right = recording::Hmc2Store::readRange(b,product,layer,tf,from,to);
    std::map<int64_t,const Hmc2Record*> l,r;
    for (const auto& x:left) l[x.bucketStartMs]=&x;
    for (const auto& x:right) r[x.bucketStartMs]=&x;
    uint64_t qualifies=0,matches=0,mismatches=0,excluded=0;
    nlohmann::json details=nlohmann::json::array();
    std::set<int64_t> qualifyingMinutes;
    if (tf == 3'600'000 && !strictJournal) {
        // An hour's observation sum alone cannot prove every constituent exists
        // on both sides. Reuse the minute qualification list (including mismatch).
        const auto minutes = diff(a,b,product,layer,from,to,60'000);
        for (const auto& d:minutes["minutes"]) if (d.at("qualifies").get<bool>()) qualifyingMinutes.insert(d.at("bucketMs").get<int64_t>());
    }
    for (auto t=from; t<to; t+=tf) {
        std::string reason;
        if (!l.contains(t) || !r.contains(t)) reason="missing";
        else if (!strictJournal && (l[t]->observedMs != tf || r[t]->observedMs != tf)) reason="partial observation";
        else if (!strictJournal && ((l[t]->flags | r[t]->flags) & recording::kResynced)) reason="resynced";
        if (reason.empty() && tf == 3'600'000 && !strictJournal)
            for (auto m=t;m<t+tf;m+=60'000) if (!qualifyingMinutes.contains(m)) { reason="nonqualifying constituent"; break; }
        const bool qualifiesHere=reason.empty();
        if (qualifiesHere) { ++qualifies; if (same(*l[t],*r[t],strictJournal)) { ++matches; reason="match"; } else { ++mismatches; reason="mismatch"; } }
        else if (strictJournal && (l.contains(t) != r.contains(t))) { ++mismatches; reason="missing counterpart"; }
        else ++excluded;
        details.push_back({{"bucketMs",t},{"qualifies",qualifiesHere},{"result",reason},
            {"observedA",l.contains(t)?l[t]->observedMs:0},{"observedB",r.contains(t)?r[t]->observedMs:0}});
        if (qualifiesHere) {
            const auto& a=*l[t]; const auto& b=*r[t];
            std::map<std::pair<int64_t,bool>,std::pair<int,int>> codesA,codesB;
            for(const auto& e:a.entries) codesA[{e.row,e.isAsk}]={e.twapCode,e.peakCode};
            for(const auto& e:b.entries) codesB[{e.row,e.isAsk}]={e.twapCode,e.peakCode};
            nlohmann::json histogram=nlohmann::json::object(), totals=nlohmann::json::object();
            for (bool ask : {false,true}) {
                long double totalA=0,totalB=0;
                for (const auto& e:a.entries) if (e.isAsk==ask) totalA+=recording::decodeSize(e.twapCode,a.header.sizeScale);
                for (const auto& e:b.entries) if (e.isAsk==ask) totalB+=recording::decodeSize(e.twapCode,b.header.sizeScale);
                const auto delta=totalB-totalA;
                totals[ask ? "ask" : "bid"]={{"a",double(totalA)},{"b",double(totalB)},{"delta",double(delta)},
                    {"relativeDelta",totalA ? nlohmann::json(double(delta/totalA)) : nlohmann::json(nullptr)}};
            }
            uint64_t onlyA=0,onlyB=0,twap=0,peak=0;int maxTwap=0;
            for(const auto& [key,value]:codesA) {
                const auto it=codesB.find(key);
                if(it==codesB.end()) {++onlyA;continue;}
                const auto delta=std::to_string(it->second.first-value.first);
                histogram[delta]=histogram.value(delta,uint64_t{0})+1;
                twap+=value.first!=it->second.first;peak+=value.second!=it->second.second;
                maxTwap=std::max(maxTwap,std::abs(value.first-it->second.first));
            }
            for(const auto& [key,value]:codesB) if(!codesA.contains(key))++onlyB;
            details.back()["difference"]={{"entriesA",a.entries.size()},{"entriesB",b.entries.size()},
                {"twapCodeDeltaHistogram",histogram},{"totalTwap",totals},
                {"onlyA",onlyA},{"onlyB",onlyB},{"twapCodes",twap},{"peakCodes",peak},{"maxTwapCodeDelta",maxTwap},
                {"midsA",{a.midOpen,a.midClose,a.midMin,a.midMax}},{"midsB",{b.midOpen,b.midClose,b.midMin,b.midMax}},
                {"midsEqual",std::tie(a.midOpen,a.midClose,a.midMin,a.midMax)==std::tie(b.midOpen,b.midClose,b.midMin,b.midMax)},
                {"boundsEqual",std::tie(a.bidRowLo,a.bidRowHi,a.askRowLo,a.askRowHi)==std::tie(b.bidRowLo,b.bidRowHi,b.askRowLo,b.askRowHi)}};
        }
    }
    return {{"product",product},{"layer",layer},{"tfMs",tf},{"qualifying",qualifies},{"matching",matches},
            {"mismatching",mismatches},{"nonqualifying",excluded},{"minutes",details}};
}
nlohmann::json compareShadow(const std::filesystem::path& shadow,const std::filesystem::path& batch,
                            const std::filesystem::path& primary,const std::string& product,
                            const std::string& layer,int64_t from,int64_t to,metrics::Counter& mismatch) {
    const auto strict=diff(shadow,batch,product,layer,from,to,60'000,true);
    uint64_t hourMismatching=0;
    if(layer=="deep" && from%3600000==0 && to%3600000==0)
        hourMismatching=diff(shadow,batch,product,layer,from,to,3600000,true).at("mismatching").get<uint64_t>();
    mismatch.inc(strict.at("mismatching").get<uint64_t>()+hourMismatching);
    const auto cross=diff(primary,shadow,product,layer,from,to);
    uint64_t within=0;
    for(const auto& minute:cross["minutes"]) if(minute["qualifies"].get<bool>()) {
        const auto& d=minute["difference"];
        bool pass=true;
        for(const auto* side:{"bid","ask"}) {
            const auto& v=d["totalTwap"][side]["relativeDelta"];
            pass=pass && !v.is_null() && std::abs(v.get<double>())<=.005;
        }
        for(size_t i=0;i<4;++i) {
            const auto a=d["midsA"][i].get<double>(),b=d["midsB"][i].get<double>();
            pass=pass && a>0 && std::abs(b-a)/a<=.0002;
        }
        const auto a=d["entriesA"].get<double>(),b=d["entriesB"].get<double>();
        const auto onlyA=d["onlyA"].get<double>(),onlyB=d["onlyB"].get<double>();
        pass=pass && a>0 && std::abs(b-a)/a<=.01 && (a-onlyA)/(a+onlyB)>=.99;
        within+=pass;
    }
    return {{"product",product},{"layer",layer},{"fromMs",from},{"toMs",to},
        {"strictHourMismatching",hourMismatching},{"strictMismatching",strict["mismatching"]},{"strictMatching",strict["matching"]},
        {"crossConnectionQualifying",cross["qualifying"]},{"crossConnectionWithinBands",within},
        {"crossConnectionInformational",true}};
}
} // namespace sentinel::roller
