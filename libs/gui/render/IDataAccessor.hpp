#pragma once
#include <vector>
#include <memory>
#include "GridTypes.hpp"
#include "../../core/marketdata/model/TradeData.h"
#include "../CoordinateSystem.h"

/*
    IDataAccessor — Interface for strategies to access data.
    
    This interface decouples strategies from the specific storage of data
    (currently UnifiedGridRenderer/DataProcessor/DataCache). It allows strategies to "pull" only what they need.
*/
class IDataAccessor {
public:
    virtual ~IDataAccessor() = default;

    // --- Heatmap Data ---
    virtual std::shared_ptr<const std::vector<CellInstance>> getVisibleCells() const = 0;

    // --- Trade Data ---
    // Returns recent trades for bubble/flow rendering.
    virtual const std::vector<Trade>& getRecentTrades() const = 0;

    // --- Context & Config ---
    virtual Viewport getViewport() const = 0;
    virtual double getIntensityScale() const = 0;
    virtual double getMinVolumeFilter() const = 0;
    virtual int getMaxCells() const = 0;
};




