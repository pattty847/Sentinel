#pragma once
#include "chart/core/ChartPorts.hpp"
#include <QPointer>

namespace chart {
class CandleBufferSource final : public ICandleSource {
public:
    bool available() const override { return !buffer_.isNull(); }
    CandleSeriesBuffer* buffer() const { return buffer_.data(); }
    void setBuffer(CandleSeriesBuffer* buffer) { buffer_ = buffer; }
    void visibleBars(const QString& symbol, int64_t tfSec, int64_t start, int64_t end,
                     std::vector<CandleBar>& out) const override {
        if (buffer_) buffer_->getVisibleSlice(symbol, tfSec, start, end, out);
    }
    int64_t oldestTimeMs(const QString& symbol, int64_t tfSec) const override {
        return buffer_ ? buffer_->oldestTimeMs(symbol, tfSec) : 0;
    }
private:
    QPointer<CandleSeriesBuffer> buffer_;
};
} // namespace chart
