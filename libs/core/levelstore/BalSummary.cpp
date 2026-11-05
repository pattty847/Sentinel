// Sparse per-bin accumulation and the bal1m ladder follow DataLab summary.cpp.
// Integer scales, checked wide sums and rational band bounds replace its fixed
// quantity scale and binary floating point band rounding.
#include "BalSummary.hpp"
#include "Crc32.hpp"
#include <map>
#include <algorithm>
#include <stdexcept>
namespace sentinel::levelstore {
namespace {
void check(bool ok,const char* reason) {if(!ok)throw std::runtime_error(reason);}
Wide factor(uint32_t from,uint32_t to) {check(to>=from,"summary scale regression");Wide f=1;while(from++<to)f*=10;return f;}
int64_t narrow(const Wide& value) {if(value<0||value>INT64_MAX)throw std::overflow_error("summary integer overflow");return value.convert_to<int64_t>();}
void putWide(ByteWriter& w,Wide n) {check(n>=0,"negative summary scalar");while(n>=128){w.u8(static_cast<uint8_t>((n&127).convert_to<unsigned>())|128);n>>=7;}w.u8(n.convert_to<uint8_t>());}
Wide getWide(ByteReader& r) {Wide n=0;for(unsigned i=0;i<19;++i){const auto b=r.u8();if(i==18)check((b&126)==0,"summary wide overflow");n|=Wide(b&127)<<(7*i);if(!(b&128))return n;}throw std::runtime_error("summary wide varint");}
struct MidRange { Wide lo=0,hi=0;bool set=false;uint32_t digits=0; };
// Midpoint truncates to the current native price unit, matching the lab rule.
std::vector<MidRange> midRanges(const std::vector<D2Hour>& hours) {
    std::vector<MidRange> out;
    for(const auto& h:hours) {
        std::map<int64_t,int64_t> book[2];auto pd=h.start.priceDigits;auto qd=h.start.sizeDigits;
        for(const auto& l:h.start.levels)book[l.isBid][l.units]=l.size;
        for(const auto& b:h.blocks) {
            MidRange range;range.digits=pd;
            const auto note=[&]{
                if(book[0].empty()&&book[1].empty())return;
                const Wide mid=book[0].empty()?Wide(book[1].rbegin()->first):book[1].empty()?Wide(book[0].begin()->first):
                    (Wide(book[1].rbegin()->first)+book[0].begin()->first)/2;
                if(!range.set){range.lo=range.hi=mid;range.set=true;}else{range.lo=std::min(range.lo,mid);range.hi=std::max(range.hi,mid);}
            };
            note();size_t change=0;
            for(size_t i=0;i<b.messages.size();++i) {
                const auto& m=b.messages[i];
                if(m.reset||m.clear!=ClearCause::None){book[0].clear();book[1].clear();}
                if(m.priceDigits!=pd||m.sizeDigits!=qd) {
                    range.lo*=factor(pd,m.priceDigits);range.hi*=factor(pd,m.priceDigits);range.digits=m.priceDigits;
                    for(auto& side:book){std::map<int64_t,int64_t> next;for(const auto& [p,q]:side){
                        const auto px=decimalAtScale({p,pd},m.priceDigits),qty=decimalAtScale({q,qd},m.sizeDigits);
                        if(!px||!qty)throw std::overflow_error("summary live scale overflow");next[*px]=*qty;}side.swap(next);}
                    pd=m.priceDigits;qd=m.sizeDigits;
                }
                while(change<b.changes.size()&&b.changes[change].message==i){const auto& c=b.changes[change++];
                    if(c.size)book[c.isBid][c.units]=c.size;else book[c.isBid].erase(c.units);}
                note();
            }
            out.push_back(std::move(range));
        }
    }
    return out;
}
}
std::vector<SummaryLevel> bal1mLevels(const std::string& product) {
    std::vector<SummaryLevel> out;
    const auto add=[&](int minutes,int width,int band=0,bool twap=false,uint32_t digits=0){out.push_back({int64_t(minutes)*60000,{width,digits},uint32_t(band),twap});};
    if(product=="PEPE-USD") {
        add(1,1,3000,true,8);add(5,1,0,true,8);
        for(int t:{15,60,240,1440})for(int r:{1,2,5,10})add(t,r,0,false,8);
    } else {
        add(1,1,300,true);add(5,1,500,true);
        for(int r:{5,10,20})add(5,r,800);add(5,25,500);
        add(15,1,1000,true);for(int r:{5,10,20,25})add(15,r,1000);
        for(int r:{50,100})add(15,r);
        for(int t:{60,1440})for(int r:{5,10,20,25,50,100})add(t,r);
        for(int r:{20,25,50,100})add(240,r);
    }
    return out;
}
std::vector<SummaryNode> buildSummary(const std::vector<D2Hour>& hours,const std::vector<SummaryLevel>& levels) {
    if(hours.empty())return {};
    const auto mids=midRanges(hours);std::vector<SummaryNode> result;
    for(const auto& level:levels) {
        check(level.durationMs>0&&level.durationMs%60000==0&&level.bandBp<10000,"summary level");
        auto start=hours.front().start;
        const auto resolution=decimalAtScale(level.resolution,start.priceDigits);
        if(!resolution||*resolution<=0)throw std::overflow_error("summary resolution not representable");
        MinuteSweep sweep(*resolution,false);sweep.start(start);
        SummaryNode node;std::map<std::pair<bool,int64_t>,BinRow> rows;MidRange mid;size_t index=0;
        int64_t expected=start.atMs;
        for(const auto& h:hours)for(const auto& b:h.blocks) {
            check(b.bucketStartMs==expected,"summary incomplete minutes");expected+=60000;
            if(b.bucketStartMs%level.durationMs==0) {
                node={level,b.bucketStartMs,0,b.priceDigits,b.sizeDigits,0,0,{}};rows.clear();mid={};mid.digits=b.priceDigits;
            }
            std::vector<BinRow> minute;int64_t covered=0;sweep.apply(b,minute,covered);
            const auto pd=b.messages.empty()?b.priceDigits:b.messages.back().priceDigits;
            const auto qd=b.messages.empty()?b.sizeDigits:b.messages.back().sizeDigits;
            const auto qfactor=factor(node.sizeDigits,qd);
            for(auto& [key,row]:rows){row.integral*=qfactor;row.peak*=qfactor;row.last=0;}
            for(const auto& row:minute){auto& target=rows[{row.isBid,row.bin}];target.isBid=row.isBid;target.bin=row.bin;
                target.integral+=row.integral;target.presenceMs+=row.presenceMs;target.peak=std::max(target.peak,row.peak);target.last=row.last;}
            node.coveredMs+=covered;node.priceDigits=pd;node.sizeDigits=qd;
            const auto& mr=mids.at(index++);mid.lo*=factor(mid.digits,pd);mid.hi*=factor(mid.digits,pd);mid.digits=pd;
            if(mr.set){if(!mid.set){mid=mr;}else{mid.lo=std::min(mid.lo,mr.lo);mid.hi=std::max(mid.hi,mr.hi);}mid.set=true;}
            if(expected%level.durationMs)continue;
            if(node.startMs%level.durationMs||expected-node.startMs!=level.durationMs)continue;
            if(level.bandBp&&mid.set) {
                const auto r=decimalAtScale(level.resolution,pd);if(!r||*r<=0)throw std::overflow_error("summary resolution overflow");
                const Wide low=mid.lo*(10000-level.bandBp)/10000;
                const Wide high=(mid.hi*(10000+level.bandBp)+9999)/10000;
                node.bandLo=narrow(low/ *r);node.bandHi=narrow(high/ *r+1);
            }
            for(auto& [key,row]:rows)if(!level.bandBp||(row.bin>=node.bandLo&&row.bin<node.bandHi)) {
                if(level.twapOnly){row.peak=0;row.presenceMs=0;}node.rows.push_back(row);
            }
            std::sort(node.rows.begin(),node.rows.end(),[](const auto& a,const auto& b){return a.isBid!=b.isBid?a.isBid:a.bin<b.bin;});
            result.push_back(std::move(node));
        }
    }
    return result;
}
std::vector<uint8_t> encodeSummary(const std::vector<SummaryNode>& nodes) {
    ByteWriter w;w.fixed32(0x344e5343);w.fixed32(4);w.varint(nodes.size());
    for(const auto& n:nodes){w.svarint(n.startMs);w.varint(n.level.durationMs);w.varint(n.level.resolution.mantissa);w.fixed32(n.level.resolution.digits);
        w.fixed32(n.level.bandBp);w.u8(n.level.twapOnly);w.varint(n.coveredMs);w.fixed32(n.priceDigits);w.fixed32(n.sizeDigits);
        w.varint(n.bandLo);w.varint(n.bandHi);w.varint(n.rows.size());
        int64_t previous=0;bool side=true;
        for(const auto& r:n.rows){
            if(r.isBid!=side){previous=0;side=r.isBid;}
            check(!n.level.twapOnly||(r.peak==0&&r.presenceMs==0),"TWAP-only metrics");
            const bool constant=n.coveredMs>0&&r.integral==r.last*n.coveredMs&&
                (n.level.twapOnly||(r.peak==r.last&&r.presenceMs==(r.last>0?n.coveredMs:0)));
            w.u8((r.isBid?1:0)|(constant?2:0));w.svarint(r.bin-previous);previous=r.bin;
            if(constant)putWide(w,r.last);
            else {putWide(w,r.integral);if(!n.level.twapOnly){putWide(w,r.peak);w.varint(r.presenceMs);}putWide(w,r.last);}
        }
    }
    w.fixed32(sentinel::crc32(w.bytes.data(),w.bytes.size()));return std::move(w.bytes);
}
std::vector<SummaryNode> decodeSummary(std::span<const uint8_t> bytes) {
    check(bytes.size()>=12,"summary truncated");ByteReader footer(bytes.last(4));check(footer.fixed32()==sentinel::crc32(bytes.data(),bytes.size()-4),"summary CRC");
    ByteReader r(bytes.first(bytes.size()-4));check(r.fixed32()==0x344e5343&&r.fixed32()==4,"summary version");const auto count=r.varint();check(count<=r.remaining(),"summary node count");
    std::vector<SummaryNode> result;
    for(uint64_t i=0;i<count;++i){SummaryNode n;n.startMs=r.svarint();n.level.durationMs=narrow(Wide(r.varint()));n.level.resolution.mantissa=narrow(Wide(r.varint()));n.level.resolution.digits=r.fixed32();
        n.level.bandBp=r.fixed32();const auto tw=r.u8();check(tw<=1,"summary flags");n.level.twapOnly=tw;n.coveredMs=narrow(Wide(r.varint()));n.priceDigits=r.fixed32();n.sizeDigits=r.fixed32();
        n.bandLo=narrow(Wide(r.varint()));n.bandHi=narrow(Wide(r.varint()));const auto rows=r.varint();check(rows<=r.remaining(),"summary row count");
        int64_t previous=0;bool side=true;
        for(uint64_t j=0;j<rows;++j){
            BinRow row;const auto flags=r.u8();check(flags<=3,"summary flags");row.isBid=flags&1;
            if(row.isBid!=side){previous=0;side=row.isBid;}
            row.bin=checkedAdd(previous,r.svarint(),"summary bin overflow");previous=row.bin;check(row.bin>=0,"summary bin");
            if(flags&2){row.last=getWide(r);row.integral=row.last*n.coveredMs;
                if(!n.level.twapOnly){row.peak=row.last;row.presenceMs=row.last>0?n.coveredMs:0;}}
            else {row.integral=getWide(r);if(!n.level.twapOnly){row.peak=getWide(r);row.presenceMs=narrow(Wide(r.varint()));}row.last=getWide(r);}
            n.rows.push_back(std::move(row));
        }
        result.push_back(std::move(n));
    }
    check(r.done(),"summary trailing bytes");check(encodeSummary(result)==std::vector<uint8_t>(bytes.begin(),bytes.end()),"summary noncanonical");return result;
}
}
