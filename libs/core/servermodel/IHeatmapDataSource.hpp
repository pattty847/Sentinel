#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "SymbolHotData.hpp"

class IHeatmapDataSource {
public:
    virtual ~IHeatmapDataSource() = default;

    virtual int64_t exchangeNowMs() const = 0;
    virtual std::vector<std::string> getSymbolsSnapshot() const = 0;
    virtual SymbolHotData& ensureSymbol(const std::string& symbol) = 0;
};
