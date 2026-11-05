// Run accumulation and price-page layout adapted from Sentinel-DataLab d2.cpp.
// Side is separate from the full int64 decimal key; quantities use each event's
// recorded scale. The metadata section preserves recorder and source semantics.
#include "D2Codec.hpp"
#include "ExactDecimal.hpp"
#include "Crc32.hpp"
#include <zstd.h>
#include <algorithm>
#include <map>
#include <stdexcept>
namespace sentinel::levelstore {
namespace {
void check(bool value,const char* reason) {if(!value)throw std::runtime_error(reason);}
ExactDecimal canonical(ExactDecimal v) {
    while(v.digits && v.mantissa%10==0) {v.mantissa/=10;--v.digits;}
    return v;
}
struct Key {
    bool bid;ExactDecimal price;
    bool operator==(const Key&) const = default;
    bool operator<(const Key& b) const {
        if(bid!=b.bid)return bid;
        auto x=std::to_string(price.mantissa),y=std::to_string(b.price.mantissa);
        const int64_t aexp=int64_t(x.size())-price.digits,bexp=int64_t(y.size())-b.price.digits;
        if(aexp!=bexp)return aexp<bexp;
        const auto n=std::max(x.size(),y.size());x.resize(n,'0');y.resize(n,'0');return x<y;
    }
};
Key key(bool bid,int64_t units,uint32_t digits) {return {bid,canonical({units,digits})};}
struct Event {uint64_t ordinal;int64_t quantity;uint32_t digits;bool operator==(const Event&) const = default;};
struct Run {int64_t initial=0;ExactDecimal current;std::vector<Event> events;};
using Runs=std::map<Key,Run>;
void set(Run& r,uint64_t ordinal,int64_t q,uint32_t digits) {
    if(!r.events.empty()&&r.events.back().ordinal==ordinal)r.events.back()={ordinal,q,digits};
    else r.events.push_back({ordinal,q,digits});
    r.current={q,digits};
}
Runs runs(const D2Hour& hour) {
    Runs result;
    for(const auto& l:hour.start.levels) {
        auto& r=result[key(l.isBid,l.units,hour.start.priceDigits)];
        r.initial=l.size;r.current={l.size,hour.start.sizeDigits};
    }
    uint64_t ordinal=0;
    for(const auto& b:hour.blocks) {
        size_t change=0;
        for(size_t i=0;i<b.messages.size();++i,++ordinal) {
            const auto& m=b.messages[i];
            if(m.reset||m.clear!=ClearCause::None)
                for(auto& [k,r]:result)if(r.current.mantissa)set(r,ordinal,0,m.sizeDigits);
            while(change<b.changes.size()&&b.changes[change].message==i) {
                const auto& c=b.changes[change++];
                set(result[key(c.isBid,c.units,m.priceDigits)],ordinal,c.size,m.sizeDigits);
            }
        }
        check(change==b.changes.size(),"D2 dangling canonical change");
    }
    return result;
}
void section(ByteWriter& w,std::span<const uint8_t> raw) {
    std::vector<uint8_t> compressed(ZSTD_compressBound(raw.size()));
    const auto n=ZSTD_compress(compressed.data(),compressed.size(),raw.data(),raw.size(),3);
    check(!ZSTD_isError(n),"D2 compression");compressed.resize(n);
    w.varint(raw.size());w.varint(n);w.fixed32(sentinel::crc32(compressed.data(),compressed.size()));
    w.bytes.insert(w.bytes.end(),compressed.begin(),compressed.end());
}
std::vector<uint8_t> section(ByteReader& r) {
    const auto raw=r.varint(),stored=r.varint();const auto crc=r.fixed32();
    check(raw<=512*1024*1024&&stored<=r.remaining(),"D2 section size");
    std::vector<uint8_t> compressed(stored),out(raw);
    for(auto& c:compressed)c=r.u8();
    check(sentinel::crc32(compressed.data(),compressed.size())==crc,"D2 page CRC");
    const auto n=ZSTD_decompress(out.data(),out.size(),compressed.data(),compressed.size());
    check(!ZSTD_isError(n)&&n==raw,"D2 decompression");return out;
}
void putKey(ByteWriter& w,const Key& k) {w.u8(k.bid);w.varint(k.price.mantissa);w.fixed32(k.price.digits);}
Key getKey(ByteReader& r) {
    const auto bid=r.u8();const auto p=r.varint();const auto digits=r.fixed32();
    check(bid<=1&&p>0&&p<=INT64_MAX,"D2 price key");
    Key k{bid!=0,{static_cast<int64_t>(p),digits}};
    check(canonical(k.price)==k.price,"D2 noncanonical price");return k;
}
}
std::vector<uint8_t> encodeD2(const D2Hour& hour) {
    const auto levels=runs(hour);
    ByteWriter meta;auto image=hour.start;
    for(auto& l:image.levels)l.size=1;
    encodeImage(meta,image);meta.varint(hour.blocks.size());
    for(auto b:hour.blocks) {for(auto& c:b.changes)c.size=0;encodeBlock(meta,b);}
    ByteWriter w;w.fixed32(0x34443253);w.fixed32(4);
    section(w,meta.bytes);
    std::vector<ByteWriter> pages;std::vector<std::pair<Key,Key>> bounds;
    for(const auto& [k,r]:levels) {
        if(pages.empty()||pages.back().bytes.size()>=32768||bounds.back().second.bid!=k.bid) {
            pages.emplace_back();bounds.push_back({k,k});
        }
        bounds.back().second=k;auto& page=pages.back();putKey(page,k);
        page.varint(r.initial);page.varint(r.events.size());uint64_t previous=0;
        for(const auto& e:r.events) {page.varint(e.ordinal-previous);previous=e.ordinal;page.varint(e.quantity);page.fixed32(e.digits);}
    }
    w.varint(pages.size());
    for(size_t i=0;i<pages.size();++i) {putKey(w,bounds[i].first);putKey(w,bounds[i].second);section(w,pages[i].bytes);}
    w.fixed32(sentinel::crc32(w.bytes.data(),w.bytes.size()));return std::move(w.bytes);
}
D2Hour decodeD2(std::span<const uint8_t> bytes) {
    check(bytes.size()>=12,"D2 truncated header");ByteReader footer(bytes.last(4));
    check(footer.fixed32()==sentinel::crc32(bytes.data(),bytes.size()-4),"D2 container CRC");
    ByteReader r(bytes.first(bytes.size()-4));check(r.fixed32()==0x34443253&&r.fixed32()==4,"D2 version");
    const auto metadata=section(r);ByteReader m(metadata);D2Hour hour;hour.start=decodeImage(m);
    const auto count=m.varint();check(count<=60,"D2 hour block count");
    for(uint64_t i=0;i<count;++i)hour.blocks.push_back(decodeBlock(m));check(m.done(),"D2 metadata suffix");
    Runs levels;std::optional<Key> previous;
    const auto pages=r.varint();check(pages<=r.remaining(),"D2 page count");
    for(uint64_t i=0;i<pages;++i) {
        const auto first=getKey(r),last=getKey(r);check(first.bid==last.bid&&!(last<first),"D2 page bounds");
        const auto raw=section(r);ByteReader p(raw);std::optional<Key> actualFirst,actualLast;
        while(!p.done()) {
            const auto k=getKey(p);check(!previous||*previous<k,"D2 price order");previous=k;
            if(!actualFirst)actualFirst=k;actualLast=k;
            auto& run=levels[k];const auto initial=p.varint();check(initial<=INT64_MAX,"D2 quantity overflow");run.initial=initial;
            const auto events=p.varint();check(events<=p.remaining(),"D2 event count");uint64_t ordinal=0;
            for(uint64_t n=0;n<events;++n) {
                const auto delta=p.varint();check(delta<=UINT64_MAX-ordinal&&(n==0||delta>0),"D2 ordinal");ordinal+=delta;
                const auto q=p.varint();const auto digits=p.fixed32();check(q<=INT64_MAX,"D2 quantity overflow");
                run.events.push_back({ordinal,static_cast<int64_t>(q),digits});
            }
        }
        check(actualFirst&&*actualFirst==first&&*actualLast==last,"D2 page directory mismatch");
    }
    check(r.done(),"D2 trailing bytes");
    for(auto& l:hour.start.levels) {
        const auto it=levels.find(key(l.isBid,l.units,hour.start.priceDigits));
        check(it!=levels.end()&&it->second.initial>0,"D2 missing initial level");l.size=it->second.initial;
    }
    uint64_t ordinal=0;
    for(auto& b:hour.blocks) {
        for(auto& c:b.changes) {
            const auto& msg=b.messages.at(c.message);const auto it=levels.find(key(c.isBid,c.units,msg.priceDigits));
            check(it!=levels.end(),"D2 missing changed level");const auto at=ordinal+c.message;
            const auto& events=it->second.events;
            const auto e=std::lower_bound(events.begin(),events.end(),at,[](const Event& e,uint64_t v){return e.ordinal<v;});
            check(e!=events.end()&&e->ordinal==at&&e->digits==msg.sizeDigits,"D2 missing change");c.size=e->quantity;
        }
        ordinal+=b.messages.size();
    }
    // Validate decoded runs, independent of the zstd version used to publish.
    const auto expected=runs(hour);check(expected.size()==levels.size(),"D2 extra runs");
    for(const auto& [key,run]:expected) {
        const auto it=levels.find(key);check(it!=levels.end()&&it->second.initial==run.initial&&it->second.events==run.events,"D2 noncanonical runs");
    }
    return hour;
}
BookImage endImage(const D2Hour& hour) {
    const auto levels=runs(hour);BookImage image=hour.start;
    image.levels.clear();
    if(!hour.blocks.empty()) {
        const auto& last=hour.blocks.back();image.atMs=last.bucketStartMs+60000;
        image.observed=last.observedAtStart;image.priceDigits=last.priceDigits;image.sizeDigits=last.sizeDigits;
        for(const auto& m:last.messages) {image.observed=m.observed;image.priceDigits=m.priceDigits;image.sizeDigits=m.sizeDigits;}
    }
    for(const auto& [k,r]:levels)if(r.current.mantissa) {
        const auto p=decimalAtScale(k.price,image.priceDigits),q=decimalAtScale(r.current,image.sizeDigits);
        if(!p||!q)throw std::overflow_error("D2 terminal scale overflow");image.levels.push_back({k.bid,*p,*q});
    }
    return image;
}
} // namespace sentinel::levelstore
