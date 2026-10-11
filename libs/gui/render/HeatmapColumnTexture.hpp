#pragma once

#include <QByteArray>
#include <QSGTexture>
#include <QSize>
#include <utility>
#include <vector>

class QRhiTexture;

// Intensity texture for RHI backends (Metal, D3D, Vulkan). Keeps one GPU
// texture and uploads only queued columns; the whole image is uploaded only
// after setFull (resize or rebuild). Render thread only.
class HeatmapColumnTexture final : public QSGTexture {
public:
    ~HeatmapColumnTexture() override;

    // image: height rows of width * bytesPerCell bytes, rowStride bytes apart.
    void setFull(QSize size, int bytesPerCell, QByteArray image, int rowStride);
    void enqueueColumn(int x, QByteArray column);

    qint64 comparisonKey() const override;
    QRhiTexture* rhiTexture() const override { return m_texture; }
    QSize textureSize() const override { return m_size; }
    bool hasAlphaChannel() const override { return false; }
    bool hasMipmaps() const override { return false; }
    void commitTextureOperations(QRhi* rhi, QRhiResourceUpdateBatch* resourceUpdates) override;

private:
    QSize m_size;
    int m_bytesPerCell = 2;
    int m_rowStride = 0;
    QByteArray m_full;
    bool m_fullPending = false;
    std::vector<std::pair<int, QByteArray>> m_columns;
    QRhiTexture* m_texture = nullptr;
};

