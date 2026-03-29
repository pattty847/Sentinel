// Mapping provider contract for render-frame time/price transforms.
#pragma once

#include "TimeAxisMapping.hpp"
#include <QPointF>
#include <QRectF>
#include <QtPlugin>
#include <cstdint>

struct MappingFrameContext {
    QRectF surfaceBounds;
    double surfaceDpr = 1.0;
    qint64 presentationTimeMs = 0;
    qint64 activeTimeframeMs = 0;
    qint64 nowEventTimeMs = 0;
    qint64 currentBoundaryStartMs = 0;
    qint64 nextBoundaryStartMs = 0;
    qint64 boundarySequence = 0;
    bool hasEventTime = false;

    bool viewportValid = false;
    qint64 viewportTimeStart = 0;
    qint64 viewportTimeEnd = 0;
    double viewportMinPrice = 0.0;
    double viewportMaxPrice = 0.0;
    QPointF viewportPanVisualOffset;
    bool viewportDragging = false;
    bool viewportAutoScrollEnabled = false;

    uint64_t heatmapGeneration = 0;
    uint64_t footprintGeneration = 0;
    uint64_t candleGeneration = 0;

    TimeAxisMapping mapping;
};

class ITimeAxisMappingProvider {
public:
    virtual ~ITimeAxisMappingProvider() = default;
    virtual MappingFrameContext currentFrameContext() const = 0;
    virtual TimeAxisMapping currentTimeAxisMapping() const = 0;
};

#define SENTINEL_ITIMEAXISMAPPINGPROVIDER_IID "io.sentinel.ITimeAxisMappingProvider/1.0"
Q_DECLARE_INTERFACE(ITimeAxisMappingProvider, SENTINEL_ITIMEAXISMAPPINGPROVIDER_IID)
