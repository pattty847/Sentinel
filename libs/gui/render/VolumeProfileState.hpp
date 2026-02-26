/*
 * Sentinel – VolumeProfileState
 *
 * CPU-side accumulator for Mode A (Session Volume Profile).
 *
 * Threading: ingest on data thread; snapshot / renderer read must take the
 *            internal lock (same pattern as FootprintStreamState).
 *
 * Value-Area math (Steidlmayer / CME standard):
 *   1. Find POC = bin with highest volume.
 *   2. Starting from POC, expand the VA one bin at a time – always adding
 *      the single bin (above or below the current frontier) with higher
 *      volume – until cumulative volume >= 70 % of total session volume.
 *   3. VAH = price at top of highest included bin.
 *      VAL = price at bottom of lowest included bin.
 */
#pragma once

#include <QByteArray>
#include <cstdint>
#include <mutex>
#include <vector>

struct VolumeProfileSlice;

class VolumeProfileState {
public:
    struct ValueArea {
        int    pocRow = -1;    // grid row of POC (top-of-grid = row 0)
        int    vahRow = -1;    // grid row of VAH (lowest row index = highest price)
        int    valRow = -1;    // grid row of VAL (highest row index = lowest price)
        double pocPrice = 0.0;
        double vahPrice = 0.0;
        double valPrice = 0.0;
        double totalVolume = 0.0;
        bool   valid = false;
    };

    struct Snapshot {
        int64_t sessionStartMs  = 0;
        int64_t sessionEndMs    = 0;
        int     sessionType     = 4;
        double  minPrice        = 0.0;
        double  maxPrice        = 0.0;
        double  tickSize        = 0.0;
        int     gridHeight      = 0;
        ValueArea va;
        bool    dirty           = false;  // true if new data since last render
        uint64_t generation     = 0;
    };

    VolumeProfileState() = default;

    // Ingest a VP slice from the data thread.  Returns true if the state changed.
    bool ingestSlice(const VolumeProfileSlice& slice);

    // Atomically swap pending bins into caller's buffer for GPU upload.
    // Returns true and populates out if new data is available.
    bool takePendingBins(std::vector<float>& out, Snapshot& outSnap);

    Snapshot snapshot() const;

    void clear();

    // ── Static math (exposed for tests) ─────────────────────────────────────
    // Steidlmayer 70 % value-area expansion.
    // bins[0] = top-of-grid (highest price), bins[N-1] = lowest price.
    static ValueArea computeValueArea(const std::vector<float>& bins,
                                      double minPrice,
                                      double tickSize,
                                      double vaFraction = 0.70);

private:
    void resetLocked(const VolumeProfileSlice& slice);

    mutable std::mutex m_mutex;

    // Current session parameters
    int64_t m_sessionStartMs = 0;
    int64_t m_sessionEndMs   = 0;
    int     m_sessionType    = 4;
    double  m_minPrice       = 0.0;
    double  m_maxPrice       = 0.0;
    double  m_tickSize       = 0.0;
    int     m_gridHeight     = 0;

    // Fixed-size volume accumulator (float per price bin, top→bottom)
    // Allocated once per session; reused across bucket updates.
    std::vector<float> m_bins;

    bool    m_dirty      = false;
    uint64_t m_generation = 0;
    ValueArea m_va;
};
