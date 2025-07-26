# Sentinel Microstructure & AI Roadmap (2025-2026)

*Owner *: Patrick (Lead Dev)  *Co-author *: o3 Planning Agent

> This single document is the canonical backlog for **next-gen market-microstructure features**.  Each phase is intentionally self-contained so we can ship, test and stabilise one major capability at a time.  Subsequent phases assume the previous ones are merged.

---

## Legend
| Emoji | Meaning |
|-------|---------|
| 🚧 | Implementation task (code) |
| 🧪 | Unit / integration test |
| 📈 | Performance / memory KPI |
| 🎨 | GUI / UX task |
| 📝 | Doc work |
| ✅ | Acceptance criterion |

---

## Phase 0 (✅ complete) – Stabilise Grid Core
* Fix `LiveOrderBook` implementation & tests <done>
* Enforce bid < ask after quantisation
* All tests passing (<1 s runtime)

Outcome → green baseline for new analytics.

---

## Phase 1 — Spoof-Pull Detection & Indicator
Goal: flag large resting liquidity that disappears when price approaches.

### 1.1 Data Features
- Modify `LiquidityTimeSeriesEngine` to track **enter / exit timestamps** per price bucket. (Already stores `firstSeen_ms` & `lastSeen_ms`; extend with `sizeAtFirstSeen`.)
- Add rolling window (e.g. 3 × slice) to compute `pulledLiquidity = sizeAtFirstSeen − sizeAtLastSeen`.

### 1.2 Algorithm (pseudocode)
```cpp
for each slice S (100 ms):
  for each price P in bids & asks:
    if price moves towards P and
       pulledLiquidity(P) > SPOOF_THRESHOLD &&
       persistenceRatio(P) < 0.3:   // didn’t stay long
        emit SpoofEvent{side, price, pulledLiquidity, timestamp}
```

Thresholds exposed in `config.yaml` for tuning.

### 1.3 Backend Tasks
| ID | Task |
|----|------|
| 🚧 SP1 | Add `sizeAtFirstSeen` to `PriceLevelMetrics` |
| 🚧 SP2 | Implement `detectSpoofEvents()` inside `finalizeLiquiditySlice()` |
| 🚧 SP3 | Create `struct SpoofEvent` + Qt signal in `LiquidityTimeSeriesEngine` |
| 🧪 SP4 | New GTest: synthetic snapshots create spoof event |
| 📈 SP5 | Ensure extra metrics add <5 % CPU per slice |

### 1.4 GUI Tasks
| ID | Task |
|----|------|
| 🎨 SP6 | In `UnifiedGridRenderer`, draw ⚠️ icon at price/time of spoof event |
| 🎨 SP7 | Tooltip shows `pulledLiquidity` & probability |

### 1.5 Acceptance
- ✅ Unit test passes.
- ✅ Icon appears on live feed when known spoof bot triggers.
- ✅ CPU < 15 ms @10 k msg/s on M1 Pro.

---

## Phase 2 — Iceberg Order Detection
Detect repetitive small prints at same price whose cumulative size >> reported book size.

### Key Steps
1. Extend `DataCache` to maintain rolling sum of trade sizes per price (`TradeRing` already available).
2. Compare cumulative trade volume vs. visible liquidity to infer hidden size.
3. Emit `IcebergEvent` (price, revealedSize, aggressorSide, confidence).
4. GUI: small 🧊 icon with size overlay.

*Dependencies*: Phase 1 signal plumbing.

---

## Phase 3 — Absorption Zones
Identify price buckets where large resting liquidity **absorbs** aggressive orders without moving.

Algorithm sketch: high market-order volume at P but mid-price change < δ.  Persistence ratio > 0.8.

Visual: shaded rectangle (semi-transparent purple) spanning absorption duration.

---

## Phase 4 — Momentum-Ignition Chain
Pattern: liquidity pull + spread widening + burst of buys/sells.

Will subscribe to events from Phases 1-3, then run a finite-state machine to trigger an `IgnitionAlert`.

---

## Phase 5 — Dynamic Grid Controls & Volume Filters
- Runtime slider for price resolution (¢ 1 – $ 10).
- Add / remove arbitrary timeframes (context menu → `LiquidityTimeSeriesEngine::addTimeframe`).
- Heat-map colour scale with min/max volume filter & log scaling.

GUI impact: update `ZoomControls.qml` → becomes `GridControls.qml`.

---

## Phase 6 — Visualization Pack
- Trade-bubble layer (instanced circles, radius ∝ size, colour = aggressor side).
- Candles built from 100 ms grid (aggregated OHLC).
- TPO / footprint overlay (volume per price bucket, text rendered via SDF font).

Performance target: ≤4 ms additional GPU time @ 5 k visible cells.

---

## Phase 7 — CopeNet v1 (LLM-Insight Service)
Microservice (FastAPI + OpenAI/GPT-4o) that ingests:
- Latest 60 s of `Spoof/Iceberg/Absorption/Ignition` events.
- Grid statistics snapshot (JSON, 5 KB).

Responds with TL;DR + risk score JSON.  GUI side panel shows chat-style insights.

Security: sign requests with HMAC; offline fallback uses local GPT-J.

---

## Phase 8 — Smart Alert Engine
If CopeNet risk > 0.8 or ignition alert fires, push desktop & webhook notifications.

Integration path: Qt signal → `AlertManager` → `WebhookService`.

---

## Phase 9 — Terminal-Grade UX Overhaul
- Left toolbar (icons) & top timeframe bar (btn-group).
- Detachable panes (Qt Dock Widgets).
- Dark Bloomberg theme (QSS).

---

## Implementation Workflow
1. **Create feature branch**: `feature/phase-X-<slug>`
2. Author code + tests; run `./build/tests/*` (all green) + `asan/tsan`.
3. Update docs (`docs/features/...`).
4. PR → squash-merge after review.
5. Tag nightly build; perf-regression CI must pass.

---

### Estimated Timeline
| Phase | Duration | Earliest Start |
|-------|----------|----------------|
| 1 | 1 week | now |
| 2 | 1 week | after 1 |
| 3 | 1 week | after 2 |
| 4 | 1.5 wk | after 3 |
| 5 | 1 wk | parallel 3-4 (GUI heavy) |
| 6 | 2 wk | after 5 |
| 7 | 2 wk | parallel 6 |
| 8 | 0.5 wk | after 7 |
| 9 | 1 wk | rolling |

Total ≈ 10-11 calendar weeks with one dev.

---

## How to Use This Document
1. Pick **one** phase, mark tasks `in-progress` in TODO file.
2. Implement code guided by task list (Claude Code or o3).
3. Keep this roadmap updated when scope changes.
4. Celebrate incremental wins. 🎉 