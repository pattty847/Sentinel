#include "ChartDebugText.hpp"
#include "render/TimeAxisMapping.hpp"
#include "render/heatmap/HeatmapGpuLayer.hpp"
#include <QDateTime>
#include <QStringList>
#include <algorithm>

QString chart::formatViewportMath(const TimeAxisMapping& m, bool dragging) {
  if (!m.valid) return "Viewport Math\nmapping: N/A";
  QStringList lines;
  lines << "Viewport Math"
        << QString("view.time: %1 → %2 (%3 ms)")
               .arg(static_cast<qint64>(m.viewStartMs))
               .arg(static_cast<qint64>(m.viewEndMs))
               .arg(static_cast<qint64>(std::max(0.0, m.viewEndMs - m.viewStartMs)))
        << QString("view.price: %1 → %2 (Δ%3)")
               .arg(m.viewMinPrice, 0, 'f', 4)
               .arg(m.viewMaxPrice, 0, 'f', 4)
               .arg(std::max(0.0, m.viewMaxPrice - m.viewMinPrice), 0, 'f', 4)
        << QString("tick: %1  grid: %2x%3  append: %4")
               .arg(m.tickSize, 0, 'f', 6)
               .arg(m.gridWidth)
               .arg(m.gridHeight)
               .arg(static_cast<qint64>(m.appendMs))
        << QString("drawRect: x%1 y%2 w%3 h%4")
               .arg(m.drawRect.x(), 0, 'f', 1)
               .arg(m.drawRect.y(), 0, 'f', 1)
               .arg(m.drawRect.width(), 0, 'f', 1)
               .arg(m.drawRect.height(), 0, 'f', 1)
        << QString("srcRect: x%1 y%2 w%3 h%4")
               .arg(m.srcRect.x(), 0, 'f', 2)
               .arg(m.srcRect.y(), 0, 'f', 2)
               .arg(m.srcRect.width(), 0, 'f', 2)
               .arg(m.srcRect.height(), 0, 'f', 2)
        << QString("cell: %1 x %2 px").arg(m.cellW, 0, 'f', 2).arg(m.cellH, 0, 'f', 2)
        << QString("dragging: %1").arg(dragging ? "yes" : "no");
  return lines.join('\n');
}

QString chart::formatDataPipeline(const heatmap::gpu::HeatmapGpuLayer* layer) {
  QStringList lines;
  lines << "Data Pipeline";
  if (!layer) {
    lines << "layer: N/A";
    return lines.join('\n');
  }
  const qint64 received = layer->liveReceivedAtMs();
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  lines << QString("symbol: %1  tf: %2 ms  tick: %3")
               .arg(QString::fromStdString(layer->symbol()))
               .arg(layer->tfMs())
               .arg(layer->tickPrice(), 0, 'f', 6)
        << QString("live open end: %1  age: %2 ms")
               .arg(layer->liveOpenEndMs())
               .arg(received > 0 ? now - received : -1)
        << QString("controller: %1").arg(layer->controller() ? "yes" : "no");
  return lines.join('\n');
}
