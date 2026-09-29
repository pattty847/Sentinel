# GPU bin lab follow-up — 2026-09-29

Apple M4, Qt 6.11 Metal QRhi. The recording root was opened read-only; its newest file was live during these runs. Times are one-run measurements and the source count changes as recording continues.

## Summary

| Source | Entries | Valid minutes | Load ms | Decode CPU ms | Upload ms | Source GPU MB | MB / million entries | GPU p50 / p95 / max ms |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Real 24 h near | 6,088,849 | 1,364 | 264.8 | 45.5 | 7.0 | 53.7 | 8.82 | 0.042 / 0.643 / 0.784 |
| Real 24 h deep | 16,479,867 | 1,363 | 736.2 | 293.0 | 47.9 | 145.5 | 8.83 | 0.033 / 0.127 / 0.429 |
| Real requested 7 d near | 6,088,849 | 1,364 | 313.5 | 42.6 | 14.2 | 54.1 | 8.89 | 0.064 / 0.700 / 1.444 |
| Synthetic 10M | 10,000,000 | 1,000 | 183.6 | 0.0 | 17.4 | 78.4 | 7.84 | 0.033 / 0.719 / 0.854 |
| Synthetic 100M | 100,000,000 | 1,000 | 1782.1 | 0.0 | 265.0 | 783.3 | 7.83 | 0.542 / 2.907 / 16.465 |

The requested seven-day range contains only 1,364 valid one-minute columns (about 22.7 hours), the same entries as the 24-hour near run. It does not verify seven days of populated data. `decode_ms` is the sum of callback CPU times across four reader workers, not wall time.

The raw sparse stream is **6 bytes per entry**: two row/side words and two 16-bit HMC2 log codes in three 32-bit words. Source GPU bytes also include time and price LODs, coverage, durations, and offsets. The 100M synthetic case fit in GPU memory (783 MB of source buffers).

## First swapped frame

| Real source | Launch to first swapped frame | Load ms | Entries |
|---|---:|---:|---:|
| 24 h near | 507.3 ms | 320.8 | 6,098,115 |
| 24 h deep | 1,064.1 ms | 806.1 | 16,516,182 |

The `--first-paint` command listens for `QQuickWindow::frameSwapped` after the data-backed draw. These runs opened the lab window on this Mac; screenshot and interaction quality remain for orchestrator review.

## Per-zoom Metal GPU times

The sweep warms all 25 levels, measures 200 passes (eight per level), and submits one drain frame. Qt reports the last completed GPU frame; each timestamp is assigned to that preceding pass. The first 20 levels zoom time and price together. The final five retain the full source range while shrinking output width to exercise multiple source minutes per output column. All times below are GPU timestamps.

### Real 24 h deep

| Zoom | Visible entries | Output | Native rows/bin | Source cols/output | GPU p50 | GPU p95 | GPU max |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1.00× | 16,479,867 | 1280×318 | 100 | 1.12 | 0.131 | 0.165 | 0.165 |
| 1.15× | 13,748,265 | 1253×276 | 100 | 1.00 | 0.073 | 0.429 | 0.429 |
| 1.32× | 10,293,571 | 1091×242 | 100 | 1.00 | 0.054 | 0.068 | 0.068 |
| 1.52× | 7,439,541 | 950×210 | 100 | 1.00 | 0.043 | 0.048 | 0.048 |
| 1.74× | 5,268,528 | 827×182 | 100 | 1.00 | 0.024 | 0.048 | 0.048 |
| 2.00× | 3,758,708 | 720×318 | 50 | 1.00 | 0.044 | 0.057 | 0.057 |
| 2.30× | 2,697,349 | 626×276 | 50 | 1.00 | 0.028 | 0.036 | 0.036 |
| 2.64× | 1,898,188 | 545×242 | 50 | 1.00 | 0.019 | 0.022 | 0.022 |
| 3.03× | 1,296,671 | 475×210 | 50 | 1.00 | 0.016 | 0.021 | 0.021 |
| 3.48× | 861,797 | 413×183 | 50 | 1.00 | 0.018 | 0.026 | 0.026 |
| 4.00× | 612,703 | 360×318 | 25 | 1.00 | 0.109 | 0.146 | 0.146 |
| 4.59× | 443,467 | 313×346 | 20 | 1.00 | 0.056 | 0.066 | 0.066 |
| 5.28× | 319,585 | 272×301 | 20 | 1.00 | 0.042 | 0.054 | 0.054 |
| 6.06× | 238,093 | 237×262 | 20 | 1.00 | 0.032 | 0.033 | 0.033 |
| 6.96× | 183,325 | 206×228 | 20 | 1.00 | 0.028 | 0.044 | 0.044 |
| 8.00× | 140,696 | 180×199 | 20 | 1.00 | 0.020 | 0.028 | 0.028 |
| 9.19× | 105,766 | 156×346 | 10 | 1.00 | 0.024 | 0.041 | 0.041 |
| 10.56× | 79,555 | 136×301 | 10 | 1.00 | 0.021 | 0.109 | 0.109 |
| 12.13× | 60,538 | 118×262 | 10 | 1.01 | 0.017 | 0.020 | 0.020 |
| 13.93× | 46,428 | 103×228 | 10 | 1.00 | 0.013 | 0.018 | 0.018 |
| 1.00× | 16,479,867 | 640×318 | 100 | 2.25 | 0.076 | 0.108 | 0.108 |
| 1.00× | 16,479,867 | 320×318 | 100 | 4.50 | 0.058 | 0.068 | 0.068 |
| 1.00× | 16,479,867 | 160×318 | 100 | 9.00 | 0.042 | 0.052 | 0.052 |
| 1.00× | 16,479,867 | 80×318 | 100 | 18.00 | 0.033 | 0.039 | 0.039 |
| 1.00× | 16,479,867 | 40×318 | 100 | 36.00 | 0.016 | 0.017 | 0.017 |

### Synthetic 10M

| Zoom | Visible entries | Output | Native rows/bin | Source cols/output | GPU p50 | GPU p95 | GPU max |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1.00× | 10,000,000 | 1000×201 | 50 | 1.00 | 0.031 | 0.038 | 0.038 |
| 1.15× | 7,621,250 | 870×350 | 25 | 1.00 | 0.831 | 0.854 | 0.854 |
| 1.32× | 5,753,200 | 757×304 | 25 | 1.00 | 0.670 | 0.738 | 0.738 |
| 1.52× | 4,349,400 | 659×330 | 20 | 1.00 | 0.526 | 0.542 | 0.542 |
| 1.74× | 3,312,000 | 574×288 | 20 | 1.00 | 0.441 | 0.562 | 0.562 |
| 2.00× | 2,510,000 | 500×251 | 20 | 1.00 | 0.361 | 0.519 | 0.519 |
| 2.30× | 1,896,600 | 435×218 | 20 | 1.00 | 0.306 | 0.323 | 0.323 |
| 2.64× | 1,440,200 | 378×190 | 20 | 1.00 | 0.220 | 0.308 | 0.308 |
| 3.03× | 1,085,700 | 329×330 | 10 | 1.00 | 0.187 | 0.243 | 0.243 |
| 3.48× | 826,560 | 287×288 | 10 | 1.00 | 0.086 | 0.203 | 0.203 |
| 4.00× | 627,500 | 250×251 | 10 | 1.00 | 0.066 | 0.073 | 0.073 |
| 4.59× | 473,060 | 217×218 | 10 | 1.00 | 0.052 | 0.061 | 0.061 |
| 5.28× | 359,100 | 189×190 | 10 | 1.00 | 0.033 | 0.038 | 0.038 |
| 6.06× | 273,075 | 164×331 | 5 | 1.01 | 0.028 | 0.054 | 0.054 |
| 6.96× | 205,920 | 143×288 | 5 | 1.00 | 0.023 | 0.029 | 0.029 |
| 8.00× | 156,875 | 125×251 | 5 | 1.00 | 0.019 | 0.025 | 0.025 |
| 9.19× | 118,810 | 108×218 | 5 | 1.01 | 0.020 | 0.024 | 0.024 |
| 10.56× | 90,250 | 94×190 | 5 | 1.01 | 0.018 | 0.024 | 0.024 |
| 12.13× | 68,890 | 82×166 | 5 | 1.01 | 0.013 | 0.023 | 0.023 |
| 13.93× | 51,120 | 71×360 | 2 | 1.01 | 0.012 | 0.017 | 0.017 |
| 1.00× | 10,000,000 | 640×201 | 50 | 1.56 | 0.030 | 0.033 | 0.033 |
| 1.00× | 10,000,000 | 320×201 | 50 | 3.12 | 0.024 | 0.031 | 0.031 |
| 1.00× | 10,000,000 | 160×201 | 50 | 6.25 | 0.023 | 0.035 | 0.035 |
| 1.00× | 10,000,000 | 80×201 | 50 | 12.50 | 0.025 | 0.040 | 0.040 |
| 1.00× | 10,000,000 | 40×201 | 50 | 25.00 | 0.014 | 0.023 | 0.023 |

## Implementation and limits

One GPU invocation gathers each output cell. Raw boundaries use binary searches over minute-offset columns; aligned eight-minute interiors use duration-weighted sparse sums. A coarse column is valid only where all eight source minutes cover the displayed row range. Sixteen-row sparse price sums handle other wide rows, while common 50-row and 100-row display ticks use direct aligned bid/ask sums. Source quantities are decoded with the HMC2 log-size formula; bid and ask accumulation stays separate. Float gather sums avoid fixed-point overflow for a 116,000 BTC wall repeated over 1,440 columns; the shader uses no float atomics.

The raw-versus-LOD unit tests compare aligned and unaligned time/price bins and a missing minute; a dense-price test compares precomputed sums with raw minute bins. These are CPU reference tests. A GPU readback comparison and screenshot inspection have not been done. Metal timing is sensitive to other GPU work on the Mac; the listed p95/max values are measured, not guaranteed bounds.

Both requested acceptance datasets are below 2 ms at p95 and max in the reported run. The lab remains isolated; production server paging and the fixed band were not changed.
