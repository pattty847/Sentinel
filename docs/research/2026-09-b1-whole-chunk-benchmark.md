# B1: whole-chunk render-ready tiles vs viewport-clipped sources (2026-09-29)

Slice B1 of `2026-09-gpu-heatmap-integration-plan.md`. Branch `lt-claude/b1-whole-chunk`.
Real recorded BTC-USD data (HMC2, read-only, about 2.3 days of near and deep), Apple M4, Metal,
Qt 6.11.2. All numbers come from `sentinel-lab --b1-bench` (JSON of the run kept out of the repo;
rerun the command below to regenerate it).

## Recommendation

**Neither pure mode. Use the hybrid (H): time-chunked GPU sources that stay resident, binned per view.**

- The render-ready unit is a 64-column, epoch-aligned tile span (MarketLens-style time chunking). Its
  composed `GpuSource` is built once per span and timeframe, uploaded once, and stays on the GPU
  (a resident pool in `HeatmapGpuBinner`).
- Each visible tile bins only the rows around the view (the view plus one view height each side) at
  the current tick. A tick change or a vertical pan that leaves those rows is a compute pass in the
  same frame. New or revised chunks rebuild only the 1-2 tiles they touch.
- Decoded chunks live in one process-wide store shared by every chart (zero redundant decodes measured).

Why not W (whole-extent render-ready cells):
- **A tick change is not a next-frame event.** Every visible tile must be re-derived at the new tick.
  W holds the old picture for 36-216 ms (CPU-built cells) or 36-619 ms (GPU-built cells) before the
  crossfade. This breaks spec rule 7. V and H change tick in one frame (16-20 ms at 60 Hz).
- **The "useful price extent" of deep is the whole book.** Coinbase deep has orders across the
  full recorder window, about $21k-$335k: 18k entries per minute spread over 62.8k rows at $5.
  The rows that contain data are therefore about the full envelope. At $10 a 64-minute tile is
  31k rows, i.e. 8 MB of cells, and a deep 1m view at 2x needs 131-153 MB of cells.

Why not V (the current viewport-clipped source):
- **Every exit from the prepared region rebuilds and re-uploads the whole source on the CPU.**
  This includes a large pan (deep 1m: 324-705 ms), a revised live chunk (deep 1m: 395-1660 ms) and a
  zoom-out beyond the margin. The cost scales with the raw entries in the region. When zoomed out at
  coarse ticks on deep, the region is most of the book. At 2x deep 1m $100 the first frame took 1.85 s
  warm and the GPU grew to 511 MB.

What H costs:
- Its GPU memory is the raw entries of the tile spans in view plus prefetch, over the **whole** price
  extent: deep 1m 97 MB at 1x and 182 MB at 2x, near 1m 24-48 MB. V clips rows, so at fine ticks
  V is smaller (deep 1m $10: 16 / 53 MB).
- The obvious next step (not measured) is to band the deep sources by price as well as by time. Keep
  tiles for a band of rows around the view resident, instead of the full $21k-$335k book. That keeps
  every property above and brings deep memory towards V.
- This is the "W for near, V for the deep full book" split the task suggested, done inside one
  mechanism, not as two parallel paths (owner rule: no permanent parallel architectures).

Secondary findings:
- W with CPU-built cells (W-cpu) has the fastest first frame and the smallest GPU footprint at coarse
  ticks (deep 1m $100: 8 MB versus V 116 MB). It pays for that in CPU memory: composed spans keep
  long-double numerators, and the capped 256 MiB intermediate cache sat at 230-260 MB. It also loses
  the next-frame tick change. It is not worth a second path.
- W with GPU-built cells (W-gpu) as implemented is limited by the binner's single pending-source slot:
  tile sources upload one after another (8 MiB per frame). H is that design with the limit removed.
- Prefetch of one tile each side (MarketLens: about one chunk) is **not** enough for continuous
  horizontal drags on deep 1m. Small-pan p95 was 55-95 ms (3-5 frames) in W and H versus 1 frame in V,
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
  7. A revised newest chunk, re-read from the live recording.
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
| deep 1m $10 1x: warm first frame | 381 | 453 | 190 | 242 |
| deep 1m $10 1x: tick change | **20** | 320 | 161 | **20** |
| deep 1m $10 1x: large vertical pan | 324 | **18** | **18** | **17** |
| deep 1m $10 1x: large horizontal pan | 423 | 277 | 245 | 274 |
| deep 1m $10 1x: revised chunk | 395 | 125 | 118 | 146 |
| deep 1m $100 2x: warm first frame | 1850 | 735 | 182 | 420 |
| deep 1m $100 2x: tick change | **17** | 610 | 182 | 130 (budget evictions) |
| deep 1m $100 2x: large vertical pan | 642 | 19 | 20 | 19 |
| deep 1m $100 2x: revised chunk | 1660 | 160 | 110 | 151 |
| deep 1m $100 2x: GPU MB end / peak | 511 / 511 | 56 / 56 | 34 / 34 | 226 / 266 |
| near 1m $1 2x: tick change | **19** | 311 | 78 | **18** |
| near 1m $10 2x: warm first frame | 545 | 412 | 75 | 132 |
| near 1m $10 2x: revised chunk | 542 | 113 | 76 | 96 |
| any: small pans p95 (vertical) | 20 | 20 | 20 | 20 |
| deep 1m: small horizontal pans p95 | 20 | 38-59 | 74-95 | 53-78 |

### Working set right after the first frame (MB, GPU / prep CPU)

| px | layer | tf | tick | V gpu / cpu | W-gpu gpu / cpu | W-cpu gpu / cpu | H gpu / cpu |
|---|---|---|---|---|---|---|---|
| 1x | near | 1m | $1 | 5.2 / 3.7 | 24.4 / 22.0 | 21.2 / 90.9 | 24.4 / 22.0 |
| 1x | near | 1m | $10 | 40.8 / 35.4 | 7.3 / 22.0 | 2.1 / 84.1 | 24.4 / 22.0 |
| 1x | near | 1m | $100 | 41.9 / 36.4 | 5.6 / 22.0 | 0.2 / 82.5 | 24.4 / 22.0 |
| 1x | near | 5m | $1 | 4.3 / 3.0 | 27.0 / 21.4 | 20.5 / 128.7 | 23.6 / 21.4 |
| 1x | near | 5m | $10 | 24.5 / 20.9 | 8.5 / 21.4 | 2.1 / 120.2 | 23.6 / 21.4 |
| 1x | near | 5m | $100 | 25.0 / 21.4 | 6.7 / 21.4 | 0.2 / 119.7 | 23.6 / 21.4 |
| 1x | near | 60m | $1 | 0.8 / 0.4 | 7.8 / 2.3 | 4.9 / 17.7 | 3.0 / 2.3 |
| 1x | near | 60m | $10 | 2.9 / 2.3 | 3.4 / 2.3 | 0.5 / 13.3 | 3.0 / 2.3 |
| 1x | near | 60m | $100 | 2.9 / 2.3 | 2.9 / 2.3 | 0.1 / 12.9 | 3.0 / 2.3 |
| 1x | deep | 1m | $10 | 15.9 / 13.2 | 91.1 / 94.6 | 76.6 / 249.6 | 96.8 / 94.6 |
| 1x | deep | 1m | $100 | 116.0 / 109.4 | 29.1 / 94.6 | 7.7 / 235.8 | 97.0 / 94.8 |
| 1x | deep | 5m | $10 | 7.0 / 5.4 | 91.4 / 66.5 | 69.3 / 244.3 | 68.6 / 66.5 |
| 1x | deep | 5m | $100 | 52.1 / 45.4 | 29.1 / 66.5 | 6.9 / 236.9 | 68.6 / 66.5 |
| 1x | deep | 60m | $10 | 0.8 / 0.5 | 22.0 / 5.6 | 15.5 / 45.7 | 6.2 / 5.6 |
| 1x | deep | 60m | $100 | 4.6 / 3.8 | 8.0 / 5.6 | 1.6 / 31.0 | 6.2 / 5.6 |
| 2x | near | 1m | $1 | 20.3 / 15.3 | 42.1 / 40.9 | 38.8 / 157.2 | 48.5 / 40.9 |
| 2x | near | 1m | $10 | 76.6 / 72.5 | 9.1 / 40.9 | 3.9 / 156.9 | 48.5 / 40.9 |
| 2x | near | 1m | $100 | 76.6 / 72.5 | 5.8 / 40.9 | 0.4 / 153.1 | 48.5 / 40.9 |
| 2x | near | 5m | $1 | 8.1 / 5.7 | 27.0 / 21.4 | 20.5 / 128.7 | 25.5 / 21.4 |
| 2x | near | 5m | $10 | 25.8 / 21.4 | 8.5 / 21.4 | 2.1 / 120.4 | 25.5 / 21.4 |
| 2x | near | 5m | $100 | 25.8 / 21.4 | 6.7 / 21.4 | 0.2 / 119.7 | 25.5 / 21.4 |
| 2x | near | 60m | $1 | 1.2 / 0.7 | 7.8 / 2.3 | 4.9 / 15.3 | 3.4 / 2.3 |
| 2x | near | 60m | $10 | 3.0 / 2.3 | 3.4 / 2.3 | 0.5 / 13.3 | 3.4 / 2.3 |
| 2x | near | 60m | $100 | 3.0 / 2.3 | 2.9 / 2.3 | 0.1 / 12.9 | 3.4 / 2.3 |
| 2x | deep | 1m | $10 | 52.5 / 43.9 | 153.3 / 174.4 | 130.7 / 259.6 | 182.0 / 174.4 |
| 2x | deep | 1m | $100 | 253.8 / 244.2 | 35.7 / 174.6 | 13.1 / 241.8 | 182.1 / 174.6 |
| 2x | deep | 5m | $10 | 13.6 / 10.5 | 91.4 / 66.5 | 69.3 / 251.0 | 70.6 / 66.5 |
| 2x | deep | 5m | $100 | 68.4 / 59.2 | 29.1 / 66.5 | 6.9 / 234.1 | 70.6 / 66.5 |
| 2x | deep | 60m | $10 | 1.4 / 0.9 | 22.0 / 5.6 | 15.5 / 45.7 | 6.7 / 5.6 |
| 2x | deep | 60m | $100 | 6.0 / 5.0 | 8.0 / 5.6 | 1.6 / 31.8 | 6.7 / 5.6 |

### Multi-chart: 1, 2 and 4 lab items on one process-wide chunk store

Timeframes {1m}, {1m, 5m}, {1m, 5m, 1h, 15m}; Auto tick; each chart's default view (newest 24 h,
+-2 %). "loads" are decodes; "shared" are `get()` calls that waited on another chart's decode.
"redund" are decodes beyond one per stored chunk: **0 in every run**.

```
mode            lay  charts |    gpuMB   prepMB  chunkMB  interMB  loads shared |  redund settleMs
viewport        near      1 |     99.4     93.2     88.6      0.0     44      0 |       0     1031
viewport        near      2 |    124.2    114.6     88.6      0.0     44     44 |       0     1007
viewport        near      4 |    136.5    124.9     88.6      0.0     44    132 |       0      987
viewport        deep      1 |     36.3     30.0    302.1      0.0     44      0 |       0     1380
viewport        deep      2 |     43.9     36.1    302.1      0.0     44     44 |       0     1321
viewport        deep      4 |     47.6     38.7    312.4      0.0     47     89 |       0     1400
whole-chunk     near      1 |     10.6      0.0     52.9     54.5     26      0 |       0      632
whole-chunk     near      2 |     18.3      0.0     69.7     71.2     34      7 |       0      662
whole-chunk     near      4 |     29.6      0.0     88.6     81.6     44     24 |       0      658
whole-chunk     deep      1 |    198.8      0.0    206.2    224.2     26      0 |       0     1130
whole-chunk     deep      2 |    274.9      0.0    250.6    246.2     34      1 |       0     1062
whole-chunk     deep      4 |    347.7      0.0    312.4    248.6     47     12 |       0     1062
whole-chunk-cpu near      1 |      5.2      2.4     52.9    203.9     26      0 |       0      224
whole-chunk-cpu near      2 |      6.6      0.2     69.7    255.3     34      1 |       0      259
whole-chunk-cpu near      4 |      8.2      0.5     88.6    252.6     44     18 |       0      307
whole-chunk-cpu deep      1 |    176.5     23.0    206.2    224.9     26      0 |       0      479
whole-chunk-cpu deep      2 |    230.4     15.3    250.6    252.8     34      1 |       0      580
whole-chunk-cpu deep      4 |    284.5     15.3    312.4    224.9     47     19 |       0      729
hybrid          near      1 |     60.4      0.0     52.9     54.5     26      0 |       0      279
hybrid          near      2 |     79.4      0.0     69.7     71.2     34      4 |       0      275
hybrid          near      4 |     91.7      0.0     88.6     81.6     44     24 |       0      309
hybrid          deep      1 |    230.0      0.0    206.2    224.2     26      0 |       0      727
hybrid          deep      2 |    287.3      0.0    250.6    252.7     34      7 |       0      767
hybrid          deep      4 |    317.1      0.0    312.4    253.2     47     11 |       0      754
```

The decoded chunk store is the dominant shared cost: deep is 206 MB for the chunks one 1m chart needs,
and 312 MB for all 2.3 days. It does not grow with the number of charts. Per-chart GPU grows
sub-linearly because coarser timeframes need fewer entries.

## Budget inputs (owner item 9)

- **One large chart (H, 2x, deep 1m):** about 180 MB of GPU for view plus prefetch. The tile LRU
  climbed to its 256 MiB cap during a session (other ticks, earlier positions). A 256 MiB per-chart
  cap is tight for 2x deep 1m: evictions caused the 130 ms tick change above.
- **Several charts:** 4 deep charts with H: 317 MB GPU, 312 MB decoded chunks, 253 MB intermediates
  (capped). Near is about a third of that.
- **Active+spare:** V keeps active and spare source sets. Its measured end-of-session GPU (up to
  511 MB) includes the spare's capacity. H has no spare set: a tick change re-bins from the resident
  sources.
- **Whole-chunk cells (W):** 8 MB per 64-column deep tile at $10 and 0.8 MB at $100; near $1 is
  about 2.3 MB.

## Not verified / limits

- 2x is emulated with doubled physical pixels at DPR 1. The on-screen window at DPR 2 was only
  screenshotted, not benchmarked.
- The recorder was live during the runs. The newest chunk changes between runs, so repeated runs
  differ slightly, and the "revised chunk" step re-reads real new minutes.
- "Cold" means that our caches are empty; the OS file cache on the T7 was warm.
- Only Metal was tested (no D3D11 run: FM-098).
- The H LRU counts a resident source once per tick key that uses it (it overcounts when two ticks
  are cached). The source pool itself is released only when no kept tile references it.
- The W resolution indicator in the lab is simplified (it names the grid, not the time range).
- The price-banded H variant recommended above is **not measured**.
- The chunk "hit%" column counts store `get()` calls. W/H call `get()` per tile (each hour chunk
  serves up to two 64-minute tiles), so their hit rate reads lower. Decodes per chunk is the reuse
  measure, and it is 1.0 everywhere.

## Reproduce

```
# the interactive lab: toggle "Prep" in the toolbar (full | viewport | whole-chunk | whole-chunk CPU | hybrid)
./build/mac-clang/apps/sentinel-lab/sentinel-lab --layer deep --tf 1 --prep hybrid
./build/mac-clang/apps/sentinel-lab/sentinel-lab --layer deep --charts 4 --prep viewport
# the benchmark (about 7 min; --b1-quick is 1x, 1m and 1h only, about 1.5 min)
./build/mac-clang/apps/sentinel-lab/sentinel-lab --b1-bench b1.json
```

## Appendix: full session table

`vPan95`/`hPan95` = p95 of 20 small pans. `vPanBig`/`hPanBig` = one 3-view pan ("-" when the
recording is shorter than 2 views). `tickChg` = the Auto step that changed the tick. `gpuMB` =
the end of the session (the LRU fills with other ticks and positions). `cpuMB` = the prep CPU plus
intermediates.

```
mode            px lay    tf  tick | ttfvCold ttfvWarm |  vPan95  vPanBig |  hPan95  hPanBig |  tickChg   revise |   gpuMB  peakMB   cpuMB |   hit%
viewport        1x near   1m    $1 |      273      154 |      20      101 |      20      285 |       17      155 |    11.3    11.3     3.7 |     85
whole-chunk     1x near   1m    $1 |      285      264 |      20       20 |      21      244 |      165      112 |    82.9    82.9    69.3 |     33
whole-chunk-cpu 1x near   1m    $1 |      161       93 |      20       20 |      37      152 |       57       97 |    77.5    77.5   254.0 |     68
hybrid          1x near   1m    $1 |      189       94 |      20       20 |      21      143 |       20       90 |    78.5    78.5    69.3 |     31
viewport        1x near   1m   $10 |      415      305 |      21       95 |      20      422 |       20      289 |    81.8    81.8    35.4 |     85
whole-chunk     1x near   1m   $10 |      276      258 |      20       19 |      20      268 |      159      110 |    13.1    13.1    69.3 |     38
whole-chunk-cpu 1x near   1m   $10 |      147       76 |      21       20 |      37      115 |       38       97 |     7.8     7.8   250.2 |     66
hybrid          1x near   1m   $10 |      183       94 |      20       19 |      21      143 |       20       96 |    78.5    78.5    69.3 |     36
viewport        1x near   1m  $100 |      412      304 |      20       96 |      20      430 |       18      282 |    84.0    84.0    36.4 |     85
whole-chunk     1x near   1m  $100 |      285      266 |      20       16 |      20      272 |      166      123 |     6.2     6.2    69.3 |     36
whole-chunk-cpu 1x near   1m  $100 |      163       77 |      20       20 |      37      110 |       38       72 |     0.8     0.8   249.9 |     65
hybrid          1x near   1m  $100 |      191      112 |      20       20 |      21      153 |       19       95 |    78.5    78.5    69.3 |     31
viewport        1x near   5m    $1 |      412      125 |      21       79 |      20        - |       19      144 |     8.7     8.7     3.0 |    100
whole-chunk     1x near   5m    $1 |      332      279 |      20       19 |      20        - |      167       98 |    39.4    39.4    22.2 |    100
whole-chunk-cpu 1x near   5m    $1 |      196       95 |      20       20 |      20        - |       58       95 |    32.9    32.9   126.2 |    100
hybrid          1x near   5m    $1 |      209       94 |      20       17 |      20        - |       16       93 |    26.6    26.6    22.2 |    100
viewport        1x near   5m   $10 |      487      228 |      20       73 |      20        - |       20      208 |    49.2    49.2    20.9 |    100
whole-chunk     1x near   5m   $10 |      351      259 |      20       17 |      20        - |      175       91 |     9.8     9.8    22.2 |    100
whole-chunk-cpu 1x near   5m   $10 |      191       77 |      20       19 |      20        - |       36       92 |     3.3     3.3   124.3 |    100
hybrid          1x near   5m   $10 |      200       83 |      21       18 |      21        - |       17       92 |    26.6    26.6    22.2 |    100
viewport        1x near   5m  $100 |      500      236 |      20       74 |      20        - |       17      223 |    50.3    50.3    21.4 |    100
whole-chunk     1x near   5m  $100 |      334      281 |      20       17 |      20        - |      159       93 |     6.8     6.8    22.2 |    100
whole-chunk-cpu 1x near   5m  $100 |      201       72 |      20       20 |      20        - |       56       95 |     0.3     0.3   124.1 |    100
hybrid          1x near   5m  $100 |      202       95 |      21       19 |      20        - |       19       93 |    26.6    26.6    22.2 |    100
viewport        1x near  60m    $1 |      348       72 |      21       56 |      20        - |       18       90 |     1.3     1.3     0.4 |    100
whole-chunk     1x near  60m    $1 |      283      115 |      20       23 |      21        - |       39       89 |    12.7    12.7     3.2 |    100
whole-chunk-cpu 1x near  60m    $1 |      267       58 |      20       20 |      20        - |       36       91 |     9.9     9.9    20.3 |    100
hybrid          1x near  60m    $1 |      267       58 |      20       19 |      20        - |       20       76 |     4.6     4.6     3.2 |    100
viewport        1x near  60m   $10 |      369      112 |      20       61 |      20        - |       17       89 |     5.5     5.5     2.3 |    100
whole-chunk     1x near  60m   $10 |      288      107 |      20       20 |      20        - |       36       98 |     3.8     3.8     3.2 |    100
whole-chunk-cpu 1x near  60m   $10 |      275       51 |      20       19 |      20        - |       39      112 |     1.0     1.0    18.1 |    100
hybrid          1x near  60m   $10 |      262       53 |      20       18 |      20        - |       18      110 |     4.6     4.6     3.2 |    100
viewport        1x near  60m  $100 |      385      114 |      20       58 |      20        - |       20      114 |     5.6     5.6     2.3 |    100
whole-chunk     1x near  60m  $100 |      288      115 |      20       19 |      20        - |       41      114 |     3.0     3.0     3.2 |    100
whole-chunk-cpu 1x near  60m  $100 |      281       56 |      20       20 |      20        - |       40      118 |     0.1     0.1    17.9 |    100
hybrid          1x near  60m  $100 |      268       59 |      20       20 |      20        - |       18      114 |     4.6     4.6     3.2 |    100
viewport        1x deep   1m   $10 |      594      381 |      21      324 |      20      423 |       20      395 |    31.6    31.6    13.2 |     84
whole-chunk     1x deep   1m   $10 |      505      453 |      20       18 |      56      277 |      320      125 |   275.1   275.1   248.0 |     36
whole-chunk-cpu 1x deep   1m   $10 |      280      190 |      20       18 |      95      245 |      161      118 |   252.9   253.0   263.3 |     70
hybrid          1x deep   1m   $10 |      422      242 |      20       17 |      55      274 |       20      146 |   166.2   248.9   248.0 |     38
viewport        1x deep   1m  $100 |     1094      827 |      20      363 |      20      669 |       17      760 |   232.2   232.2   109.4 |     84
whole-chunk     1x deep   1m  $100 |      520      454 |      20       20 |      59      278 |      307      148 |    49.9    49.9   248.0 |     36
whole-chunk-cpu 1x deep   1m  $100 |      221      109 |      20       17 |      76      153 |      118       94 |    27.6    27.6   236.4 |     68
hybrid          1x deep   1m  $100 |      359      268 |      20       20 |      56      250 |       19      150 |   166.5   249.1   248.3 |     36
viewport        1x deep   5m   $10 |      724      249 |      21      236 |      20        - |       18      282 |    14.2    14.2     5.4 |    100
whole-chunk     1x deep   5m   $10 |      472      387 |      20       19 |      20        - |      237      107 |   133.7   133.7    69.6 |    100
whole-chunk-cpu 1x deep   5m   $10 |      291      168 |      20       17 |      20        - |      107       93 |   111.5   111.5   244.0 |    100
hybrid          1x deep   5m   $10 |      333      196 |      20       20 |      20        - |       18      111 |    74.0    74.0    69.6 |    100
viewport        1x deep   5m  $100 |      849      420 |      20      207 |      20        - |       20      406 |   104.4   104.4    45.4 |    100
whole-chunk     1x deep   5m  $100 |      462      397 |      20       18 |      20        - |      247       93 |    33.3    33.3    69.6 |    100
whole-chunk-cpu 1x deep   5m  $100 |      254       89 |      21       20 |      20        - |      107       85 |    11.2    11.2   237.1 |    100
hybrid          1x deep   5m  $100 |      326      208 |      20       19 |      20        - |       20      110 |    74.0    74.0    69.6 |    100
viewport        1x deep  60m   $10 |      117       54 |      20       56 |      20        - |       20      115 |     1.5     1.5     0.5 |    100
whole-chunk     1x deep  60m   $10 |      148      114 |      21       16 |      20        - |       38      115 |    37.5    37.5     8.2 |    100
whole-chunk-cpu 1x deep  60m   $10 |       92       55 |      21       18 |      20        - |       59       93 |    31.0    31.0    52.5 |    100
hybrid          1x deep  60m   $10 |       90       59 |      20       20 |      20        - |       20      113 |     9.6     9.6     8.2 |    100
viewport        1x deep  60m  $100 |      129       96 |      21       55 |      20        - |       18      105 |     9.0     9.0     3.8 |    100
whole-chunk     1x deep  60m  $100 |      153      113 |      20       20 |      20        - |       38      116 |     9.6     9.6     8.2 |    100
whole-chunk-cpu 1x deep  60m  $100 |       96       73 |      20       18 |      20        - |       56       94 |     3.1     3.1    45.5 |    100
hybrid          1x deep  60m  $100 |       91       56 |      20       19 |      20        - |       20       97 |     9.6     9.6     8.2 |    100
viewport        2x near   1m    $1 |      513      269 |      20      259 |      20        - |       19      267 |    42.1    42.1    15.3 |    100
whole-chunk     2x near   1m    $1 |      486      412 |      20       16 |      36        - |      311      106 |   100.3   100.3    79.3 |     53
whole-chunk-cpu 2x near   1m    $1 |      217      148 |      20       20 |      72        - |       78       95 |    94.8    94.8   259.0 |     85
hybrid          2x near   1m    $1 |      219      154 |      20       19 |      38        - |       18       73 |   102.6   102.6    79.3 |     53
viewport        2x near   1m   $10 |      776      545 |      20      172 |      20        - |       19      542 |   154.8   154.8    72.5 |    100
whole-chunk     2x near   1m   $10 |      485      412 |      20       17 |      38        - |      321      113 |    15.0    15.0    79.3 |     50
whole-chunk-cpu 2x near   1m   $10 |      193       75 |      20       17 |      57        - |       53       76 |     9.5     9.5   255.1 |     85
hybrid          2x near   1m   $10 |      255      132 |      20       18 |      39        - |       19       96 |   102.6   102.6    79.3 |     53
viewport        2x near   1m  $100 |      744      554 |      20      153 |      20        - |       17      495 |   154.8   154.8    72.5 |    100
whole-chunk     2x near   1m  $100 |      499      424 |      20       20 |      37        - |      327      135 |     6.4     6.4    79.3 |     53
whole-chunk-cpu 2x near   1m  $100 |      203       94 |      20       17 |      60        - |       56       75 |     1.0     1.0   254.8 |     85
hybrid          2x near   1m  $100 |      243      156 |      20       19 |      39        - |       19       92 |   102.6   102.6    79.3 |     53
viewport        2x near   5m    $1 |      404      134 |      20      131 |      20        - |       19      151 |    16.9    16.9     5.7 |    100
whole-chunk     2x near   5m    $1 |      332      277 |      20       20 |      20        - |      168       90 |    39.4    39.4    22.2 |    100
whole-chunk-cpu 2x near   5m    $1 |      239       93 |      20       19 |      20        - |       54       91 |    32.9    32.9   126.2 |    100
hybrid          2x near   5m    $1 |      222       90 |      20       19 |      20        - |       20       76 |    30.8    30.8    22.2 |    100
viewport        2x near   5m   $10 |      504      227 |      20      113 |      20        - |       19      261 |    52.3    52.3    21.4 |    100
whole-chunk     2x near   5m   $10 |      330      278 |      21       20 |      20        - |      169       98 |     9.8     9.8    22.2 |    100
whole-chunk-cpu 2x near   5m   $10 |      200      212 |      20       19 |      20        - |       38       92 |     3.3     3.3   124.3 |    100
hybrid          2x near   5m   $10 |      258      111 |      20       19 |      20        - |       17       93 |    30.8    30.8    22.2 |    100
viewport        2x near   5m  $100 |      519      216 |      20       96 |      20        - |       20      286 |    52.3    52.3    21.4 |    100
whole-chunk     2x near   5m  $100 |      353      304 |      20       20 |      20        - |      159       92 |     6.8     6.8    22.2 |    100
whole-chunk-cpu 2x near   5m  $100 |      206       75 |      20       17 |      20        - |       54       96 |     0.3     0.3   124.1 |    100
hybrid          2x near   5m  $100 |      222       91 |      20       17 |      20        - |       17       74 |    30.8    30.8    22.2 |    100
viewport        2x near  60m    $1 |      366       75 |      20       77 |      20        - |       17      126 |     2.3     2.3     0.7 |    100
whole-chunk     2x near  60m    $1 |      325      165 |      20       18 |      20        - |       37       92 |    12.7    12.7     3.2 |    100
whole-chunk-cpu 2x near  60m    $1 |      389       90 |      20       17 |      20        - |       40       88 |     9.9     9.9    20.3 |    100
hybrid          2x near  60m    $1 |      311       71 |      20       18 |      20        - |       17      113 |     5.7     5.7     3.2 |    100
viewport        2x near  60m   $10 |      397      115 |      21       52 |      20        - |       17      133 |     5.8     5.8     2.3 |    100
whole-chunk     2x near  60m   $10 |      283      108 |      20       18 |      21        - |       37      115 |     3.8     3.8     3.2 |    100
whole-chunk-cpu 2x near  60m   $10 |      261       81 |      20       18 |      20        - |       39      111 |     1.0     1.0    18.1 |    100
hybrid          2x near  60m   $10 |      283       56 |      20       20 |      21        - |       19      109 |     5.7     5.7     3.2 |    100
viewport        2x near  60m  $100 |      387      111 |      20       59 |      20        - |       19      116 |     5.8     5.8     2.3 |    100
whole-chunk     2x near  60m  $100 |      283      116 |      20       17 |      20        - |       37      115 |     3.0     3.0     3.2 |    100
whole-chunk-cpu 2x near  60m  $100 |      290       79 |      20       17 |      20        - |       38      109 |     0.1     0.1    17.9 |    100
hybrid          2x near  60m  $100 |      299       57 |      20       18 |      20        - |       18      116 |     5.7     5.7     3.2 |    100
viewport        2x deep   1m   $10 |     1282      904 |      20      705 |      20        - |       19      733 |   106.5   106.5    43.9 |    100
whole-chunk     2x deep   1m   $10 |      914      756 |      20       17 |      38        - |      619      137 |   276.1   276.1   254.1 |     60
whole-chunk-cpu 2x deep   1m   $10 |      412      297 |      20       17 |      77        - |      216      109 |   253.5   253.5   229.7 |     85
hybrid          2x deep   1m   $10 |      571      442 |      20       19 |      53        - |      134      129 |   226.1   268.7   254.1 |     60
viewport        2x deep   1m  $100 |     2251     1850 |      20      642 |      20        - |       17     1660 |   511.4   511.4   244.3 |    100
whole-chunk     2x deep   1m  $100 |      973      735 |      20       19 |      58        - |      610      160 |    56.4    56.4   254.4 |     60
whole-chunk-cpu 2x deep   1m  $100 |      353      182 |      20       20 |      74        - |      182      110 |    33.8    33.8   224.0 |     85
hybrid          2x deep   1m  $100 |      601      420 |      20       19 |      78        - |      130      151 |   226.4   265.6   254.4 |     60
viewport        2x deep   5m   $10 |      768      286 |      20      248 |      20        - |       19      287 |    27.9    27.9    10.5 |    100
whole-chunk     2x deep   5m   $10 |      530      357 |      20       19 |      20        - |      232      107 |   133.7   133.7    69.6 |    100
whole-chunk-cpu 2x deep   5m   $10 |      518      206 |      20       18 |      20        - |      109       95 |   111.5   111.5   262.8 |    100
hybrid          2x deep   5m   $10 |      402      202 |      20       18 |      20        - |       17      110 |    78.1    78.1    69.6 |    100
viewport        2x deep   5m  $100 |     1006      500 |      20      209 |      20        - |       18      492 |   138.0   138.0    59.2 |    100
whole-chunk     2x deep   5m  $100 |      484      365 |      20       16 |      20        - |      242      117 |    33.3    33.3    69.6 |    100
whole-chunk-cpu 2x deep   5m  $100 |      264       94 |      20       19 |      20        - |      111       91 |    11.2    11.2   239.5 |    100
hybrid          2x deep   5m  $100 |      353      198 |      20       17 |      20        - |       19      107 |    78.1    78.1    69.6 |    100
viewport        2x deep  60m   $10 |      134      108 |      20       58 |      20        - |       18       92 |     2.6     2.6     0.9 |    100
whole-chunk     2x deep  60m   $10 |      170      110 |      20       20 |      20        - |       38       94 |    37.5    37.5     8.2 |    100
whole-chunk-cpu 2x deep  60m   $10 |      110       73 |      20       19 |      20        - |       57      109 |    31.0    31.0    52.5 |    100
hybrid          2x deep  60m   $10 |      111       57 |      20       17 |      20        - |       19      113 |    10.6    10.6     8.2 |    100
viewport        2x deep  60m  $100 |      131       92 |      20       56 |      20        - |       19      111 |    11.8    11.8     5.0 |    100
whole-chunk     2x deep  60m  $100 |      152      113 |      20       20 |      20        - |       36       90 |     9.6     9.6     8.2 |    100
whole-chunk-cpu 2x deep  60m  $100 |       93       73 |      20       16 |      20        - |       37      109 |     3.1     3.1    45.5 |    100
hybrid          2x deep  60m  $100 |       91       58 |      20       17 |      20        - |       17      111 |    10.6    10.6     8.2 |    100
```
