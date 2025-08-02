#include "IRenderStrategy.hpp"
#include <QSGGeometryNode>
#include <QSGGeometry>
#include <algorithm>

void IRenderStrategy::ensureGeometryCapacity(QSGGeometryNode* node, int vertexCount) {
    if (!node || !node->geometry()) return;
    
    auto* geometry = node->geometry();
    if (geometry->vertexCount() < vertexCount) {
        geometry->allocate(vertexCount);
        node->markDirty(QSGNode::DirtyGeometry);
    }
}

double IRenderStrategy::calculateIntensity(double liquidity, double intensityScale) const {
    if (liquidity <= 0.0) return 0.0;
    
    // Logarithmic scaling for better visual distribution
    double logLiquidity = std::log(1.0 + liquidity);
    double intensity = logLiquidity * intensityScale * 0.1;
    
    return std::min(1.0, intensity);
}