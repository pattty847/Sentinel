#include "dynamiclod.h"

DynamicLOD::DynamicLOD(QObject* parent)
    : QObject(parent)
{
    // TODO: Initialize members if needed
}

DynamicLOD::TimeFrame DynamicLOD::selectOptimalTimeFrame(double msPerPixel, double targetPixelsPerBucket) const {
    // Target: Each bucket should be ~20 pixels wide for good visibility
    double targetBucketDuration_ms = msPerPixel * targetPixelsPerBucket;
    
    // Find the timeframe closest to our target
    TimeFrame bestTF = TimeFrame::TF_1s;
    double bestDiff = std::abs(getTimeFrameDuration(TimeFrame::TF_1s) - targetBucketDuration_ms);
    
    for (int i = 0; i < static_cast<int>(TimeFrame::COUNT); ++i) {
        TimeFrame tf = static_cast<TimeFrame>(i);
        double duration = getTimeFrameDuration(tf);
        double diff = std::abs(duration - targetBucketDuration_ms);
        
        if (diff < bestDiff) {
            bestDiff = diff;
            bestTF = tf;
        }
    }
    
    return bestTF;
}

DynamicLOD::GridInfo DynamicLOD::calculateGrid(int64_t viewStart_ms, int64_t viewEnd_ms, double pixelWidth) const {
    double msPerPixel = static_cast<double>(viewEnd_ms - viewStart_ms) / pixelWidth;
    TimeFrame optimalTF = selectOptimalTimeFrame(msPerPixel);
    
    GridInfo info;
    info.timeFrame = optimalTF;
    info.bucketDuration_ms = getTimeFrameDuration(optimalTF);
    
    // Align to perfect bucket boundaries
    info.firstBucketStart_ms = alignTimestamp(viewStart_ms, optimalTF);
    info.lastBucketStart_ms = alignTimestamp(viewEnd_ms, optimalTF);
    
    // Calculate grid metrics
    int64_t totalDuration = info.lastBucketStart_ms - info.firstBucketStart_ms;
    info.totalBuckets = (totalDuration / info.bucketDuration_ms) + 1;
    info.pixelsPerBucket = pixelWidth / static_cast<double>(info.totalBuckets);
    
    return info;
}