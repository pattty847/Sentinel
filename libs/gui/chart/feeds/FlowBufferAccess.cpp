#include "chart/feeds/FlowBufferAccess.hpp"
#include "datasources/RemoteGridDataSource.hpp"
#include <QDateTime>
#include <QPointer>
namespace chart {
void FlowBufferAccess::bind(IGridDataSource& source) {
    clear();
    const QPointer<RemoteGridDataSource> remote = qobject_cast<RemoteGridDataSource*>(&source);
    remote_ = [remote]() -> const FlowSeriesBuffer* { return remote ? &remote->flowBuffer() : nullptr; };
}
void FlowBufferAccess::receive(const FlowSeriesBuffer::Batch& batch) {
    if (!remote_ || !remote_()) fallback_.apply(batch, QDateTime::currentMSecsSinceEpoch());
}
const FlowSeriesBuffer::Series* FlowBufferAccess::series(const QString& symbol, int64_t timeframe) const {
    const auto* remote = remote_ ? remote_() : nullptr;
    const auto& buffer = remote ? *remote : fallback_;
    return buffer.symbol() == symbol ? buffer.series(timeframe) : nullptr;
}
} // namespace chart
