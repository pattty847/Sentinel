# B1: whole-chunk render-ready tiles vs viewport-clipped sources (2026-09-29)

Slice B1 of `2026-09-gpu-heatmap-integration-plan.md`. Branch `lt-claude/b1-whole-chunk`.
Real recorded BTC-USD data (HMC2, read-only), Apple M4, Metal, Qt 6.11.2.
- **Data range:** the closed range from the start of the recording (2026-09-28, about 02:43 UTC)
  to a pinned end of **2026-09-30T00:00:00Z**, about 45 h of near and deep. Every chunk in this range
  is sealed.
- **One run for every mode:** all four modes ran in one process against that same fixed range, so
  the table compares like with like.
- **Source of every number:** `sentinel-lab --b1-bench b1.json --end-utc 2026-09-30T00:00:00Z`.
  The run's JSON is kept out of the repo; the command regenerates it.

## Recommendation

**Neither pure mode. Use the hybrid (H): time-chunked GPU sources that stay resident, binned per view.**

- The render-ready unit is a 64-column, epoch-aligned tile span (MarketLens-style time chunking). Its
  composed `GpuSource` is built once per span and timeframe, uploaded once, and stays on the GPU
  (a resident pool in `HeatmapGpuBinner`).
- Each visible tile bins only the rows around the view (the view plus one view height each side) at
  the current tick. A tick change or a vertical pan that leaves those rows is a compute pass in the
  same frame. New or revised chunks rebuild only the 1-2 tiles they touch (88-189 ms).
- Decoded chunks live in one process-wide store shared by every chart (zero redundant decodes measured).

Why not W (whole-extent render-ready cells):
- **A tick change is not a next-frame event.** Every visible tile must be re-derived at the new tick.
  W holds the old picture for 34-188 ms (CPU-built cells) or 37-606 ms (GPU-built cells) before the
  crossfade. This breaks spec rule 7. V and H change tick in one frame (16-20 ms at 60 Hz).
- **The "useful price extent" of deep is the whole book.** Coinbase deep has orders across the
  full recorder window, about $21k-$335k: 18k entries per minute spread over 62.8k rows at $5.
  The rows that contain data are therefore about the full envelope. At $10 a 64-minute tile is
  31k rows, i.e. 8 MB of cells, and a deep 1m view at 2x needs 123-146 MB of cells.

Why not V (the current viewport-clipped source):
- **Every exit from the prepared region rebuilds and re-uploads the whole source on the CPU.**
  This includes a large pan (deep 1m: 316-693 ms), a revised chunk (deep 1m: 420-1537 ms) and a
  zoom-out beyond the margin. The cost scales with the raw entries in the region. When zoomed out at
  coarse ticks on deep, the region is most of the book. At 2x deep 1m $100 the first frame took 1.55 s
  warm and the GPU grew to 500 MB.

What H costs:
- Its GPU memory is the raw entries of the tile spans in view plus prefetch, over the **whole** price
  extent: deep 1m 91 MB at 1x and 176 MB at 2x, near 1m 23-47 MB. V clips rows, so at fine ticks
  V is smaller (deep 1m $10: 16 / 51 MB).
- The obvious next step (not measured) is to band the deep sources by price as well as by time. Keep
  tiles for a band of rows around the view resident, instead of the full $21k-$335k book. That keeps
  every property above and brings deep memory towards V.
- This is the "W for near, V for the deep full book" split the task suggested, done inside one
  mechanism, not as two parallel paths (owner rule: no permanent parallel architectures).

Secondary findings:
- W with CPU-built cells (W-cpu) has the fastest first frame and the smallest GPU footprint at coarse
  ticks (deep 1m $100: 7 MB versus V 116 MB). It pays for that in CPU memory: composed spans keep
  long-double numerators, and the capped 256 MiB intermediate cache reached 256 MB on deep 1m. It also loses
  the next-frame tick change. It is not worth a second path.
- W with GPU-built cells (W-gpu) as implemented is limited by the binner's single pending-source slot:
  tile sources upload one after another (8 MiB per frame). H is that design with the limit removed.
- Prefetch of one tile each side (MarketLens: about one chunk) is **not** enough for continuous
  horizontal drags on deep 1m. Small-pan p95 was 55-94 ms (3-5 frames) in W and H versus 1 frame in V,
  whose margin is one view width. Use at least two tiles, or one view width, for H.

## What was built (lab and GPU heatmap target only)

| Piece | File | Notes |
|---|---|---|
| Decoded chunk store | `libs/core/heatmap/ChunkStore.{hpp,cpp}` | Process-wide LRU by bytes (512 MiB). Concurrent `get()` of one key shares a single decode. `revise()`/`reload()` give a new generation per version. |
| Tile math, CPU cells, LRU | `libs/core/heatmap/HeatmapTiles.{hpp,cpp}` | 64-column epoch tiles; chunk plan (deep hour rollups before the watermark, minutes after); absolute bin anchoring; useful extent in integer price units; `buildCells` (binColumn, same states/codes as the kernel); `ByteLru` with protected keys. |
| Clipped composition | `libs/core/heatmap/TimeComposer.{hpp,cpp}` | `compose(pointers, tf, {startMs, endMs, trustedInputs})`: compose a tile span from the chunks around it without copying them. |
| `binInto`, resident pool | `libs/gui/render/heatmap/HeatmapGpuBinner.*` | Bin the active or a resident source into a caller buffer; any number of resident sources. |
| Tile node | `libs/gui/render/heatmap/HeatmapTileNode.*` | Row blocks of 8192 (the deep extent exceeds the 16384-row grid). Hold-until-ready, 150 ms crossfade on a tick change, per-slot fallback for revised tiles, loading hatch, view-row re-binning (H). Draw = one quad per tile block; pan/zoom = uniforms. |
| Display shader | `shaders/heatmap_display.frag` | `dims.z` bit 0 for tiles: clamp columns (tile edge on a pixel centre) and rows (sentinel rows carry the outside-the-book state). V path unchanged. |
| Lab | `libs/gui/lab/LabChunks.*`, `LabItem.*`, `B1Bench.*`, `apps/sentinel-lab` | Prep modes `full`, `viewport`, `whole-chunk`, `whole-chunk-cpu`, `hybrid`; up to 4 charts; debug panel "PREP (B1)" (mode, GPU memory, prep CPU, decoded chunks, intermediates, footprint, last prep ms, builds, tiles, chunk hits/misses/decodes). |

Correctness checks:
- The CPU cells equal the GPU `binInto` cells exactly, for state, code and side (`HeatmapTileGpuTests`).
  This covers $5/$10/$20/$50 ticks, an incompatible $7.50 tick (veil), a 1m/5m grid change, a
  size-scale change, gaps, no-data edges and tiles taller than one row block.
- On real data, W-gpu, W-cpu and H are pixel-identical to each other. Against V they differ only
  where a pixel centre lies exactly on a column or row edge. There, float rounding picks the
  neighbouring bin: 13,515 of the 13,516 differing pixels equal an adjacent V pixel.

## Method

- **Scene:** `lab::OffscreenQuick` renders the real Qt Quick scene graph into a Metal texture. It
  is paced at 60 Hz, and each offscreen frame waits for the GPU. "1x" is 1500x880 physical pixels
  and "2x" is 3000x1760 (DPR 1 with doubled pixels; the physical grid sizes equal DPR 2).
- **Budgets:** one upload budget for every mode, 8 MiB per frame (S4/T used 2 MiB). W/H tile LRU
  256 MiB per chart, W intermediates 256 MiB, chunk store 512 MiB.
- **Session:** each (mode, px, layer, tf, tick) runs in fresh lab items. Fixed Manual preset ticks.
  The view has the newest data at the right edge, 3 px per column (all the data when it is shorter),
  and rows of the tick 3 px tall. The steps are:
  1. First fully drawn frame, **cold** (chunk store and intermediates empty) and **warm** (another
     chart decoded them).
  2. 20 small vertical pans (5 % of the height).
  3. One large vertical pan (3 heights).
  4. 20 small horizontal pans (5 % of the width, back in time).
  5. One large horizontal pan 3 views back, into unprepared and undecoded chunks.
  6. Auto tick with a price zoom-out, 12 % per step, until the tick changes; the latency of that step
     is measured.
  7. A revised newest chunk: `reload()` stores a new version of it.
- **Latency:** wall time from the input to the first frame where the whole view is drawn from the
  prepared representation of the current timeframe and tick. That means no loading hatch from
  missing preparation, no held or stale picture, and no build in flight for the view. One frame is
  about 17-21 ms at 60 Hz; a "20" means next frame.
- **Memory:** "gpu" is the bytes of our QRhi buffers. "cpu" is the prep CPU memory: the V source
  image, W cells not yet uploaded, and the W/H tile intermediates. The decoded chunk store is listed
  separately in the multi-chart table. Process footprint (`task_vm_info`) is in the JSON.

## Results

### Headline (ms at 60 Hz; latency to the first complete frame; GPU MB at the end of the session / peak)

| case | V | W-gpu | W-cpu | H |
|---|---|---|---|---|
| deep 1m $10 1x: warm first frame | 380 | 433 | 171 | 229 |
| deep 1m $10 1x: tick change | **20** | 301 | 146 | **19** |
| deep 1m $10 1x: large vertical pan | 316 | **20** | **16** | **20** |
| deep 1m $10 1x: large horizontal pan | 338 | 242 | 191 | 201 |
| deep 1m $10 1x: revised chunk | 420 | 150 | 114 | 155 |
| deep 1m $100 2x: warm first frame | 1550 | 752 | 180 | 409 |
| deep 1m $100 2x: tick change | **19** | 606 | 168 | **19** |
| deep 1m $100 2x: large vertical pan | 644 | 19 | 19 | 19 |
| deep 1m $100 2x: revised chunk | 1537 | 151 | 106 | 129 |
| deep 1m $100 2x: GPU MB end / peak | 500 / 500 | 55 / 55 | 33 / 33 | 275 / 278 |
| near 1m $1 2x: tick change | **17** | 296 | 71 | **17** |
| near 1m $10 2x: warm first frame | 530 | 377 | 93 | 131 |
| near 1m $10 2x: revised chunk | 609 | 72 | 113 | 93 |
| any: small pans p95 (vertical) | 20 | 20 | 20 | 20 |
| deep 1m: small horizontal pans p95 | 20 | 56-58 | 55-94 | 56-59 |

### Working set right after the first frame (MB, GPU / prep CPU)

| px | layer | tf | tick | V gpu / cpu | W-gpu gpu / cpu | W-cpu gpu / cpu | H gpu / cpu |
|---|---|---|---|---|---|---|---|
| 1x | near | 1m | $1 | 5.2 / 3.7 | 22.4 / 20.8 | 17.0 / 88.7 | 23.0 / 20.8 |
| 1x | near | 1m | $10 | 41.0 / 35.6 | 7.1 / 20.8 | 1.9 / 79.6 | 23.0 / 20.8 |
| 1x | near | 1m | $100 | 41.8 / 36.3 | 5.6 / 20.8 | 0.2 / 78.1 | 23.0 / 20.8 |
| 1x | near | 5m | $1 | 4.0 / 2.7 | 24.8 / 20.6 | 18.4 / 124.0 | 22.5 / 20.6 |
| 1x | near | 5m | $10 | 23.5 / 20.1 | 8.3 / 20.6 | 1.8 / 116.7 | 22.5 / 20.6 |
| 1x | near | 5m | $100 | 24.1 / 20.6 | 6.7 / 20.6 | 0.2 / 115.0 | 22.5 / 20.6 |
| 1x | near | 60m | $1 | 0.7 / 0.4 | 7.7 / 2.3 | 4.9 / 17.4 | 2.9 / 2.3 |
| 1x | near | 60m | $10 | 2.8 / 2.2 | 3.3 / 2.3 | 0.5 / 13.0 | 2.9 / 2.3 |
| 1x | near | 60m | $100 | 2.9 / 2.3 | 2.8 / 2.3 | 0.1 / 12.6 | 2.9 / 2.3 |
| 1x | deep | 1m | $10 | 15.8 / 13.2 | 83.8 / 89.0 | 61.2 / 273.1 | 91.0 / 89.0 |
| 1x | deep | 1m | $100 | 116.2 / 109.5 | 28.7 / 89.0 | 6.9 / 250.8 | 91.0 / 89.0 |
| 1x | deep | 5m | $10 | 6.7 / 5.1 | 83.7 / 63.3 | 61.6 / 256.0 | 65.3 / 63.3 |
| 1x | deep | 5m | $100 | 49.7 / 43.3 | 28.3 / 63.3 | 6.2 / 233.6 | 65.3 / 63.3 |
| 1x | deep | 60m | $10 | 0.8 / 0.5 | 21.8 / 5.4 | 15.5 / 37.1 | 6.1 / 5.4 |
| 1x | deep | 60m | $100 | 4.5 / 3.7 | 7.9 / 5.4 | 1.6 / 30.9 | 6.1 / 5.4 |
| 2x | near | 1m | $1 | 19.4 / 14.5 | 40.0 / 39.5 | 36.7 / 156.3 | 47.1 / 39.5 |
| 2x | near | 1m | $10 | 76.7 / 72.6 | 8.9 / 39.5 | 3.7 / 149.7 | 47.1 / 39.5 |
| 2x | near | 1m | $100 | 76.7 / 72.6 | 5.8 / 39.5 | 0.4 / 148.0 | 47.1 / 39.5 |
| 2x | near | 5m | $1 | 7.6 / 5.3 | 24.8 / 20.6 | 18.4 / 124.0 | 24.3 / 20.6 |
| 2x | near | 5m | $10 | 24.8 / 20.6 | 8.3 / 20.6 | 1.8 / 116.7 | 24.3 / 20.6 |
| 2x | near | 5m | $100 | 24.8 / 20.6 | 6.7 / 20.6 | 0.2 / 115.0 | 24.3 / 20.6 |
| 2x | near | 60m | $1 | 1.2 / 0.7 | 7.7 / 2.3 | 4.9 / 17.4 | 3.4 / 2.3 |
| 2x | near | 60m | $10 | 2.9 / 2.3 | 3.3 / 2.3 | 0.5 / 13.0 | 3.4 / 2.3 |
| 2x | near | 60m | $100 | 2.9 / 2.3 | 2.8 / 2.3 | 0.1 / 12.6 | 3.4 / 2.3 |
| 2x | deep | 1m | $10 | 51.0 / 42.5 | 145.6 / 168.5 | 123.0 / 273.0 | 175.7 / 168.5 |
| 2x | deep | 1m | $100 | 248.2 / 239.2 | 34.9 / 168.5 | 13.1 / 253.8 | 175.7 / 168.5 |
| 2x | deep | 5m | $10 | 12.9 / 10.0 | 83.7 / 63.3 | 61.6 / 256.4 | 67.0 / 63.3 |
| 2x | deep | 5m | $100 | 65.2 / 56.5 | 28.3 / 63.3 | 6.2 / 249.5 | 67.0 / 63.3 |
| 2x | deep | 60m | $10 | 1.4 / 0.9 | 21.8 / 5.4 | 15.5 / 44.9 | 6.5 / 5.4 |
| 2x | deep | 60m | $100 | 5.8 / 4.8 | 7.9 / 5.4 | 1.6 / 30.9 | 6.5 / 5.4 |

### Multi-chart: 1, 2 and 4 lab items on one process-wide chunk store

Timeframes {1m}, {1m, 5m}, {1m, 5m, 1h, 15m}; Auto tick; each chart's default view (newest 24 h,
+-2 %). "loads" are decodes; "shared" are `get()` calls that waited on another chart's decode.
"redund" are decodes beyond one per stored chunk: **0 in every run**.

```
mode            lay  charts |    gpuMB   prepMB  chunkMB  interMB  loads shared |  redund settleMs
viewport        near      1 |     95.3     89.5     85.1      0.0     42      0 |       0     1044
viewport        near      2 |    119.1    110.1     85.1      0.0     42     42 |       0      977
viewport        near      4 |    131.1    120.1     85.1      0.0     42    126 |       0      975
viewport        deep      1 |     34.7     28.5    287.7      0.0     42      0 |       0     1298
viewport        deep      2 |     41.9     34.3    287.7      0.0     42     42 |       0     1260
viewport        deep      4 |     45.5     36.8    297.6      0.0     44     85 |       0     1219
whole-chunk     near      1 |     10.4      0.0     53.4     55.3     26      0 |       0      656
whole-chunk     near      2 |     17.9      0.0     66.1     71.2     32      1 |       0      664
whole-chunk     near      4 |     28.9      0.0     85.1     81.2     42     29 |       0      692
whole-chunk     deep      1 |    199.1      0.0    202.8    220.9     26      0 |       0     1191
whole-chunk     deep      2 |    267.2      0.0    236.2    253.0     32      2 |       0     1110
whole-chunk     deep      4 |    332.2      0.0    297.6    250.3     44      9 |       0     1099
whole-chunk-cpu near      1 |      5.0      0.2     53.4    206.8     26      1 |       0      252
whole-chunk-cpu near      2 |      6.3      1.9     66.1    241.3     32     11 |       0      250
whole-chunk-cpu near      4 |      7.8      0.5     85.1    248.7     42     21 |       0      367
whole-chunk-cpu deep      1 |    184.2     15.3    202.8    238.9     26      0 |       0      519
whole-chunk-cpu deep      2 |    222.8     23.0    236.2    237.8     32      9 |       0      554
whole-chunk-cpu deep      4 |    261.4     23.2    297.6    237.6     44     11 |       0      642
hybrid          near      1 |     61.5      0.0     53.4     55.3     26      0 |       0      309
hybrid          near      2 |     79.1      0.0     66.1     71.2     32      2 |       0      307
hybrid          near      4 |     90.8      0.0     85.1     81.2     42     27 |       0      341
hybrid          deep      1 |    226.9      0.0    202.8    220.9     26      0 |       0      720
hybrid          deep      2 |    280.7      0.0    236.2    253.0     32      9 |       0      757
hybrid          deep      4 |    309.2      0.0    297.6    253.8     44     13 |       0      758
```

The decoded chunk store is the dominant shared cost: deep is 203 MB for the chunks one 1m chart needs,
and 298 MB for all 45 hours. It does not grow with the number of charts. Per-chart GPU grows
sub-linearly because coarser timeframes need fewer entries.

## Budget inputs (owner item 9)

- **One large chart (H, 2x, deep 1m):** about 176 MB of GPU for view plus prefetch right after the
  first frame. Over a session the tile LRU reaches its 256 MiB cap and goes above it (275 MB),
  because tiles in view and tiles still drawn are protected. A per-chart cap of about 320 MiB fits
  2x deep 1m without evicting tiles in view.
- **Several charts:** 4 deep charts with H: 309 MB GPU, 298 MB decoded chunks, 254 MB intermediates
  (capped). Near is about 30 % of that.
- **Active+spare:** V keeps active and spare source sets. Its end-of-session GPU (up to 500 MB)
  includes the spare's capacity. H has no spare set: a tick change re-bins from the resident sources.
- **Whole-chunk cells (W):** 8 MB per 64-column deep tile at $10 and 0.8 MB at $100; near $1 is
  about 2.3 MB.

## Review fixes (Codex review, round 1)

- **Tiles that lost their GPU copy come back.** Once a tile is uploaded, its CPU data is released.
  A QRhi or scene-graph recreation, or a replaced node, now shows up as a tile the node no longer
  reports resident. The lab drops such tiles and rebuilds them from the chunk store. Test:
  `HeatmapTileLabItem.TilesComeBackAfterTheQRhiIsRecreated` moves a lab item to a new scene with a
  new QRhi, for W-gpu, W-cpu and H. It fails without the fix.
- **A slow load cannot overwrite a newer revision.** Every acquisition (a `get()` load, `revise()`,
  `reload()`) takes a ticket when it starts, and a completion never replaces a version with a newer
  ticket. Test: `ASlowLoadNeverReplacesARevisionThatArrivedDuringIt`, gated by a promise, not by
  timing. It fails without the fix.
- **Revisions reach every chart.**
  - The store keeps the latest generation of every key after eviction. An evicted chunk therefore no
    longer reads as unchanged after a revision.
  - An evicted sealed chunk keeps its generation when it is re-read, because sealed content never
    changes. So eviction alone does not force a rebuild; an open chunk that is re-read gets a new
    generation.
  - Every revision is broadcast as a queued Qt signal to all lab items, which also pick up the
    refreshed availability.
  - Tests: `LatestGenerationSurvivesEvictionAndSealedReloadsKeepIt`,
    `RevisionListenerHearsEveryRevision`, `ARevisionFromOneChartRebuildsTheOtherChartsTiles` (the last
    fails without the broadcast).
- **Deterministic shared-load test.** The loaders block on a promise until the store's counters show
  every caller waiting. No sleeps.
- **Methodology.** Every mode was rerun in one process against the pinned, closed range above. The
  earlier live-recording runs (one of them a hybrid-only rerun) gave the same picture within a few
  percent. The recommendation did not change.

## Not verified / limits

- 2x is emulated with doubled physical pixels at DPR 1. The on-screen window at DPR 2 was only
  screenshotted, not benchmarked.
- "Revised chunk" forces a new version of the newest (sealed) chunk through `reload()`. It measures
  the cost of re-preparing whatever depends on that chunk; the content happens to be unchanged.
- "Cold" means that our caches are empty; the OS file cache on the T7 was warm.
- Only Metal was tested (no D3D11 run: FM-098).
- The W resolution indicator in the lab is simplified (it names the grid, not the time range).
- The price-banded H variant recommended above is **not measured**.
- The chunk "hit%" column counts store `get()` calls. W/H call `get()` per tile (each hour chunk
  serves up to two 64-minute tiles), so their hit rate reads lower. Decodes per chunk is the reuse
  measure, and it is 1.0 everywhere.

## Reproduce

```
# the interactive lab: toggle "Prep" in the toolbar (full | viewport | whole-chunk | whole-chunk CPU | hybrid);
# add --end-utc 2026-09-30T00:00:00Z to see exactly the benchmarked range
./build/mac-clang/apps/sentinel-lab/sentinel-lab --layer deep --tf 1 --prep hybrid
./build/mac-clang/apps/sentinel-lab/sentinel-lab --layer deep --charts 4 --prep viewport
# the benchmark (about 4.5 min; --b1-quick is 1x, 1m and 1h only, about 1.5 min; --b1-modes hybrid,viewport picks modes)
./build/mac-clang/apps/sentinel-lab/sentinel-lab --b1-bench b1.json --end-utc 2026-09-30T00:00:00Z
```

## Appendix: full session table

`vPan95`/`hPan95` = p95 of 20 small pans. `vPanBig`/`hPanBig` = one 3-view pan ("-" when the
recording is shorter than 2 views). `tickChg` = the Auto step that changed the tick. `gpuMB` =
the end of the session (the LRU fills with other ticks and positions). `cpuMB` = the prep CPU plus
intermediates.

```
mode            px lay    tf  tick | ttfvCold ttfvWarm |  vPan95  vPanBig |  hPan95  hPanBig |  tickChg   revise |   gpuMB  peakMB   cpuMB |   hit%
viewport        1x near   1m    $1 |      273      150 |      20       93 |      20      308 |       19      166 |    10.5    10.5     3.7 |     85
whole-chunk     1x near   1m    $1 |      338      235 |      20       17 |      20      251 |      145       74 |    75.6    75.6    64.2 |     31
whole-chunk-cpu 1x near   1m    $1 |      207       89 |      20       20 |      36      155 |       36       76 |    70.2    70.2   242.1 |     65
hybrid          1x near   1m    $1 |      220       98 |      20       19 |      20      136 |       19       94 |    76.6    76.6    64.2 |     33
viewport        1x near   1m   $10 |      415      287 |      20       97 |      20      379 |       20      314 |    82.2    82.2    35.6 |     85
whole-chunk     1x near   1m   $10 |      340      246 |      20       20 |      20      240 |      148      151 |    12.5    12.5    64.2 |     36
whole-chunk-cpu 1x near   1m   $10 |      189       78 |      20       19 |      37      114 |       37       93 |     7.0     7.0   240.2 |     64
hybrid          1x near   1m   $10 |      220       94 |      20       19 |      20      150 |       19      124 |    76.6    76.6    64.2 |     33
viewport        1x near   1m  $100 |      422      300 |      20       94 |      21      353 |       20      295 |    83.9    83.9    36.3 |     85
whole-chunk     1x near   1m  $100 |      344      250 |      20       20 |      20      246 |      152      129 |     6.2     6.2    64.2 |     33
whole-chunk-cpu 1x near   1m  $100 |      184       76 |      20       18 |      38      108 |       38       73 |     0.7     0.7   240.0 |     67
hybrid          1x near   1m  $100 |      209       97 |      20       17 |      21      151 |       18       88 |    76.6    76.6    64.2 |     33
viewport        1x near   5m    $1 |      405      127 |      20       74 |      20        - |       17      156 |     8.1     8.1     2.7 |    100
whole-chunk     1x near   5m    $1 |      380      267 |      20       19 |      20        - |      156      129 |    36.2    36.2    23.1 |    100
whole-chunk-cpu 1x near   5m    $1 |      239       97 |      20       20 |      20        - |       74      134 |    29.7    29.7   131.3 |    100
hybrid          1x near   5m    $1 |      237       97 |      20       17 |      21        - |       20      114 |    27.2    27.2    23.1 |    100
viewport        1x near   5m   $10 |      472      229 |      20       74 |      20        - |       20      239 |    47.3    47.3    20.1 |    100
whole-chunk     1x near   5m   $10 |      410      246 |      21       16 |      20        - |      156      111 |     9.5     9.5    23.1 |    100
whole-chunk-cpu 1x near   5m   $10 |      227       71 |      20       20 |      20        - |       39      128 |     3.0     3.0   129.4 |    100
hybrid          1x near   5m   $10 |      237       92 |      20       20 |      20        - |       19      111 |    27.2    27.2    23.1 |    100
viewport        1x near   5m  $100 |      484      227 |      20       76 |      20        - |       20      246 |    48.4    48.4    20.6 |    100
whole-chunk     1x near   5m  $100 |      356      244 |      20       20 |      20        - |      147      149 |     6.8     6.8    23.1 |    100
whole-chunk-cpu 1x near   5m  $100 |      223       71 |      20       19 |      20        - |       38      131 |     0.3     0.3   129.2 |    100
hybrid          1x near   5m  $100 |      242       94 |      21       16 |      20        - |       18      113 |    27.2    27.2    23.1 |    100
viewport        1x near  60m    $1 |      338       73 |      21       53 |      20        - |       20      151 |     1.2     1.2     0.4 |    100
whole-chunk     1x near  60m    $1 |      282       93 |      20       16 |      20        - |       39      134 |    12.7    12.7     3.1 |    100
whole-chunk-cpu 1x near  60m    $1 |      263       56 |      20       18 |      20        - |       38      108 |     9.9     9.9    19.7 |    100
hybrid          1x near  60m    $1 |      279       54 |      20       20 |      20        - |       19      114 |     4.6     4.6     3.1 |    100
viewport        1x near  60m   $10 |      376       94 |      20       55 |      20        - |       19      145 |     5.4     5.4     2.2 |    100
whole-chunk     1x near  60m   $10 |      281       94 |      20       21 |      20        - |       38      131 |     3.8     3.8     3.1 |    100
whole-chunk-cpu 1x near  60m   $10 |      284       55 |      20       18 |      20        - |       39      127 |     1.0     1.0    17.5 |    100
hybrid          1x near  60m   $10 |      265       59 |      20       19 |      20        - |       20      134 |     4.6     4.6     3.1 |    100
viewport        1x near  60m  $100 |      371      114 |      20       57 |      20        - |       18      150 |     5.5     5.5     2.3 |    100
whole-chunk     1x near  60m  $100 |      285       88 |      20       16 |      20        - |       38      131 |     2.9     2.9     3.1 |    100
whole-chunk-cpu 1x near  60m  $100 |      265       54 |      20       17 |      20        - |       36      131 |     0.1     0.1    17.3 |    100
hybrid          1x near  60m  $100 |      269       57 |      20       17 |      20        - |       16      153 |     4.6     4.6     3.1 |    100
viewport        1x deep   1m   $10 |      601      380 |      20      316 |      20      338 |       20      420 |    31.4    31.4    13.2 |     86
whole-chunk     1x deep   1m   $10 |      589      433 |      20       20 |      56      242 |      301      150 |   264.1   264.1   234.0 |     37
whole-chunk-cpu 1x deep   1m   $10 |      328      171 |      20       16 |      55      191 |      146      114 |   241.6   241.6   258.8 |     65
hybrid          1x deep   1m   $10 |      387      229 |      20       20 |      57      201 |       19      155 |   246.4   246.4   234.0 |     34
viewport        1x deep   1m  $100 |     1032      803 |      20      371 |      20      580 |       20      799 |   232.5   232.5   109.5 |     86
whole-chunk     1x deep   1m  $100 |      600      434 |      20       20 |      56      240 |      290      164 |    46.8    46.8   234.0 |     32
whole-chunk-cpu 1x deep   1m  $100 |      240      116 |      20       20 |      73      126 |      109       98 |    24.2    24.2   250.8 |     65
hybrid          1x deep   1m  $100 |      393      224 |      20       18 |      56      223 |       17      131 |   246.4   246.4   234.0 |     37
viewport        1x deep   5m   $10 |      679      224 |      21      195 |      20        - |       20      283 |    13.6    13.6     5.1 |    100
whole-chunk     1x deep   5m   $10 |      494      380 |      20       17 |      20        - |      217      187 |   122.2   122.2    73.3 |    100
whole-chunk-cpu 1x deep   5m   $10 |      323      171 |      20       19 |      20        - |      110      148 |   100.1   100.1   262.2 |    100
hybrid          1x deep   5m   $10 |      367      190 |      20       18 |      20        - |       18      187 |    77.4    77.4    73.3 |    100
viewport        1x deep   5m  $100 |      820      417 |      20      197 |      20        - |       19      430 |    99.6    99.6    43.3 |    100
whole-chunk     1x deep   5m  $100 |      511      384 |      20       20 |      20        - |      222      188 |    32.2    32.2    73.3 |    100
whole-chunk-cpu 1x deep   5m  $100 |      282       94 |      20       17 |      20        - |       91      147 |    10.0    10.0   245.5 |    100
hybrid          1x deep   5m  $100 |      353      191 |      20       20 |      20        - |       20      189 |    77.4    77.4    73.3 |    100
viewport        1x deep  60m   $10 |      154       74 |      20       36 |      20        - |       20      114 |     1.4     1.4     0.5 |    100
whole-chunk     1x deep  60m   $10 |      185      110 |      20       19 |      20        - |       37      107 |    29.6    29.6     5.4 |    100
whole-chunk-cpu 1x deep  60m   $10 |      149       72 |      20       18 |      20        - |       54      112 |    23.2    23.2    29.4 |    100
hybrid          1x deep  60m   $10 |      136       55 |      20       20 |      20        - |       18      127 |     6.5     6.5     5.4 |    100
viewport        1x deep  60m  $100 |      173       91 |      20       39 |      20        - |       19      118 |     8.7     8.7     3.7 |    100
whole-chunk     1x deep  60m  $100 |      230      128 |      20       17 |      21        - |       40      125 |     8.6     8.6     5.4 |    100
whole-chunk-cpu 1x deep  60m  $100 |      132       71 |      21       17 |      20        - |       34      126 |     2.3     2.3    29.4 |    100
hybrid          1x deep  60m  $100 |      129       58 |      20       19 |      20        - |       18      132 |     6.5     6.5     5.4 |    100
viewport        2x near   1m    $1 |      567      279 |      20      258 |      20        - |       17      303 |    39.2    39.2    14.5 |    100
whole-chunk     2x near   1m    $1 |      453      395 |      20       19 |      39        - |      296      111 |    97.5    97.5    79.1 |     51
whole-chunk-cpu 2x near   1m    $1 |      264      134 |      20       20 |      72        - |       71       95 |    92.0    92.0   255.9 |     83
hybrid          2x near   1m    $1 |      243      133 |      20       16 |      37        - |       17       91 |   108.8   108.8    79.1 |     51
viewport        2x near   1m   $10 |      810      530 |      20      167 |      20        - |       19      609 |   155.0   155.0    72.6 |    100
whole-chunk     2x near   1m   $10 |      487      377 |      20       19 |      37        - |      295       72 |    14.7    14.7    79.1 |     51
whole-chunk-cpu 2x near   1m   $10 |      243       93 |      20       20 |      56        - |       55      113 |     9.2     9.2   254.0 |     83
hybrid          2x near   1m   $10 |      282      131 |      20       20 |      39        - |       18       93 |   108.8   108.8    79.1 |     51
viewport        2x near   1m  $100 |      748      539 |      20      148 |      20        - |       17      549 |   155.0   155.0    72.6 |    100
whole-chunk     2x near   1m  $100 |      458      381 |      20       17 |      56        - |      297       93 |     6.4     6.4    79.1 |     49
whole-chunk-cpu 2x near   1m  $100 |      207       80 |      20       19 |      70        - |       53       74 |     1.0     1.0   253.8 |     83
hybrid          2x near   1m  $100 |      274      149 |      20       19 |      37        - |       18       93 |   108.8   108.8    79.1 |     51
viewport        2x near   5m    $1 |      393      135 |      20      132 |      20        - |       17      180 |    15.8    15.8     5.3 |    100
whole-chunk     2x near   5m    $1 |      384      259 |      20       18 |      20        - |      145      125 |    36.2    36.2    23.1 |    100
whole-chunk-cpu 2x near   5m    $1 |      236       94 |      20       18 |      20        - |       56      108 |    29.7    29.7   131.3 |    100
hybrid          2x near   5m    $1 |      239       95 |      20       19 |      20        - |       19      115 |    31.0    31.0    23.1 |    100
viewport        2x near   5m   $10 |      481      207 |      20       75 |      20        - |       18      259 |    50.2    50.2    20.6 |    100
whole-chunk     2x near   5m   $10 |      360      236 |      20       18 |      20        - |      144      114 |     9.5     9.5    23.1 |    100
whole-chunk-cpu 2x near   5m   $10 |      241       70 |      20       19 |      20        - |       35      128 |     3.0     3.0   129.4 |    100
hybrid          2x near   5m   $10 |      242       96 |      20       19 |      20        - |       20      128 |    31.0    31.0    23.1 |    100
viewport        2x near   5m  $100 |      501      234 |      20       73 |      20        - |       20      248 |    50.2    50.2    20.6 |    100
whole-chunk     2x near   5m  $100 |      371      265 |      21       20 |      20        - |      151      127 |     6.8     6.8    23.1 |    100
whole-chunk-cpu 2x near   5m  $100 |      255       74 |      20       17 |      20        - |       35      128 |     0.3     0.3   129.2 |    100
hybrid          2x near   5m  $100 |      223       97 |      20       17 |      20        - |       18      146 |    31.0    31.0    23.1 |    100
viewport        2x near  60m    $1 |      367       90 |      20       78 |      20        - |       18      170 |     2.2     2.2     0.7 |    100
whole-chunk     2x near  60m    $1 |      308      116 |      20       19 |      20        - |       40      130 |    12.7    12.7     3.1 |    100
whole-chunk-cpu 2x near  60m    $1 |      320       66 |      20       18 |      20        - |       38      150 |     9.9     9.9    19.7 |    100
hybrid          2x near  60m    $1 |      264       54 |      20       19 |      21        - |       18      130 |     5.8     5.8     3.1 |    100
viewport        2x near  60m   $10 |      389       95 |      20       76 |      20        - |       19      164 |     5.7     5.7     2.3 |    100
whole-chunk     2x near  60m   $10 |      279       97 |      20       18 |      20        - |       37      128 |     3.8     3.8     3.1 |    100
whole-chunk-cpu 2x near  60m   $10 |      268       57 |      20       20 |      20        - |       34      146 |     1.0     1.0    17.5 |    100
hybrid          2x near  60m   $10 |      263       55 |      20       17 |      20        - |       19      130 |     5.8     5.8     3.1 |    100
viewport        2x near  60m  $100 |      370      108 |      20       56 |      20        - |       18      159 |     5.7     5.7     2.3 |    100
whole-chunk     2x near  60m  $100 |      279       95 |      20       17 |      20        - |       37      149 |     2.9     2.9     3.1 |    100
whole-chunk-cpu 2x near  60m  $100 |      263       56 |      20       19 |      20        - |       39      152 |     0.1     0.1    17.3 |    100
hybrid          2x near  60m  $100 |      273       57 |      20       19 |      20        - |       17      132 |     5.8     5.8     3.1 |    100
viewport        2x deep   1m   $10 |     1149      734 |      20      693 |      20        - |       19      738 |   103.5   103.5    42.5 |    100
whole-chunk     2x deep   1m   $10 |      863      745 |      20       19 |      58        - |      599      120 |   272.2   275.9   255.1 |     59
whole-chunk-cpu 2x deep   1m   $10 |      426      280 |      20       18 |      94        - |      188      146 |   249.6   253.3   257.7 |     83
hybrid          2x deep   1m   $10 |      587      425 |      20       18 |      58        - |       18      127 |   274.5   278.4   248.6 |     49
viewport        2x deep   1m  $100 |     1971     1550 |      20      644 |      20        - |       19     1537 |   500.1   500.1   239.2 |    100
whole-chunk     2x deep   1m  $100 |      880      752 |      20       19 |      56        - |      606      151 |    55.3    55.3   255.1 |     56
whole-chunk-cpu 2x deep   1m  $100 |      333      180 |      20       19 |      73        - |      168      106 |    32.7    32.7   251.9 |     83
hybrid          2x deep   1m  $100 |      579      409 |      20       19 |      59        - |       19      129 |   274.5   278.4   248.6 |     49
viewport        2x deep   5m   $10 |      687      265 |      20      223 |      20        - |       20      302 |    26.5    26.5    10.0 |    100
whole-chunk     2x deep   5m   $10 |      509      371 |      20       17 |      20        - |      219      180 |   122.2   122.2    73.3 |    100
whole-chunk-cpu 2x deep   5m   $10 |      337      167 |      20       19 |      20        - |      111      146 |   100.1   100.1   262.2 |    100
hybrid          2x deep   5m   $10 |      373      187 |      20       19 |      20        - |       20      169 |    81.2    81.2    73.3 |    100
viewport        2x deep   5m  $100 |      951      580 |      20      207 |      20        - |       17      501 |   131.6   131.6    56.5 |    100
whole-chunk     2x deep   5m  $100 |      501      370 |      20       18 |      20        - |      228      186 |    32.2    32.2    73.3 |    100
whole-chunk-cpu 2x deep   5m  $100 |      371      150 |      20       18 |      20        - |      113      149 |    10.0    10.0   256.6 |    100
hybrid          2x deep   5m  $100 |      372      185 |      20       18 |      20        - |       20      182 |    81.2    81.2    73.3 |    100
viewport        2x deep  60m   $10 |      155       69 |      20       53 |      20        - |       19      155 |     2.3     2.3     0.9 |    100
whole-chunk     2x deep  60m   $10 |      186      111 |      20       19 |      20        - |       38      131 |    29.6    29.6     5.4 |    100
whole-chunk-cpu 2x deep  60m   $10 |      154       71 |      20       19 |      20        - |       36      110 |    23.2    23.2    29.4 |    100
hybrid          2x deep  60m   $10 |      154       56 |      20       18 |      20        - |       19      126 |     7.4     7.4     5.4 |    100
viewport        2x deep  60m  $100 |      291       95 |      20       55 |      20        - |       17      121 |    11.5    11.5     4.8 |    100
whole-chunk     2x deep  60m  $100 |      206      112 |      20       19 |      20        - |       38      108 |     8.6     8.6     5.4 |    100
whole-chunk-cpu 2x deep  60m  $100 |      150       70 |      20       19 |      20        - |       37      134 |     2.3     2.3    29.4 |    100
hybrid          2x deep  60m  $100 |      125       56 |      20       16 |      21        - |       17      127 |     7.4     7.4     5.4 |    100
```
