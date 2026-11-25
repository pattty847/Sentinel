#pragma once
// Sealed-run planner: coarsest dividing level wins; holes fall back to finer
// levels, never overlap a selected coarse chunk. Pure, bounded recording range.
#include "protocol/ChunkClientWire.hpp"
#include <functional>
namespace sentinel::flow {
struct FlowChunkWant {
    std::string instrument, channel;
    int64_t timeframeMs = 0, fromMs = 0, toMs = 0;
    int pan = 0; // negative older, positive newer
    bool padding = true; // anchor prefixes need only their exact interval
};
struct PlannedFlowChunk {
    protocol::chunks::Key key;
    int tier = 0; // visible, pan side, opposite side
    uint64_t order = 0;
};
bool validWant(const FlowChunkWant&);
size_t flowChunkEstimate(const protocol::chunks::Key&);
std::vector<PlannedFlowChunk> planFlowChunks(const FlowChunkWant&, const protocol::chunks::Availability&,
    const std::function<size_t(const protocol::chunks::Key&)>& estimate, size_t paddingBudget = 32ull << 20);
}
