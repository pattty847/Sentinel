# GUI Pipeline Review Notes

Scope: MainWindowGPU, DataProcessor, UnifiedGridRenderer, and the heatmap pipeline.

## Findings

1) Cross-thread calls into DataProcessor
- UnifiedGridRenderer calls DataProcessor methods directly even though DataProcessor lives on a worker QThread.
- Risk: QObject thread-affinity violations and races (e.g., clearData, setPriceResolution, setTimeframe).
- Fix: route calls via QMetaObject::invokeMethod(..., Qt::QueuedConnection).
- Files: libs/gui/UnifiedGridRenderer.cpp

2) DataProcessor mutates GUI-thread GridViewState
- DataProcessor::clearData() calls GridViewState::resetZoom().
- Risk: GridViewState is GUI-thread-only; this can be invoked from the worker thread.
- Fix: move viewport reset into UnifiedGridRenderer::clearData() and keep DataProcessor clear purely internal.
- Files: libs/gui/render/DataProcessor.cpp

3) Heatmap recenter env not applied
- UnifiedGridRenderer::init() calls DataProcessor::setHeatmapRecenterFraction() before DataProcessor is constructed.
- Risk: env-based recenter never takes effect when GPU heatmap is enabled.
- Fix: move the recenter apply after DataProcessor creation or guard with non-null pointer.
- Files: libs/gui/UnifiedGridRenderer.cpp

4) Trade signal currently dropped
- MainWindowGPU connects tradeReceived to UnifiedGridRenderer::onTradeReceived, which is empty.
- Context: trade handling will be used by the candle system soon.
- Action: keep the connection; add a TODO to onTradeReceived for follow-up.
- Files: libs/gui/MainWindowGpu.cpp, libs/gui/UnifiedGridRenderer.cpp

5) DataProcessor::setDataSource unused
- DataProcessor stores m_dataSource but never uses it in current GPU-only path.
- Action: either remove it or document why it remains (future use).
- Files: libs/gui/render/DataProcessor.hpp
