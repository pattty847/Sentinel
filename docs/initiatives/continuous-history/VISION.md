Sentinel Continuous History — Vision & Execution Brief

September 4, 2026. FORGE initiative. Slices 0-2 and the first bounded heatmap-paging seam are implemented; later release slices remain scoped below.

**Direction**

Make Sentinel a persistent market-data workspace: leave collection running, open a client later, inspect recorded market behavior across time and price, and return to live without losing context. The first user is the owner using a personal server and desktop clients. Shared commercial hosting, account management, and exchange-data redistribution are separate product decisions.

The first release is one complete workflow: BTC-USD, one-minute heatmap history, restart recovery, automatic loading while panning, correct rendering across recorded price bands, explicit missing-data states, and return to live. Footprint and TPO follow as independently usable releases over the same history infrastructure. This preserves the previously recorded owner decision to start persistence with a one-minute anchor.

Success means the recorded history remains useful when the client disconnects, the server restarts, the price moves beyond the initial band, or the viewport leaves the current GPU window. A successful disk read or a completed backend feature is not sufficient.

**What the investigation establishes**

The [re-entry review](/Users/copeharder/Programming/Sentinel/docs/REENTRY_REVIEW.md) contains the source evidence and initial validation. Existing test binaries passed 70 cases. Fresh isolated probes reproduced older-history rejection and two session-boundary bugs. Server lifetime, queue, blocking-I/O, and durability concerns are grounded in source; their production impact has not been measured. A fresh project build remains unverified after the previous targeted build triggered and partially completed dependency reconciliation.

Existing plans already supply much of the intended architecture:

- [F1 persistence decisions](/Users/copeharder/Programming/Sentinel/docs/private/plans/F1_HEATMAP_PERSISTENCE.md:33): one-minute initial scope; timestamp-addressed records; per-column price metadata; preserve gaps; exclusive writer ownership. These are recorded owner decisions, not newly proposed defaults.
- [Infinite Canvas V2](/Users/copeharder/Programming/Sentinel/docs/private/plans/INFINITE_CANVAS_V2.md:38): CPU column retention and a GPU viewport cache. Retain that separation. Bring automatic history loading into the first release, make all memory limits explicit, and replace bandwidth-based FPS assurances with measurement.
- [Footprint V1](/Users/copeharder/Programming/Sentinel/docs/private/plans/tpo/plans/TPO_V1.md:98): canonical bid/ask values were envisioned, but transport intentionally exposed only delta. The next footprint release expands that contract; it is not merely enabling a hidden toolbar control.
- [Coordinate contracts](/Users/copeharder/Programming/Sentinel/docs/COORDINATE_SYSTEMS.md): time/price mapping remains central, while session-anchored TPO retains its own horizontal domain.

These older plans are design history. Current invariants and implementation govern where they disagree, including current layer-toggle behavior. This initiative coordinates F1, F4, and F8 without replacing their feature records.

**Experience and important states**

| Situation | What the user experiences |
| --- | --- |
| Opening a client | The newest available view loads; recording health and last received time are visible |
| Panning into older data | Nearby history loads automatically; already loaded content remains usable |
| History request still running | A restrained loading indication marks the requested interval; the chart remains interactive |
| Server was offline or price was outside recorded coverage | A visible gap or unavailable region, distinct from recorded zero liquidity |
| Reaching the oldest available history | The UI communicates the history boundary and stops repeated requests |
| Server connection drops | Loaded history remains browsable; live state becomes disconnected/stale |
| Reading history while trades continue | The viewport stays where the user put it; collection and live reception continue |
| Returning to live | A single action restores live following without leaving historical coordinates or session state behind |
| Switching symbol/timeframe during a load | Old responses cannot replace or reconfigure the newly selected view |

No per-cell QML objects are introduced. Existing toolbar and chart controls remain the interaction surface. Footprint/TPO controls become available only when their end-to-end acceptance criteria pass.

**Architecture and ownership**

Keep the current exchange -> ServerDataModel -> stream -> DataProcessor -> GPU path. Extend it at the missing seams:

1. **Server collection and storage.** `HeatmapTwapStreamer` remains authoritative for finalized heatmap columns. `HeatmapColumnStore` remains the disk implementation; its RAM ring is a recent-history cache. Storage work must have an explicit queue limit and durability state rather than blocking aggregation while holding its history mutex. When persistence is requested and exclusive ownership cannot be acquired, enforce the existing documented refusal-to-start contract.
2. **History serving.** Move disk queries and REST bootstrap off `SentinelStreamServer`'s network loop to bounded workers. Keep ordered session state on its executor. Bound work before it is posted, as well as the socket write queue. Use bounded response pages, request identity, a continuation/floor indication, and cancellation or stale-result suppression. Full book deltas must not be silently discarded to make a queue fit; disconnect/resynchronize a slow client when necessary.
3. **Client history.** Give `DataProcessor` or its existing datasource worker one bounded, timestamp-addressed heatmap cache. Extend the intended ColumnStore concept; do not revive the disabled accumulation map as a second source of truth. It accepts historical insertion, duplicate results, live revisions, and finalization. Keys include symbol, timeframe, bucket timestamp, and applicable data generation. Columns retain their original price metadata.
4. **Viewport preparation.** A worker prepares immutable upload batches for the requested time/price interval and a bounded margin. The render thread consumes snapshots and performs GPU updates. It must not query disk, wait for the network, or hold a mutable history-cache lock. Account for referenced snapshots and pending batches in the memory budget so eviction actually bounds retained memory.
5. **GPU and coordinates.** `UnifiedGridRenderer`, `HeatmapOverlayRenderer`, and `TimeAxisMapping` retain their roles. GPU residency is independent of total retained history. Reuse buffers and textures; update only affected regions. Use a focused prototype to choose between worker-side row remapping and per-column GPU price mapping. Verify numeric/coverage behavior and frame costs before choosing; the old plan does not prove the current shader can remain unchanged.
6. **Trade-derived charts.** Introduce reusable pure C++ aggregation over timestamp, price, quantity, aggressor side, and trade identity. The existing [binary trade reader](/Users/copeharder/Programming/Sentinel/libs/core/trading/MarketEventSource.cpp:69) is a starting point, but its trading DTO discards side and ID. Extract a common decoder within core and keep the trading adapter consuming its needed subset. Do not make server history depend on simulated-trading behavior or add another independent binary parser.

New domain aggregation, storage, and shared decoding belong in pure C++ core components. Qt/QSG behavior, viewport preparation, and GUI caches belong in `libs/gui`. Existing QObject orchestration can adapt between them. Application bootstraps remain thin. Preserve family schema validation, queued Qt thread crossings, immutable render snapshots, and `setViewport()` versioning.

The one-minute milestone loads authoritative columns without manufacturing coarser timeframes. It can pan through arbitrary retained history; overview zoom beyond the resident source-cell budget requires the later rollup slice. Do not describe that limitation as unlimited zoom. Missing time buckets remain missing; source price bands do not imply coverage outside their recorded ranges.

**Delivery slices and gates**

| Slice | Observable result | Primary work and verification |
| --- | --- | --- |
| 0. Recover a reproducible baseline | Current-source focused tests build and run with documented tool versions | Inspect the partial generated dependency installation and existing vcpkg baseline before running another reconciliation. Preserve source changes and available artifacts. Avoid unsolicited library upgrades. Record fresh build results separately from the earlier binary-only test run. |
| 1. Client lifecycle and server responsiveness | Disconnecting, reconnecting, or browsing from a slow client does not accumulate abandoned sessions or stall another client | Explicit idempotent Session close; disconnect model callbacks; weak ownership; executor confinement; budgets for queued work and outgoing bytes; bounded history workers; subscription reference accounting. Verify repeated connect/disconnect, forced write failure, queue overflow, and one delayed history request alongside another client's live flow. |
| 2. Trustworthy one-minute recording | A restarted server serves recorded columns and reports recording failures accurately | Confirm `heatmap.timeframe: 60000` through `server_config`; the YAML key is `timeframe`. Repair platform-specific lock/sync behavior, sync-error propagation, scheduled flushing, day-writer retirement, and periodic retention of explicitly eligible files. Test writer contention, process termination, corrupt/truncated records, restart gaps, and date rollover using isolated temporary data. Do not silently enable deletion of existing archives. |
| 3. Historical heatmap browsing | Open yesterday, cross a recenter boundary, continue receiving live updates, and return to live | Carry request identity and availability through client DTOs; merge older/live columns into one bounded cache; prepare viewport uploads; preserve per-column price bands; implement loading/gap/floor states. Test live-first/history-later, interleaved responses, duplicate buckets, symbol/timeframe changes, empty chunks, and history older than both RAM and GPU capacity. Visually verify alignment with candles and axes. |
| 4. Usable footprint | Bid/ask numbers and delta agree with the same recorded trades before and after restart | Preserve side/ID in common tape decoding; add bounded time-range reads and duplicate handling; derive separate numeric side totals; version the richer payload; retain scale through staging; render batched numbers. Compare live and replay results for an identical fixture, including late trades and incomplete intervals. Advanced cluster/imbalance modes follow later. |
| 5. Consistent TPO | The same covered session produces the same letters and POC/VAH/VAL live and after reload | W1/Australia boundary arithmetic is fixed and covered around rollover. Deliver the complete mode first for a 24-hour UTC session with proposed 30-minute periods. Share occupancy/session rules between live and history; distinguish approximation and partial coverage. Test ties, empty sessions, reconnection, and bootstrap followed by partial live updates. Keep the current five-day weekly definition until its product meaning is explicitly decided. |
| 6. Larger overview zoom | Zooming out requests bounded detail appropriate to available pixels | Add layer-specific time/price aggregation with explicit coverage and numeric conservation. Volume sums, TPO occupancy, and time-weighted resting liquidity need different operators. Do not average palette colors into analytical data. Compare overview and detailed fixtures, then measure frame time, uploads, memory, and server request latency. |

Slice 3 is the first product milestone. Slices 0–2 make it dependable. Footprint and TPO are release gates of their own, not prerequisites for proving the historical heatmap. The small session-math correction can land early without exposing unfinished TPO controls. Each coherent code slice should have targeted verification and a focused commit under the repository's checkpoint rule.

For the first milestone, declare CPU cache, pending upload, history-worker, page-size, and per-client byte budgets before coding their queues. Limits must be observable and exercised under saturation. Performance acceptance uses a documented Release build, machine, chart size, and representative trace: target smooth 60 FPS, report frame-time distribution and worst stalls, and compare against the restored baseline. The old 100-FPS/1-ms upload estimates are not evidence. Full stress and long-running deployment checks occur when that milestone requires them; this brief has not run them.

**Scope boundaries and product decisions**

Initial engineering assumes BTC-USD and a one-minute heatmap collection profile, matching the prior owner-approved persistence scope. This does not authorize changing an existing live deployment's resolution. Keeping a separate one-second live profile or introducing independent capture/display resolutions can follow without expanding the first milestone.

Recommended initial TPO semantics use observed trade-price occupancy as the authoritative value, and distinguish candle-derived bootstrap where it is offered. A blank area must not imply “nothing traded” when collection was absent. Existing archives lack complete coverage/provenance metadata, so they cannot automatically be promoted to verified-complete history. Conventional range-filled TPO is a possible user-facing convention; changing it requires an explicit definition and consistent live/history behavior.

Three decisions require owner judgment before the relevant release, but do not block baseline and lifecycle fixes:

- **Hosting and retention:** recommend one owner-operated collector initially, with an explicit storage budget and retention policy chosen before unattended deployment. Do not create paid infrastructure or choose an archive-deletion horizon during shape work.
- **Overview ambition:** recommend shipping historical paging at the one-minute anchor first, then coarser zoom. A first release that must show months in one view moves rollup work into the first milestone and increases scope.
- **Session conventions:** recommend 24-hour UTC TPO first. Before weekly/geographic profiles ship, choose seven-day crypto versus five-day trading week, timezone/DST semantics, and whether occupancy means observed prints or a defined range-fill convention.

Deferred capabilities include full raw order-book reconstruction, cross-exchange consolidation, arbitrary per-client aggregation settings, broad renderer/framework replacement, cloud services, billing, advanced footprint styles, and unrelated trading/SEC features. They are not needed to prove this initiative's first workflow. Stored data is a reusable asset, but a live feed cannot reconstruct liquidity outside the times and price bands actually captured.

**Current state and next move**

The baseline builds and the automated suite passes. Server sessions now drain deterministically with bounded event and write queues. One-minute heatmap persistence is enabled with OS-level sync, writer retirement, and non-destructive unlimited retention. Historical heatmap replies carry their request boundary and storage floor, use 1,024-column pages, fill missing timestamps with blank columns, resample changing price bands on the worker thread, and replace the GPU page in one linear batch. Manual history remains stable while live messages continue, and returning to auto-scroll reloads the latest page. Late symbol, timeframe, and request-boundary responses are suppressed. TPO session selection now consistently chooses the latest open at or before the query, including W1 Sunday rollover and the overnight Australia session.

The next heatmap work is visual runtime validation against a live multi-day archive and measured Release-build frame and request latency. Missing historical buckets now carry a separate coverage mask and render as muted bands, distinct from recorded all-zero columns. Page loading and the reached storage floor are visible on the chart. The broader bounded CPU cache, asynchronous server history workers, bidirectional prefetch, and overview rollups remain open. Footprint and TPO retain their separate release gates.
