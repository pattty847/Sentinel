// S4 spike, kept as a regression guard: a QSGRenderNode that lives in an
// ordinary Qt Quick scene graph records a COMPUTE pass in prepare() (outside
// the scene graph's main render pass) and draws the compute output in render().
// If a Qt upgrade breaks this, HeatmapTileNode must fall back to
// QQuickWindow::beforeRendering (see docs/research/2026-09-gpu-heatmap-integration-plan.md, S4).
#include "lab/OffscreenQuick.hpp"
#include "lab/RhiBackend.hpp"
#include <QFile>
#include <QGuiApplication>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSGRenderNode>
#include <gtest/gtest.h>
#include <rhi/qrhi.h>
#include <rhi/qshader.h>
#include <array>
#include <atomic>
#include <cstring>
#include <iostream>
#include <memory>

namespace {
QShader loadShader(const char *path) {
    QFile file(QString::fromLatin1(path));
    return file.open(QIODevice::ReadOnly) ? QShader::fromSerialized(file.readAll()) : QShader{};
}

struct SpikeStats {
    std::atomic<int> prepares{0}, dispatches{0}, renders{0}, failures{0};
};

class SpikeNode final : public QSGRenderNode {
public:
    SpikeNode(SpikeStats *stats, QRectF rect, uint32_t columns, uint32_t blue)
        : stats_(stats), rect_(rect), columns_(columns), blue_(blue) {}
    void prepare() override {
        stats_->prepares.fetch_add(1);
        QRhiCommandBuffer *cb = commandBuffer();
        QRhiRenderTarget *rt = renderTarget();
        if (!cb || !rt) { stats_->failures.fetch_add(1); return; }
        QRhi *rhi = rt->rhi();
        if (!ready_ && !create(rhi, rt)) { stats_->failures.fetch_add(1); return; }
        const std::array<uint32_t, 4> dims{columns_, blue_, 0, 0};
        auto *computeUpdates = rhi->nextResourceUpdateBatch();
        computeUpdates->updateDynamicBuffer(computeParams_.get(), 0, sizeof(dims), dims.data());
        // The question the spike answers: is a compute pass legal here?
        cb->beginComputePass(computeUpdates);
        cb->setComputePipeline(compute_.get());
        cb->setShaderResources(computeBindings_.get());
        cb->dispatch(int((columns_ + 63) / 64), 1, 1);
        cb->endComputePass();
        stats_->dispatches.fetch_add(1);
        struct { float mvp[16]; float rect[4]; uint32_t dims[4]; } draw{};
        const QMatrix4x4 mvp = *projectionMatrix() * *matrix();
        std::memcpy(draw.mvp, mvp.constData(), sizeof(draw.mvp));
        draw.rect[0] = float(rect_.x()); draw.rect[1] = float(rect_.y());
        draw.rect[2] = float(rect_.width()); draw.rect[3] = float(rect_.height());
        draw.dims[0] = columns_;
        auto *drawUpdates = rhi->nextResourceUpdateBatch();
        drawUpdates->updateDynamicBuffer(drawParams_.get(), 0, sizeof(draw), &draw);
        cb->resourceUpdate(drawUpdates);
    }
    void render(const RenderState *state) override {
        if (!ready_) return;
        QRhiCommandBuffer *cb = commandBuffer();
        cb->setGraphicsPipeline(graphics_.get());
        const QSize size = renderTarget()->pixelSize();
        cb->setViewport(QRhiViewport(0, 0, float(size.width()), float(size.height())));
        // UsesScissor: a scissor must always be set. Metal tolerated none; D3D
        // clips everything (as HeatmapTileNode::render does, fall back to full).
        if (state->scissorEnabled()) {
            const QRect r = state->scissorRect();
            cb->setScissor(QRhiScissor(r.x(), r.y(), r.width(), r.height()));
        } else {
            cb->setScissor(QRhiScissor(0, 0, size.width(), size.height()));
        }
        cb->setShaderResources(drawBindings_.get());
        cb->draw(4);
        stats_->renders.fetch_add(1);
    }
    void releaseResources() override {
        graphics_.reset(); drawBindings_.reset(); compute_.reset(); computeBindings_.reset();
        output_.reset(); computeParams_.reset(); drawParams_.reset();
        ready_ = false;
    }
    ~SpikeNode() override { releaseResources(); }
    StateFlags changedStates() const override { return ViewportState | ScissorState; }
    RenderingFlags flags() const override { return BoundedRectRendering | NoExternalRendering; }
    QRectF rect() const override { return rect_; }

private:
    bool create(QRhi *rhi, QRhiRenderTarget *rt) {
        if (!rhi->isFeatureSupported(QRhi::Compute)) return false;
        output_.reset(rhi->newBuffer(QRhiBuffer::Static, QRhiBuffer::StorageBuffer, columns_ * 16));
        computeParams_.reset(rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 16));
        drawParams_.reset(rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 96));
        if (!output_->create() || !computeParams_->create() || !drawParams_->create()) return false;
        const auto cs = QRhiShaderResourceBinding::ComputeStage;
        computeBindings_.reset(rhi->newShaderResourceBindings());
        computeBindings_->setBindings({QRhiShaderResourceBinding::bufferStore(0, cs, output_.get()),
                                       QRhiShaderResourceBinding::uniformBuffer(1, cs, computeParams_.get())});
        if (!computeBindings_->create()) return false;
        compute_.reset(rhi->newComputePipeline());
        compute_->setShaderStage({QRhiShaderStage::Compute, loadShader(":/spikeshaders/spike_fill.comp.qsb")});
        compute_->setShaderResourceBindings(computeBindings_.get());
        if (!compute_->create()) return false;
        const auto fs = QRhiShaderResourceBinding::FragmentStage;
        const auto vs = QRhiShaderResourceBinding::VertexStage;
        drawBindings_.reset(rhi->newShaderResourceBindings());
        drawBindings_->setBindings({QRhiShaderResourceBinding::bufferLoad(0, fs, output_.get()),
                                    QRhiShaderResourceBinding::uniformBuffer(1, vs | fs, drawParams_.get())});
        if (!drawBindings_->create()) return false;
        graphics_.reset(rhi->newGraphicsPipeline());
        graphics_->setShaderStages({{QRhiShaderStage::Vertex, loadShader(":/spikeshaders/spike_draw.vert.qsb")},
                                    {QRhiShaderStage::Fragment, loadShader(":/spikeshaders/spike_draw.frag.qsb")}});
        graphics_->setTopology(QRhiGraphicsPipeline::TriangleStrip);
        graphics_->setShaderResourceBindings(drawBindings_.get());
        graphics_->setRenderPassDescriptor(rt->renderPassDescriptor());
        graphics_->setSampleCount(rt->sampleCount());
        graphics_->setFlags(QRhiGraphicsPipeline::UsesScissor);
        if (!graphics_->create()) return false;
        ready_ = true;
        return true;
    }
    SpikeStats *stats_;
    QRectF rect_;
    uint32_t columns_, blue_;
    bool ready_ = false;
    std::unique_ptr<QRhiBuffer> output_, computeParams_, drawParams_;
    std::unique_ptr<QRhiShaderResourceBindings> computeBindings_, drawBindings_;
    std::unique_ptr<QRhiComputePipeline> compute_;
    std::unique_ptr<QRhiGraphicsPipeline> graphics_;
};

class SpikeItem final : public QQuickItem {
public:
    SpikeStats stats;
    SpikeItem() { setFlag(ItemHasContents, true); }
protected:
    QSGNode *updatePaintNode(QSGNode *old, UpdatePaintNodeData *) override {
        delete old; // the spike rebuilds nothing after the first frame
        return new SpikeNode(&stats, QRectF(0, 0, width(), height()), 8, 200);
    }
};

TEST(QsgComputeSpike, ComputeInRenderNodePrepareFeedsRenderInSameFrame) {
    if (const QString why = lab::gpuUnavailableReason(); !why.isEmpty())
        GTEST_SKIP() << "GPU case skipped: " << why.toStdString();
    lab::OffscreenQuick scene;
    QString error;
    ASSERT_TRUE(scene.create(QSize(64, 32), &error)) << error.toStdString();
    // A sibling rectangle below and a QSG tree around the node prove it runs
    // inside the normal batch renderer, not an isolated pass.
    auto *item = new SpikeItem;
    item->setParentItem(scene.window()->contentItem());
    item->setPosition(QPointF(16, 8));
    item->setSize(QSizeF(32, 16));
    scene.window()->setColor(Qt::black);
    const QImage frame = scene.renderFrame(&error);
    ASSERT_FALSE(frame.isNull()) << error.toStdString();
    EXPECT_EQ(item->stats.failures.load(), 0);
    EXPECT_GE(item->stats.dispatches.load(), 1);
    EXPECT_GE(item->stats.renders.load(), 1);
    // 8 compute columns over a 32 px item: 4 px per column. Column i is red if
    // i%4==0, green if i%4==1, and always carries blue = 200.
    for (int column = 0; column < 8; ++column) {
        const QColor pixel = frame.pixelColor(16 + column * 4 + 2, 16);
        EXPECT_EQ(pixel.red(), column % 4 == 0 ? 255 : 0) << "column " << column;
        EXPECT_EQ(pixel.green(), column % 4 == 1 ? 255 : 0) << "column " << column;
        EXPECT_EQ(pixel.blue(), 200) << "column " << column;
    }
    // Outside the item the window clear colour remains.
    EXPECT_EQ(frame.pixelColor(4, 4), QColor(Qt::black));
    // A second frame dispatches again (per-frame compute is legal, not a one-off).
    const QImage second = scene.renderFrame(&error);
    ASSERT_FALSE(second.isNull()) << error.toStdString();
    EXPECT_GE(item->stats.dispatches.load(), 2);
}
} // namespace

int main(int argc, char **argv) {
    // Offscreen unless set: Vulkan needs a real platform plugin (QT_QPA_PLATFORM=windows|xcb).
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    lab::selectQuickSceneGraph(); // before any QQuickWindow: Qt fixes the backend at the first one
    ::testing::InitGoogleTest(&argc, argv);
    std::cout << "[sentinel] " << lab::describeRhi().toStdString() << std::endl;
    return RUN_ALL_TESTS();
}
