#pragma once

#include "render/FrameContext.hpp"
#include <atomic>

namespace chart {

// GUI writes controls; sync stamps/submits; afterRendering publishes completion.
class FrameAck {
    std::atomic<uint64_t> m_controlRevision{0};
    std::atomic<uint64_t> m_controlSelectionEpoch{0};
    std::atomic<uint64_t> m_controlViewportVersion{0};
    std::atomic<uint64_t> m_renderedRevision{0};
    std::atomic<uint64_t> m_renderedFrameId{0};
    uint64_t m_nextFrameId = 0; // render thread only
    uint64_t m_pendingFrameRevision = 0; // render thread only
    uint64_t m_pendingFrameId = 0; // render thread only

public:
    void setAgentControlRevision(uint64_t revision, uint64_t selectionEpoch, uint64_t viewportVersion) {
        m_controlSelectionEpoch.store(selectionEpoch, std::memory_order_release);
        m_controlViewportVersion.store(viewportVersion, std::memory_order_release);
        m_controlRevision.store(revision, std::memory_order_release);
    }
    uint64_t renderedControlRevision() const { return m_renderedRevision.load(std::memory_order_acquire); }
    uint64_t renderedFrameId() const { return m_renderedFrameId.load(std::memory_order_acquire); }
    void stamp(FrameContext& frame) {
        frame.controlRevision = m_controlRevision.load(std::memory_order_acquire);
        frame.selectionEpoch = m_controlSelectionEpoch.load(std::memory_order_acquire);
        frame.viewportVersion = m_controlViewportVersion.load(std::memory_order_acquire);
        frame.frameId = ++m_nextFrameId;
    }
    void submitted(const FrameContext& frame) {
        m_pendingFrameRevision = frame.controlRevision;
        m_pendingFrameId = frame.frameId;
    }
    void rendered() {
        m_renderedFrameId.store(m_pendingFrameId, std::memory_order_release);
        m_renderedRevision.store(m_pendingFrameRevision, std::memory_order_release);
    }
};

} // namespace chart
