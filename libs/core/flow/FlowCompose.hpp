#pragma once
#include "FlowBar.hpp"
#include <span>

namespace sentinel::flow {
FlowBar composeFlowBars(std::span<const FlowBar> bars, int64_t timeframeMs);
} // namespace sentinel::flow
