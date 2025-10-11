#pragma once
#include "IMarketDataSink.hpp"
#include "../cache/DataCache.hpp"

class DataCacheSinkAdapter : public IMarketDataSink {
public:
    explicit DataCacheSinkAdapter(DataCache& cache) : m_cache(cache) {}
    void onTrade(const Trade& trade) override { m_cache.addTrade(trade); }

private:
    DataCache& m_cache;
};


