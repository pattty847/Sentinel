# GPU bin lab, absolute bins and selected timeframes — 2026-09-29

Apple M4, Qt 6.11 Metal QRhi. The recording directory was read only; a live recorder changed real entry counts between runs. The sweep uses 25 zoom levels, eight timed passes per level (200 total) at 1920×1080 and 3840×2160 render areas. Times are QRhi finished-frame GPU timestamps, attributed to the preceding frame. The output grid includes two guard bins per edge.

## Results

| Source and selected timeframe | Entries | Source resolution | Load ms | Upload ms | Source MB / million entries | 1x p50 / p95 / max ms | 2x p50 / p95 / max ms |
|---|---:|---:|---:|---:|---:|---:|---:|
| Real 24 h deep · 1m | 18,892,999 | 1m | 331.223 | 15.115 | 4.003 | 0.181 / 2.220 / 16.895 | 0.202 / 2.380 / 19.426 |
| Real 24 h near · 1m | 6,410,513 | 1m | 203.736 | 3.851 | 4.009 | 0.121 / 1.132 / 17.107 | 0.193 / 1.649 / 2.167 |
| Synthetic 10M · 1m | 10,000,000 | 1m | 59.055 | 7.380 | 4.004 | 0.080 / 1.322 / 1.985 | 0.130 / 1.349 / 1.987 |
| Real 24 h deep · 5m | 3,808,690 | 5m | 1520.050 | 2.991 | 8.019 | 0.036 / 0.713 / 1.347 | 0.042 / 0.908 / 1.029 |
| Real 24 h deep · 16m | 1,201,410 | 16m | 1004.303 | 1.379 | 8.048 | 0.027 / 0.272 / 0.869 | 0.033 / 0.315 / 0.960 |
| Real 24 h deep · 1h | 323,798 | 60m | 11.389 | 0.371 | 8.134 | 0.020 / 0.201 / 0.303 | 0.021 / 0.179 / 0.500 |
| Real 24 h deep · 4h | 100,804 | 240m | 66.926 | 0.198 | 8.343 | 0.025 / 0.212 / 0.451 | 0.017 / 0.121 / 0.159 |
| Real 24 h deep · 1D | 33,556 | 1440m | 92.635 | 0.183 | 8.765 | 0.023 / 0.155 / 0.256 | 0.021 / 0.157 / 0.291 |

The 10M-entry sweep stays below 2 ms p95 and max at both render sizes. Explicit 5m, 16m, 1h, 4h, and 1D deep columns also stay below 1.4 ms max in these runs. The raw 1m deep sweep has 2.22/2.38 ms p95 at 1x/2x and isolated 16.89/19.43 ms maxima. A repeat 24 h deep run yielded 2.21/2.39 ms p95 and 31.03/9.06 ms maxima at 1x/2x. Near 1m also has one isolated 17.11 ms 1x sample despite a 1.13 ms p95. The requested approximately 4 ms maximum is therefore **not met for raw 1m** in this finished-frame benchmark. The outliers occur at several zoom levels and are reported without assigning a cause.

The raw minute stream uses four bytes per entry when the relative row fits 16 bits: 16 row bits, one side bit at the top of the row field, and 15 HMC2 size-code bits. Wider rows use six bytes per entry; a Metal readback test exercises that fallback. Explicit composed columns use the same row/code packing plus four bytes of per-entry covered duration. Source MB per million includes offsets, row coverage, scale, and duration metadata. The full-view 1m deep source occupied 75.630 MB of GPU storage. Output cells cost 16 bytes each and are disposable.

## Semantics and interaction

Each output column is one selected UTC-epoch-aligned timeframe. A 1m view reads raw minute entries. 5m, 15m, and custom sub-hour columns (including 16m) are composed once from minutes, per native grid and per native row/side covered duration. Deep 1h uses persisted schema-4 hour rollups, with an unpersisted tail composed from minute records; 4h and 1D are explicitly composed from those hours. Near and non-hour custom selections use minute records. Every price view runs one gather pass over the selected sparse entries at the ladder tick or a compatible manual tick. There are no pre-summed price bins or hidden eight-minute time LOD.

Time and price bins are anchored to absolute UTC and price-ladder origins. The compute grid has guard bins; fractional pans update only a fragment mapping until the viewport leaves the guard. Manual zoom stops at the configured minimum column width (default 1 px), including after a resize. Auto zoom steps 1m, 5m, 15m, 1h, 4h, 1D. Dragging upward applies the requested vertical sign change. The source upload remains resident on resize. A timeframe change composes the recent visible hours first on a worker, paints them, then composes older history and swaps source buffers in bounded upload slices.

The gather was retained because float atomics are not portable to Metal and fixed-point atomics require a range-dependent scale that can overflow on a 116,000 BTC wall integrated over 1,440 minutes. Bid/ask sums and each native grid's covered-duration normalization are deterministic floats in the shader. Explicit timeframe composition uses double or long-double CPU accumulation and encodes once; the accepted difference from server aggregation is at most one log-code step.

## GPU readback parity

The Metal test compared synthetic HMC2 with nonzero base rows, missing minutes, partial side coverage, mixed $5/$10 grids, changing size scales, hour coverage runs, and a minute tail, plus the real deep recording. Views span 1m, 5m, 16m, 15m, 1h, 4h, and 1D with several native-row groups. Raw minute, persisted hour, and minute-tail paths had maximum log-code deviation **0**. Explicit selected 5m, 16m, 4h, and 1D columns had maximum deviation **1**. Every case had **zero dominant-side and validity/veil mismatches**. The test skips the GPU part with a clear message when no Metal device is available; the real part skips when the T7 recording is absent.

A headless offscreen Metal screenshot was rendered at `--screenshot /private/tmp/sentinel-lab-round4-final-5m.png --hours 24 --layer deep --tf 5`. The PNG shows the deep-book heatmap. The interactive QQuick window and mouse direction could not be driven from this sandbox; the orchestrator should check them. The 100M stress test was not run.

## Per-zoom raw-entry sweep

Each source/output column below is one minute; no time aggregation occurs at zoom out in manual mode. Per-level p95 equals that level's maximum because each level has eight samples. Columns × rows include guard bins.

### Real 24 h deep

| Zoom | Visible entries 1x | 1x grid | Native rows/bin 1x | 1x GPU p50/max ms | 2x grid | Native rows/bin 2x | 2x GPU p50/max ms |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1.000 | 18,892,999 | 1444×322 | 200 | 2.362 / 16.895 | 1444×638 | 100 | 2.674 / 6.042 |
| 1.149 | 14,932,160 | 1258×280 | 200 | 2.187 / 15.391 | 1258×556 | 100 | 2.329 / 2.390 |
| 1.320 | 10,780,740 | 1096×486 | 100 | 1.614 / 2.088 | 1096×966 | 50 | 1.837 / 2.352 |
| 1.516 | 7,534,866 | 956×423 | 100 | 1.154 / 2.258 | 956×1050 | 40 | 1.610 / 19.426 |
| 1.741 | 5,386,254 | 832×368 | 100 | 0.972 / 1.230 | 832×914 | 40 | 1.089 / 1.181 |
| 2.000 | 3,851,672 | 724×322 | 100 | 0.713 / 1.021 | 724×797 | 40 | 0.838 / 1.187 |
| 2.297 | 2,778,570 | 632×280 | 100 | 0.526 / 0.637 | 632×694 | 40 | 0.606 / 0.954 |
| 2.639 | 1,915,837 | 550×485 | 50 | 0.526 / 0.918 | 550×605 | 40 | 0.486 / 0.625 |
| 3.031 | 1,325,415 | 480×528 | 40 | 0.459 / 0.799 | 480×1050 | 20 | 0.494 / 0.732 |
| 3.482 | 885,846 | 418×460 | 40 | 0.247 / 0.671 | 418×915 | 20 | 0.360 / 0.499 |
| 4.000 | 626,771 | 364×401 | 40 | 0.185 / 0.807 | 364×797 | 20 | 0.272 / 0.412 |
| 4.595 | 456,952 | 318×350 | 40 | 0.065 / 0.077 | 318×694 | 20 | 0.185 / 0.229 |
| 5.278 | 332,343 | 278×305 | 40 | 0.059 / 0.228 | 278×605 | 20 | 0.112 / 0.311 |
| 6.063 | 244,535 | 242×528 | 20 | 0.062 / 0.342 | 242×1050 | 10 | 0.199 / 0.216 |
| 6.964 | 188,992 | 212×460 | 20 | 0.047 / 0.556 | 212×915 | 10 | 0.134 / 0.159 |
| 8.000 | 144,526 | 184×401 | 20 | 0.038 / 0.634 | 184×797 | 10 | 0.060 / 0.277 |
| 9.190 | 110,160 | 162×350 | 20 | 0.030 / 0.300 | 162×695 | 10 | 0.046 / 0.050 |
| 10.556 | 84,774 | 142×305 | 20 | 0.023 / 0.036 | 142×605 | 10 | 0.036 / 0.271 |
| 12.126 | 63,240 | 124×527 | 10 | 0.030 / 0.197 | 124×1050 | 5 | 0.052 / 0.251 |
| 13.929 | 49,140 | 108×459 | 10 | 0.025 / 0.198 | 108×914 | 5 | 0.039 / 0.112 |
| 16.000 | 36,190 | 94×401 | 10 | 0.062 / 0.280 | 94×995 | 4 | 0.036 / 0.184 |
| 18.379 | 27,048 | 84×349 | 10 | 0.018 / 0.231 | 84×867 | 4 | 0.028 / 0.241 |
| 21.112 | 21,460 | 74×305 | 10 | 0.013 / 0.112 | 74×756 | 4 | 0.026 / 0.133 |
| 24.251 | 16,448 | 64×528 | 5 | 0.049 / 0.199 | 64×658 | 4 | 0.019 / 0.192 |
| 27.858 | 12,712 | 56×460 | 5 | 0.017 / 0.223 | 56×574 | 4 | 0.016 / 0.096 |

### Synthetic 10M

| Zoom | Visible entries 1x | 1x grid | Native rows/bin 1x | 1x GPU p50/max ms | 2x grid | Native rows/bin 2x | 2x GPU p50/max ms |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1.000 | 10,000,000 | 1004×505 | 20 | 1.449 / 1.985 | 1004×1005 | 10 | 1.617 / 1.987 |
| 1.149 | 7,708,800 | 876×440 | 20 | 1.139 / 1.453 | 876×876 | 10 | 1.266 / 1.704 |
| 1.320 | 5,852,160 | 762×384 | 20 | 0.862 / 1.510 | 762×762 | 10 | 0.916 / 1.530 |
| 1.516 | 4,435,520 | 664×334 | 20 | 0.860 / 1.799 | 664×664 | 10 | 0.695 / 0.899 |
| 1.741 | 3,387,200 | 580×292 | 20 | 0.550 / 0.926 | 580×580 | 10 | 0.547 / 1.053 |
| 2.000 | 2,545,200 | 504×505 | 10 | 0.449 / 0.959 | 504×1005 | 5 | 0.548 / 1.012 |
| 2.297 | 1,936,000 | 440×440 | 10 | 0.318 / 0.557 | 440×876 | 5 | 0.432 / 0.690 |
| 2.639 | 1,474,560 | 384×384 | 10 | 0.260 / 0.381 | 384×763 | 5 | 0.317 / 0.702 |
| 3.031 | 1,115,560 | 334×334 | 10 | 0.229 / 0.414 | 334×664 | 5 | 0.242 / 0.804 |
| 3.482 | 852,640 | 292×292 | 10 | 0.072 / 0.426 | 292×580 | 5 | 0.178 / 0.436 |
| 4.000 | 641,350 | 254×505 | 5 | 0.105 / 0.270 | 254×505 | 5 | 0.077 / 0.292 |
| 4.595 | 488,400 | 222×440 | 5 | 0.060 / 0.251 | 222×440 | 5 | 0.055 / 0.338 |
| 5.278 | 372,480 | 194×384 | 5 | 0.042 / 0.050 | 194×952 | 2 | 0.130 / 0.419 |
| 6.063 | 284,750 | 170×335 | 5 | 0.036 / 0.290 | 170×830 | 2 | 0.058 / 0.803 |
| 6.964 | 216,080 | 148×292 | 5 | 0.043 / 0.479 | 148×723 | 2 | 0.046 / 0.055 |
| 8.000 | 165,750 | 130×255 | 5 | 0.033 / 0.224 | 130×630 | 2 | 0.036 / 0.273 |
| 9.190 | 126,540 | 114×222 | 5 | 0.018 / 0.230 | 114×549 | 2 | 0.030 / 0.524 |
| 10.556 | 95,800 | 100×479 | 2 | 0.021 / 0.251 | 100×953 | 1 | 0.038 / 0.266 |
| 12.126 | 73,392 | 88×417 | 2 | 0.020 / 0.110 | 88×829 | 1 | 0.030 / 0.292 |
| 13.929 | 55,328 | 76×364 | 2 | 0.017 / 0.105 | 76×723 | 1 | 0.024 / 0.194 |
| 16.000 | 43,112 | 68×317 | 2 | 0.016 / 0.024 | 68×629 | 1 | 0.023 / 0.118 |
| 18.379 | 33,240 | 60×277 | 2 | 0.011 / 0.078 | 60×549 | 1 | 0.015 / 0.131 |
| 21.112 | 24,908 | 52×479 | 1 | 0.019 / 0.158 | 52×479 | 1 | 0.012 / 0.021 |
| 24.251 | 19,182 | 46×417 | 1 | 0.023 / 0.039 | 46×417 | 1 | 0.012 / 0.097 |
| 27.858 | 14,520 | 40×363 | 1 | 0.010 / 0.082 | 40×363 | 1 | 0.007 / 0.126 |
