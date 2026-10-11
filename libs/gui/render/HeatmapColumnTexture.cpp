#include "HeatmapColumnTexture.hpp"
#include "../PerformanceMonitor.hpp"
#include <QPoint>
#include <rhi/qrhi.h>

HeatmapColumnTexture::~HeatmapColumnTexture() {
    if (m_texture) {
        m_texture->deleteLater();  // released once the GPU is done with it
    }
}

qint64 HeatmapColumnTexture::comparisonKey() const {
    return static_cast<qint64>(reinterpret_cast<quintptr>(this));
}

void HeatmapColumnTexture::setFull(QSize size, int bytesPerCell, QByteArray image, int rowStride) {
    m_size = size;
    m_bytesPerCell = bytesPerCell;
    m_rowStride = rowStride;
    m_full = std::move(image);
    m_fullPending = true;
    m_columns.clear();
}

void HeatmapColumnTexture::enqueueColumn(int x, QByteArray column) {
    if (x < 0 || x >= m_size.width() || column.size() != m_size.height() * m_bytesPerCell) {
        return;
    }
    for (auto& pending : m_columns) {
        if (pending.first == x) {
            pending.second = std::move(column);
            return;
        }
    }
    m_columns.emplace_back(x, std::move(column));
}

void HeatmapColumnTexture::commitTextureOperations(QRhi* rhi, QRhiResourceUpdateBatch* resourceUpdates) {
    if (!rhi || !resourceUpdates || m_size.isEmpty()) {
        return;
    }
    const QRhiTexture::Format format = (m_bytesPerCell == 2) ? QRhiTexture::R16 : QRhiTexture::R8;
    if (!m_texture || m_texture->pixelSize() != m_size || m_texture->format() != format) {
        if (m_texture) {
            m_texture->deleteLater();
            m_texture = nullptr;
        }
        auto* texture = rhi->newTexture(format, m_size, 1, {});
        if (!texture->create()) {
            delete texture;
            return;
        }
        m_texture = texture;
        m_fullPending = !m_full.isEmpty();
    }

    qint64 uploadedBytes = 0;
    if (m_fullPending) {
        QRhiTextureSubresourceUploadDescription full(m_full);
        full.setDataStride(static_cast<quint32>(m_rowStride));
        resourceUpdates->uploadTexture(m_texture, QRhiTextureUploadDescription({0, 0, full}));
        uploadedBytes += m_full.size();
        m_fullPending = false;
        m_columns.clear();  // the full image already contains them
    }
    for (const auto& [x, column] : m_columns) {
        QRhiTextureSubresourceUploadDescription part(column);
        part.setSourceSize(QSize(1, m_size.height()));
        part.setDestinationTopLeft(QPoint(x, 0));
        part.setDataStride(static_cast<quint32>(m_bytesPerCell));
        resourceUpdates->uploadTexture(m_texture, QRhiTextureUploadDescription({0, 0, part}));
        uploadedBytes += column.size();
    }
    m_columns.clear();
    if (uploadedBytes > 0) {
        PerformanceMonitor::instance().addUploadBytes(uploadedBytes);
    }
}

