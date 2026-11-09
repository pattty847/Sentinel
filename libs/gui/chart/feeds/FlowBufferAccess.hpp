#pragma once
#include "datasources/FlowSeriesBuffer.hpp"
#include <functional>
class IGridDataSource;
namespace chart {
// GUI-thread access shared by footprint and CVD. Remote sources retain history;
// synthetic sources use the same merge policy in a local fallback buffer.
class FlowBufferAccess {
public:
    void bind(IGridDataSource&);
    void clear() { fallback_.clear(); }
    void receive(const FlowSeriesBuffer::Batch&);
    const FlowSeriesBuffer::Series* series(const QString& symbol, int64_t timeframe) const;
private:
    std::function<const FlowSeriesBuffer*()> remote_;
    FlowSeriesBuffer fallback_;
};
} // namespace chart
