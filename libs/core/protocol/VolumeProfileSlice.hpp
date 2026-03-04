#pragma once

#include <QByteArray>
#include <QMetaType>
#include <QString>

/*
 * Sentinel – VolumeProfileSlice
 *
 * Wire payload for Mode A (Session Volume Profile).  Sent once per heatmap
 * bucket update with the accumulated per-level volumes for the current session.
 *
 * volumeBinsF32: float32 little-endian, one value per price bin, size == gridHeight.
 *   bin[0]             → price = maxPrice - 0.5*tickSize  (top of grid)
 *   bin[gridHeight-1]  → price = minPrice + 0.5*tickSize  (bottom of grid)
 *   Same row ordering as HeatmapSlice / FootprintSlice.
 *
 * POC / VAH / VAL use Steidlmayer 70 % value-area methodology:
 *   - POC  = price level with the highest volume
 *   - VA   = smallest contiguous set of levels (expanding outward from POC)
 *             that contains ≥ 70 % of totalVolume
 *   - VAH  = highest price in the VA
 *   - VAL  = lowest  price in the VA
 */
struct VolumeProfileSlice {
    QString  symbol;
    int64_t  sessionStartMs  = 0;
    int64_t  sessionEndMs    = 0;
    int      sessionType     = 4;    // maps to SessionManager::SessionType (4 = H24)
    double   minPrice        = 0.0;
    double   maxPrice        = 0.0;
    double   tickSize        = 0.0;
    int      gridHeight      = 0;
    double   totalVolume     = 0.0;
    double   pocPrice        = 0.0;  // price at POC bin centre
    double   vahPrice        = 0.0;  // value area high
    double   valPrice        = 0.0;  // value area low
    QString  format          = QStringLiteral("vp_f32");
    QByteArray volumeBinsF32;        // float32 LE, length = gridHeight * 4
};

Q_DECLARE_METATYPE(VolumeProfileSlice)
