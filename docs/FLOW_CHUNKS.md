# Flow chunks

**Draft spec, not implemented.** Nothing here describes running code. Flow history today (FLD1 day
files, FHE1 pages, `flow_bar` live messages) is in [MARKETDATA.md](MARKETDATA.md) "Live flow bars" and
"Flow history". This spec replaces FLD1 and FHE1 and puts flow on heatmap-style chunks. The server
(plan step 3) and the client (step 5) implement it; step 4 moves the heatmap onto the same envelope.
Measurements are in section 12; they come from `flow_history_bench --lateness` and `--columnar`
(`tests/core/flow_history_bench.cpp`, prototype codec `tests/core/flow_chunk_proto.hpp`).

Decided rules this spec makes exact:
- RAWL2 is the only truth. Every flow file is a rebuildable index of it.
- A sealed bar is a deterministic function of RAWL2. 1h and 1d bars are composed from sealed 1m bars only.
- History serves only sealed chunks. The live stream owns the live bar and every closing bar.
- Sub-minute flow is never written to disk.

## 1. Units

One unit per quantity. Every price is in **price units**; rows are used only for row offsets.

| Quantity | Unit | Stored as | Source |
| --- | --- | --- | --- |
| Price, open, high, low, close | price units = price x `price_scale` | i64 | `price_scale` is at least 1 / quote increment (`roller/Grid.cpp:53`), so every trade price is an integer; conversion `recording::priceUnits` (`servermodel/RecordingCodec.hpp:89`) |
| Row | `floorDiv(price units, row_tick_units)` on the day's recorder grid | not stored | `flow/FlowBarBuilder.cpp:142` |
| Row offset | row - low row of its bar | i16, 0..32767 | `flow/FlowBar.hpp:21` |
| Low row | `floorDiv(low, row_tick_units)` | derived | holds for every FLD1 bar (section 12) |
| Base volume | size x `base_scale` (10^decimals of `base_increment`) | i64 | `flow/FlowScales.cpp:8-27` |
| Quote volume | price x size x `quote_scale` (10^8), rounded once per trade | i64 | `flow/FlowBarBuilder.cpp:95-116` |
| Trade count | trades, unknown side included | u32 | |
| Time | ms since the Unix epoch, UTC | i64 | |

OHLC on the wire are price-unit deltas (section 4), never row offsets. Rows map to price only through the
bar's own grid: price of row `r` = `r x row_tick_units / price_scale`.

## 2. Chunk key

`(instrument_id, channel, level_ms, price_step, start_ms)`

| Field | Form | Rule |
| --- | --- | --- |
| `instrument_id` | `<venue>:<symbol>`, ASCII, at most 72 bytes | venue `coinbase` or `kalshi` (`InstrumentSpec.hpp:15-21`); symbol as the venue spells it, checked by `validateInstrumentId` (`InstrumentSpec.hpp:23-32`). Example `coinbase:BTC-USD`. Required in every new file, key and cache. |
| `channel` | string, at most 32 bytes | `flow.summary`, `flow.rows`, `trades.big`. Step 4 adds `book.near`, `book.deep` for the heatmap. |
| `level_ms` | i64 | 60000, 3600000 or 86400000 |
| `price_step` | u32 | native rows per stored row. 1 = native grid. 0 for channels without rows (`flow.summary`, `trades.big`). v1 accepts only these two values; coarser rows (2, 5, ...) can be added later without changing any other key. |
| `start_ms` | i64 | chunk start, `heatmap::chunkStartOf(level, t)` (`heatmap/ChunkCodec.cpp:238-242`) |

| Level | Bars per chunk | Span | Alignment |
| --- | --- | --- | --- |
| 1m | 60 | 1 UTC hour | Unix-epoch hour |
| 1h | 24 | 1 UTC day | UTC midnight |
| 1d | 16 | 16 UTC days | whole 16-day spans after 2000-01-01 (`kDayChunkDays`, `heatmap/ChunkCodec.hpp:30`) |

Spans and alignment are the heatmap's, so one key arithmetic serves both. `trades.big` uses the same three
levels and spans; its level only selects the span (events are not bars, section 4).

## 3. Envelope

The binary envelope stays SHE1 exactly as today: `SHE1`, u16 envelope version 1, u64 request id, then
the inner frame (`heatmap/ChunkCodec.cpp:458-464`). The decoder dispatches on the inner magic: `SHC1`
(heatmap, wire v3, until step 4) or `SCK1` (this spec). All integers little-endian; `str8` is a u8 length
then bytes.

`SCK1` frame:

| # | Field | Type | Notes |
| --- | --- | --- | --- |
| 1 | magic | 4 bytes `SCK1` | |
| 2 | wire version | u16 = 1 | |
| 3 | kind | u8 | 1 chunk, 2 live, 3 not_modified, 4 error |
| 4 | state | u8 | 0 open, 1 sealed |
| 5 | `instrument_id` | str8 | |
| 6 | `channel` | str8 | |
| 7 | `level_ms` | i64 | |
| 8 | `price_step` | u32 | |
| 9 | `start_ms` | i64 | |
| 10 | `end_ms` | i64 | exclusive extent; `start + span` for kind 1 |
| 11 | `committed_through_ms` | i64 | equals `end_ms` when sealed |
| 12 | `revision` | u64 | 0 when sealed |
| 13 | `generation` | u32 | section 7 |
| 14 | `content_hash` | u64 | FNV-1a 64 over fields 1-13 then the raw payload (the SHC1 rule, `heatmap/ChunkCodec.cpp:58-66`) |
| 15 | `payload_version` | u16 | per channel; 1 for every channel here |
| 16 | codec | u8 | 0 raw, 1 zstd |
| 17 | raw length | u32 | at most 16 MiB |
| 18 | stored length | u32 | equals the remaining frame bytes |
| 19 | payload | bytes | section 4 |

- `not_modified` (3): fields 1-14, no payload.
- `error` (4): fields 1-9, u16 code, str8 message (at most 255 bytes). Codes as SHC1
  (`heatmap/ChunkCodec.hpp:48-53`): 1 invalid_request, 2 unavailable, 3 busy, 4 build_failed.
- `live` (2): open, never cached; payload as kind 1 plus the live trailer (section 8).
- Limits and decoder checks as SHC1: frame and raw payload at most 16 MiB; zstd frame content size checked
  before allocation; every count bounded by the remaining bytes before allocation.
- Codec: zstd level 1 when it saves at least 10%, otherwise raw. On the measured data zstd 1 saves 10-34% on
  `flow.rows` chunks and level 3 at most 1% more; `flow.summary` saves 5-8% for BTC, ETH and SOL (sent raw) and
  21% for PEPE (section 12).
  The server caches the finished frame.

Request (JSON text frame):
`{"type":"chunk_request","req":<u64>,"instrument":"coinbase:BTC-USD","channel":"flow.rows","level_ms":60000,"price_step":1,"starts":[<i64>...],"have_hash":["<16 hex>"|""...]}`.
1..64 starts; one binary reply per start (chunk, not_modified or error), as `heatmap_chunk_request`
(MARKETDATA.md "Heatmap chunk wire"). Budgets, busy errors and the history pool are the heatmap's.

Availability (JSON, pushed on subscribe and on change):
`{"type":"chunk_availability","instrument":..,"wire_version":1,"channels":{"flow.rows":{"levels":[{"level_ms":60000,"chunk_span_ms":3600000,"oldest_ms":..,"sealed_through_ms":..}]}},"generations":[[<day_ms>,<gen>]...]}`.
`generations` lists only days whose generation is not 1 (rebuilds are rare). `sealed_through_ms` is
section 7's T.

What changes, and when:

| SHC1 v3 field (`heatmap/ChunkCodec.cpp:326-336`) | SCK1 | Flow (step 3) | Heatmap (step 4) |
| --- | --- | --- | --- |
| `SHC1`, u16 version 3 | `SCK1`, u16 version 1 | uses SCK1 | moves to SCK1; SHC1 and its decoder are deleted |
| u8 kind, u8 sealed | same | same | same |
| str8 symbol | str8 `instrument_id` | `coinbase:BTC-USD` | the same id replaces the bare symbol |
| str8 source (`hmc2.near`) | str8 channel | `flow.*` | `book.near`, `book.deep`; the `hmc2.*` ids are retired |
| - | u32 `price_step` | 0 or 1 | 1 |
| i64 level, start, end | same | same | same |
| i64 `tailStartMs` (live kind only) | in the live payload | - | moves into the heatmap live payload |
| grid identity and `SizeScale` (40 bytes) | in the payload | flow grid table | heatmap payload header |
| i64 committed through, u64 revision | same | same | same |
| - | u32 generation | per-day generation | per-day generation (RollupStore fingerprints feed it) |
| u64 FNV-1a content hash | same rule | same | same |
| u32 column and entry counts | in the payload | `n` and row counts | heatmap payload header |
| zstd level 3 always | u16 payload version, u8 codec | zstd 1 or raw | zstd 3, unchanged payload bytes |
| `heatmap_chunk_request`, `heatmap_availability` | `chunk_request`, `chunk_availability` | new messages | the heatmap messages are deleted |

Step 4 changes no heatmap payload byte; heatmap byte-parity tests compare the decoded `SparseColumns`
before and after the move.

## 4. Channels and payloads

### flow.summary and flow.rows

`flow.rows` = summary section + rows section. `flow.summary` = the summary section alone; it is a byte
prefix of the `flow.rows` payload, so the server cuts it without re-encoding, and a client holding a
`flow.rows` chunk also holds its summary. Both channels share one key except `price_step` (0 and 1).

Payload header:

| Field | Type | Meaning |
| --- | --- | --- |
| `grid_count` | u8 | 0 only when the chunk has no bars; 1 for 1m and 1h chunks (one UTC day has one grid, `flow/FlowCodec.cpp:165-167`); 1..16 for 1d chunks |
| grids | `grid_count` x 32 bytes | i64 `row_tick_units`, i64 `price_scale`, i64 `base_scale`, i64 `quote_scale` |
| `run_count` | u8 | coverage runs |
| runs | `run_count` x 5 bytes | u16 from slot, u16 to slot (exclusive), u8 status: 1 complete, 2 partial, 3 unrecorded, 4 building. Runs are ascending and cover `[0, slots)` exactly. Only live frames (kind 2) may hold `building`. |

Summary section, field-major (every value of one field, then the next field). `n` = bars with trades.

| Field | Count | Encoding | Meaning |
| --- | --- | --- | --- |
| `n` | 1 | varint | present bars, at most the span's slots |
| slot gap | n | varint | slot - previous slot - 1 (previous starts at -1); bar start = chunk start + slot x level |
| grid index | n, only if `grid_count` > 1 | u8 | index into grids |
| open | n | zigzag varint | open - previous bar's close (the first bar: open - 0) |
| up | n | varint | high - open |
| down | n | varint | open - low |
| close | n | varint | close - low |
| base volume | n | varint | all trades |
| quote volume | n | varint | all trades |
| buy volume | n | varint | aggressor buys |
| sell volume | n | varint | aggressor sells |
| count | n | varint | all trades, unknown side included |

Rows section (`flow.rows` only):

| Field | Count | Encoding | Meaning |
| --- | --- | --- | --- |
| row count | n | varint | rows of each bar, at most 32768 |
| offset gap | rows | varint | offset - previous offset - 1 within the bar (the first row: its offset, always 0) |
| bid volume | rows | varint | sell-aggressor base volume at the row |
| ask volume | rows | varint | buy-aggressor base volume |
| bid count | rows | varint | |
| ask count | rows | varint | |

Derived on read, never stored: low row = `floorDiv(low, row_tick_units)`; delta = buy - sell; unknown-side
volume = base - buy - sell; maximum row volume; VWAP = quote / base. A row with zero volumes exists when only
unknown-side trades printed there (`flow/FlowBarBuilder.cpp:189-195`); it is kept.

The decoder rejects: slots outside the span or not ascending; `high < close`; `buy + sell > base`; per-bar
row volume sums not equal to sell (bid) and buy (ask); known row counts above `count`; offsets not ascending
or above 32767; first offset not 0; last offset not `floorDiv(high, row_tick_units) - low row`; a grid
index out of range; trailing bytes. These are the FLB1 checks (`flow/FlowCodec.cpp:56-97`) plus the
derivation rules.

Bars with trades only. A slot with coverage `complete` and no bar is an observed empty minute; `unrecorded`
has no bar and means no recording.

**Grid table in 1d chunks.** Each 1d bar is one UTC day and uses that day's grid through its grid index.
Readers map rows to price with the bar's grid, never one chunk-wide grid. A 1h bar composed across a grid
change cannot exist: the grid changes only at UTC midnight.

### trades.big

One chunk = the big-trade events of its span: one UTC hour (level 60000), one UTC day (3600000) or 16 days
(86400000); `price_step` 0. A coarser chunk is the concatenation of its finer chunks (same floor, same events),
so a far zoom fetches one chunk per day or per 16 days instead of one per hour. Measured size at the proposed
floors: about 9 KB per BTC day (estimate, section 12). An event is one trade, or
a sweep: consecutive trades in feed order with consecutive numeric trade ids, the same exchange millisecond
and the same side, collapsed as notables are today (`flow/FlowBarBuilder.cpp:202-225`). An event is kept when
its notional is at or above the instrument's floor (`big_floor_quote`, quote units, server config). The floor
is written in each chunk; a floor change is a rebuild with a new generation. Proposed floors, about 50-500
events a day each (section 12; the owner decides, open item 2):

| Instrument | Floor | Events per day (lower bound, 53-54 days) |
| --- | --- | --- |
| BTC-USD | $100,000 | 500 |
| ETH-USD | $50,000 | 344 |
| SOL-USD | $25,000 | 287 |
| PEPE-USD | $10,000 | 51 |

The client may draw a higher floor than the chunk's; it never needs a lower one from history.

| Field | Count | Encoding | Meaning |
| --- | --- | --- | --- |
| grid | 1 | 32 bytes | as above |
| floor | 1 | varint | `big_floor_quote` used |
| `n` | 1 | varint | events, ascending by (exchange ms, first trade id) |
| time | n | varint | exchange ms - previous event's ms (the first: - chunk start) |
| side | n | u8 | 0 unknown, 1 buy, 2 sell |
| price | n | zigzag varint | first trade's price units - previous event's (the first: - 0) |
| low | n | varint | price - lowest price of the sweep |
| high | n | varint | highest price of the sweep - price |
| size | n | varint | base units |
| notional | n | varint | quote units |
| trades | n | varint | trades collapsed into the event |
| levels | n | varint | distinct prices |

Trade ids are not stored: the live tail and sealed chunks meet at "sealed through T" by exchange time, not
by id (section 8). Sealing uses the same seal rule as bars (section 7), per event's minute.

## 5. Per-day file (replaces FLD1)

Path: `<flow_root>/<venue>/<symbol>/<YYYY-MM-DD>.flc` (UTC day). One file per instrument and day holds the
day's 24 1m chunks, its 1h chunk and its 1d bar. The 16-day 1d chunk is assembled at serve time from 16
day files (the heatmap RollupStore serves 1d the same way, MARKETDATA.md "Rollup levels").

| Part | Layout |
| --- | --- |
| Header | `FLC1`; u16 file version 1; u16 header length; u32 CRC-32 of the header with this field zeroed; str8 `instrument_id`; i64 day start; u32 generation; grid (32 bytes); u8 seal rule version; u32 seal window ms; 32-byte source fingerprint (SHA-256 of the RAWL2 source identities, as FLD1 `sourceFingerprint` and `.state` today, `servermodel/FlowStore.cpp:66-130`); u32 trades past the seal window; u32 trades rejected by the builder |
| Section table | 26 entries, fixed: 24 hour chunks (1m), then the 1h chunk, then the 1d bar. Each entry: u32 offset, u32 summary length, u32 rows length, u32 CRC-32 of both, u8 status (0 absent, 1 sealed). |
| Sections | each = summary section then rows section, exactly the `flow.rows` payload bytes of section 4 for that key, raw (no zstd) |

- The file stores what the wire sends, so a sealed chunk is served by copying bytes into an `SCK1` frame
  (plus zstd when it pays). No decode and no re-encode on read; the server checks the section CRC once
  when it loads the file and caches the frame (the heatmap's 256 MiB sealed LRU).
- The 1m section of an hour is written once the hour is sealed (section 7). The file is rewritten
  atomically (temp file, fsync, rename, directory fsync, as `atomicFile`, `servermodel/FlowStore.cpp:471-488`),
  adding sections. The 1h and 1d sections are written once all 24 hour sections are sealed (an unrecorded hour
  is a sealed section with no bars).
- Appending a sealed hour does not change sealed content, so it does not bump the generation.
- The `.state` sidecar goes away: the header carries the generation, fingerprint and counts.
- Uncompressed on disk: the busiest measured BTC day is 1.14 MB against 2.18 MB of FLD1, a median BTC day
  0.46 MB against 1.43 MB (section 12). zstd would save 15-35% and cost a decompression on every cold read.

## 6. In-memory FlowChunk

Client and server hold one decoded chunk as one allocation with per-field arrays. `FlowBar` stays the server
builder's working type (`flow/FlowBar.hpp:38-51`); FlowChunk is what is stored, served and drawn.

```cpp
struct FlowChunkKey { std::string instrument, channel; int64_t levelMs; uint32_t priceStep; int64_t startMs; };
struct CoverageRun { uint16_t fromSlot, toSlot; uint8_t status; };
struct FlowGrid { int64_t rowTickUnits, priceScale, baseScale, quoteScale; };
static_assert(sizeof(FlowGrid) == 32);
struct FlowRow { int64_t bidVol, askVol; uint32_t bidCount, askCount; };  // no offset, no padding
static_assert(sizeof(FlowRow) == 24 && alignof(FlowRow) == 8);

struct FlowChunk {                        // one allocation; arrays are spans into it
    FlowChunkKey key; uint32_t generation; uint16_t slots; uint16_t bars; uint32_t rows;
    std::span<const FlowGrid> grids;
    std::span<const CoverageRun> coverage;
    // Summary, bars entries each, ascending slot:
    std::span<const int64_t> open, high, low, close;          // price units
    std::span<const int64_t> baseVol, quoteVol, buyVol, sellVol;
    std::span<const uint32_t> count;
    std::span<const uint32_t> rowBegin;                       // bars + 1; empty for flow.summary
    std::span<const uint16_t> slot;
    std::span<const uint8_t> grid;                            // empty when grids.size() == 1
    // Rows, rows entries each:
    std::span<const FlowRow> row;
    std::span<const int16_t> offset;
};
```

| Part | Bytes |
| --- | --- |
| per bar (summary) | 8 x 8 + 4 + 4 + 2 + 1 = 75 |
| per row | 24 + 2 = 26 |
| BTC 1m hour chunk (60 bars, 37.8 rows each on average) | about 63 KB |

Arrays are laid out widest first (i64, u32, u16, u8) inside the allocation, each 8-byte aligned.
Rows stay sparse (an offset per present row). Row density is 0.75 for BTC 1m and 0.95-1.0 everywhere else
(section 12); a dense layout without offsets would cost 22% more for BTC 1m and save at most 8% elsewhere, so
one sparse layout serves every level.
`tests/core/flow_chunk_proto.hpp` holds the same arrays as vectors with the same `static_assert`s.

## 7. Seal and ownership

A 1m bar `[s, s + 60 s)` moves one way: **live** (now < s + 60 s), **closing** (until the seal), **sealed**.

| Rule | Definition |
| --- | --- |
| Membership | A trade belongs to bar b iff its exchange time is in `[s, s + 60 s)` and its record comes, in RAWL2 order, before the product's first record whose receive time is `>= s + 60 s + W` (the bar's seal point). With a monotone receive clock this is "receive time `< s + 60 s + W`". |
| Receive time | The RAWL2 record's `systemNs / 10^6`, the time the store builder uses today (`servermodel/FlowStore.cpp:452`). RAWL2 order is `JournalReader` order (run start, run id, segment). Membership is a pure function of RAWL2, and it is what live sees: live seals a bar when its receive watermark passes the seal point, so a record after a backward clock step is late in both. |
| Seal window W | **15 s** (`flow.seal_window_ms`, server config, recorded in each day file header). |
| Sealed through T | per instrument, the largest hour boundary such that every hour from the first recorded hour up to it has a sealed section on disk. An hour `[h, h + 1 h)` can be built when the receive watermark passes `h + 1 h + W`. |
| Rollups | 1h and 1d bars are `flow::composeFlowBars` (`flow/FlowCompose.cpp:9-70`) of sealed 1m bars only. sum(1m) == 1h holds by construction. |
| Late trades | A trade that misses its bar's seal is not in any sealed bar. The builder counts it per day (file header) and in `sentinel_flow_late_trades_total{instrument}`. It stays in RAWL2. Live drops it from the bar too (it would revise a bar whose sealed bytes will not contain it). |

Why W = 15 s: on the 4 measured days (3 BTC, 1 ETH, 6.5 M trades) the latest trade arrived 1.3 s after
its bar ended, the 99.9th percentile within 0.02 s, and neither a 15 s nor a 60 s window excludes a single trade
(section 12). Both windows give the same bars on this data; 15 s is the smaller one with ten times the observed
maximum as margin. It also bounds the live clock offset: receive minus exchange went as low as -0.14 s. W only
delays when an hour can be built (by W) and how long live keeps closing bars; a larger W buys nothing that was
measured. Re-measure over all recorded days before the cutover (open item 1).

Today's store rule is different and not deterministic per bar: it reads RAWL2 until receive time passes the end of
its whole build run plus 60 s (`servermodel/FlowStore.cpp:455`, runs grouped at `:996-1032`), so a trade an hour late
is in the bar when its hour was built in a later run, and out when not. The new builder applies the membership rule
per bar.

**Ownership.**
- History serves only chunks whose whole span is before T. It never serves an open chunk, and the client never
  requests a key whose span reaches T.
- The live stream owns everything at or after T: closing and sealed-but-not-yet-built minutes, and the live bar.
  Its retention therefore covers at least `[T, now]` (today it keeps the forming hour plus grace and up to six
  hours the store has not built, `servermodel/ServerDataModel.cpp:1152-1159`).
- First connect: the reply to `flow_live_subscribe` starts with one closing-tail snapshot (section 8) over
  `[T, live bar]`, then live updates.

**Generation and revalidation.**
- Each day file has a u32 generation, starting at 1. A rebuild whose sealed content differs (a changed source
  fingerprint, a new grid, a seal-rule or scale-rule change) writes generation + 1. Adding a sealed hour does not.
- A chunk's generation = the sum of the generations of the days its span covers (1 day for 1m and 1h chunks, up to
  16 for 1d). Any day's bump raises it.
- `chunk_availability.generations` lists days with generation above 1. A client holding a sealed chunk whose
  computed generation differs revalidates it with `have_hash`; an unchanged content hash returns `not_modified`.

## 8. Live tail

- `flow_live_subscribe` and `flow_bar` keep today's semantics (MARKETDATA.md "Live flow bars"): one message per
  changed bucket, `revision` as envelope metadata, higher revision wins, at equal revision `complete` wins.
- New: the subscribe reply begins with a closing-tail snapshot, one binary `SCK1` frame of kind 2 (`live`, state
  open, never cached) on `channel` `flow.rows` or `flow.summary` (as subscribed). Only in this frame `level_ms` is
  the subscribed timeframe (any served minute multiple); `start_ms` = T floored to that timeframe, `end_ms` = end
  of the live bar, slots = (end - start) / timeframe. Its payload is the section 4 payload over those slots, the
  bucket crossing T included as the provisional composite, followed by a live trailer: varint revision per
  present bar, u8 flags per present bar (bit 0 closed, bit 1 complete). Coverage may say `building` (for example
  after a server restart, open item 5).
- 1 s subscriptions are unchanged: sub-minute flow has no chunks.
- `flow_bar` messages then follow for buckets at or after T only.
- `chunk_availability` carries the new T whenever it advances. Client merge at T:

| Bucket | Source |
| --- | --- |
| end <= T | sealed chunks only. Live data for it is dropped (today's rule, MARKETDATA.md "Client merge of flow bars"). |
| start >= T | live only (snapshot, then `flow_bar`). |
| start < T < end (coarse bucket crossing T) | live: the server composes it from sealed hours and live minutes, as today's provisional crossing bucket. |

When T advances, the client drops live buckets that now end at or before T only after the covering chunk is in its
store, so the picture never shows a hole.

## 9. Client fetch

| Rule | Value |
| --- | --- |
| Wanted range | the viewport plus padding: 2 screens on each side, 4 toward the pan direction (the sign of the last pan), ending at T |
| Rounding | out to whole chunks of the level in use |
| Padding cap | 32 MiB of estimated chunk bytes per chart (estimate: section 12 sizes per level; known decoded size once fetched). Visible chunks are never capped. |
| Level | the coarsest stored level dividing the timeframe; finer levels at the edges where a coarse chunk would cross T (the heatmap's `tiles::chunksFor`, `heatmap/HeatmapTiles.hpp:72`). 5m/15m from 1m, 4h from 1h, 1w from 1d. |
| Channel | `flow.summary` for candles, volume, CVD and the footer; `flow.rows` for footprint, TPO and VP. A held `flow.rows` chunk answers a summary want. |
| Order | visible chunks (newest first), then the pan side, then the other side |
| In flight | at most 8 chunks and 8 MiB of estimated bytes across all charts |
| Cache | one byte-budgeted LRU of decoded FlowChunks shared by every chart, 256 MiB to start; wanted keys are never evicted; survives timeframe and symbol switches (keys carry the instrument) |
| Draw | only the viewport; padding is fetched and held, not drawn |
| Never | requests at or after T, or for future time; no edge throttles |

Retry, busy backoff, deadlines and reconnect follow the heatmap fetcher (MARKETDATA.md "Process-wide chunk fetching").

## 10. Rebuild and migration

1. The server builds `.flc` files from RAWL2 in a background thread (the FlowStore builder's priority and
   rescan), newest day first, into a new root beside the FLD1 root. FLD1 keeps serving FHE1 meanwhile.
2. Every day's `.flc` is checked against its FLD1 day: per 1m bar, the summary fields and rows equal FLD1's
   (notables excluded), except bars that differ because of the per-bar membership rule; each such bar is listed
   with the trades that moved.
3. Cutover (owner approval at the Mac required): the server serves `chunk_request` from `.flc`; the client moves
   to chunks (step 5); FHE1, FLB1 history, FLD1 files, their `.state` sidecars, `flow::encodeFlowDay` /
   `decodeFlowDay` and the FLD1 tests are deleted in the same change. No backward compatibility.
4. The FLD1 root is deleted after the cutover, by the conductor with the owner present.
5. `flow_bar` FLB1 live messages stay until the live path is reworked; notables leave FLB1 when `trades.big` ships.

## 11. What stays as is

`flow_bar` revision semantics; the 1 s live series and its journal replay (RAM only); `FlowBarBuilder` and
`composeFlowBars`; the recorder grid as the row grid; quote scale 10^8.

## 12. Measurements

Run 2026-11-22 on the recorded data under `/Volumes/T7/sentinel-data` (read-only). Commands:
`flow_history_bench --lateness <raw-l2> <symbol> <days...>` and
`flow_history_bench --columnar <hmc2-flow> <symbol> <days...>`.

### Lateness

Every trade with its exchange time in the day, deduplicated and ordered by `JournalFeed` as the store builder
does, receive time = RAWL2 record time, read until one hour past the day. "After bar end" = receive time - end
of the trade's 1m bar.

| Day | Trades | Receive - exchange p50 / p99 / p99.9 / max | min | After bar end: 0-100 ms / 100 ms-1 s / 1-5 s / >= 5 s | After bar end p99.9 / max | Excluded at 15 s / 60 s | Reconnects |
| --- | --- | --- | --- | --- | --- | --- | --- |
| BTC 2026-11-19 | 1,894,339 | 67 / 393 / 868 / 12,942 ms | -139 ms | 2,021 / 399 / 0 / 0 | 20 / 456 ms | 0 / 0 | 3 |
| BTC 2026-11-20 | 2,478,362 | 66 / 889 / 2,404 / 4,215 ms | -58 ms | 2,159 / 698 / 1 / 0 | 7 / 1,278 ms | 0 / 0 | 8 |
| BTC 2026-11-21 | 1,695,851 | 50 / 258 / 742 / 3,345 ms | -116 ms | 1,539 / 136 / 0 / 0 | -1 / 142 ms | 0 / 0 | 4 |
| ETH 2026-11-20 | 434,248 | 57 / 952 / 1,579 / 3,066 ms | -58 ms | 320 / 180 / 0 / 0 | 17 / 880 ms | 0 / 0 | 8 |

- 99.8% of trades arrived before their bar ended and all but one less than 1 s after it. The one (1,278 ms)
  came in a reconnect snapshot frame; reconnect snapshots recovered nothing else late.
- Negative receive - exchange (down to -139 ms) is clock offset between Coinbase and the capture host.
- Scan cost: 53-67 s per day.

### Sizes

Lean = the section 4 payload (`flow_chunk_proto.hpp`, `Delta`); "no deltas" = the same arrays as plain varints
(`Varint`); "fixed" = fixed-width arrays, the in-memory image (`Fixed`). FLB1 = today's FLB1 frames plus their
24-byte FLD1 index entries. All per-hour pieces compressed independently. Bytes per 1m bar in brackets.

1m level, one day of hour chunks:

| Day | FLB1 | FLB1 no notables | Lean fixed | Lean no deltas | Lean | Lean zstd 1 | Lean zstd 3 | Summary only |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| BTC 2026-11-20 (busy) | 2,179,776 (1514) | 1,454,016 | 3,201,928 | 1,198,853 | 1,146,131 (796) | 850,299 | 838,635 | 46,526 (32.3) |
| BTC 2026-10-22 (median) | 1,429,529 (993) | 703,769 | 1,351,612 | 479,416 | 459,502 (319) | 363,060 | 362,395 | 45,264 (31.4) |
| ETH 2026-11-20 | 1,250,098 (868) | 535,858 | 712,584 | 323,156 | 313,565 (218) | 266,660 | 266,662 | 45,903 (31.9) |
| SOL 2026-11-20 | 1,196,490 (831) | 482,250 | 599,874 | 267,274 | 261,538 (182) | 220,058 | 219,831 | 44,658 (31.0) |
| PEPE 2026-10-13 (1,150 bars) | 477,597 (415) | 216,578 | 123,656 | 47,878 | 43,302 (38) | 28,538 | 28,216 | 28,032 (24.4) |

- Lean against today's FLD1 file: BTC 1.9x (busy) to 3.1x (median) smaller, ETH 4.0x, SOL 4.6x, PEPE 11.4x.
  Dropping notables gives most of it; varints give the rest; deltas add 3-10%.
- zstd on the lean form: 15-26% saving at 1m (PEPE 34%), 10-16% at 1h and 1d; level 3 within 1% of level 1.
  The summary alone compresses 5-8% (BTC, ETH, SOL) and 21% (PEPE). The fixed-width in-memory image is larger
  than today's FLB1.
- Day file (header once, offsets, 24 sections): BTC busy 1,144,866 bytes, median 458,237. The prototype's
  header and offsets are 127 bytes; section 5's header and 26-entry table are about 560 bytes.
- Decode of a whole day: BTC busy 1.6 ms lean (2.0 ms to `FlowBar`s) against 5.0 ms for FLD1 without its
  validation re-encode; ETH 0.4 against 1.1 ms; PEPE 0.08 against 0.31 ms.

1h and 1d levels, composed from 1m with `flow::composeFlowBars`:

| Chunk | BTC | ETH | SOL | PEPE |
| --- | --- | --- | --- | --- |
| 1h day chunk, average over all days, lean | 110 KB (4,602 B/bar) | 28 KB | 29 KB | 3.2 KB |
| 1h day chunk, zstd 1 | 94 KB | 25 KB | 25 KB | 2.9 KB |
| 1h day chunk, BTC busy day 2026-11-20 | 212 KB (182 KB zstd 1) | | | |
| 1d 16-day chunk, lean (16 days) | 418-426 KB (26 KB/bar) | 102-120 KB | 103-122 KB | 7.8-12.6 KB |
| 1d 16-day chunk, zstd 1 | 353-358 KB | 89-106 KB | 90-106 KB | 6.9-11.0 KB |
| Summary per bar, 1h / 1d | 39 / 44 B | 38 / 44 B | 36 / 43 B | 33 / 37 B |

All 54 BTC days: 1h chunks 5.9 MB lean (5.0 MB zstd 1); the four 1d chunks 1.63 MB.

### Rows and offsets (all FLD1 days: BTC 54, others 53)

| Instrument, level | Row density (rows / (max offset + 1)): total, p10, p50 | Widest offset range (day) | int16 headroom |
| --- | --- | --- | --- |
| BTC 1m | 0.755, 0.47, 0.74 | 1,408 (2026-11-13) | 23x |
| BTC 1h | 0.952, 0.88, 0.96 | 2,911 (2026-11-21) | 11x |
| BTC 1d | 0.992, 0.98, 0.99 | 5,809 (2026-11-05) | 5.6x |
| ETH 1d | 1.000 | 1,162 (2026-11-05) | 28x |
| SOL 1d | 0.999 | 1,116 (2026-10-08) | 29x |
| PEPE 1d | 1.000 | 131 (2026-10-29) | 250x |

BTC 1m averages 37.8 rows per bar. Checks over every bar: lean round trip equal to FLD1 without notables (0
mismatches), low row = `floorDiv(low, row_tick_units)` (0 mismatches), native tick = row tick units / price scale
(0 mismatches), no 1d composition overflowed int16.

### Big trades (from FLD1 notables)

Sweep-collapsed events at or above a floor, per day. Notables keep the top 8 per minute, so these are lower
bounds; "truncated" counts minutes whose eighth notable was at or above the floor.

| Floor | BTC | ETH | SOL | PEPE |
| --- | --- | --- | --- | --- |
| $10,000 | 5,908 (22,255 truncated) | 3,334 | 1,020 | 51 (11) |
| $25,000 | 3,030 | 1,322 | 287 (170) | 15 |
| $50,000 | 1,567 | 344 (170) | 110 | 5.3 |
| $100,000 | 500 (324) | 98 | 40 | 1.4 |
| $250,000 | 98 | 23 | 8.6 | 0.08 |
| $1,000,000 | 7.3 | 1.2 | 0.4 | 0 |

Not measured: an event in the section 4 layout should take about 15-20 bytes, so 500 a day is about 9 KB.

## 13. Open items

1. **Lateness on every day.** Four days were measured. Before the cutover, run `--lateness` over every recorded
   day and product (about a minute per day, in the background) and check that no day excludes a trade at 15 s.
   The metric `sentinel_flow_late_trades_total` then watches production.
2. **Big-trade floors** (owner decision: what bubbles show far back). The table in section 4 is a proposal.
   Sweep grouping beyond same millisecond, same side and consecutive ids (plan question 2) stays as today.
3. **Days without a recorder grid** (plan question 4). The live model drops their trades
   (`sentinel_flow_missing_grid_trades_total`); the store already has a journal grid fallback
   (`dayScales`, `servermodel/FlowStore.cpp:342-412`). The live path needs the same before chunks replace FHE1.
4. **Server memory** (plan question 5): profile the ~877 MB before step 3 adds the sealed-frame cache.
5. **Startup gap.** After a server restart, live holds only bars since its first live trade, so `[T, first
   trade)` is `building` in the closing-tail snapshot until the store seals it. Startup replay of the open hour
   (plan step 6) closes it.
6. **Live message format.** Live updates stay `flow_bar` with FLB1 bodies. Moving them onto `SCK1` kind 2
   frames (and dropping FLB1 notables when `trades.big` ships) is left to the live rework.
7. **Heatmap generations** (step 4): how RollupStore fingerprints and HMC2 rewrites map to the per-day
   generation, replacing "a GUI does not revalidate sealed chunks" (MARKETDATA.md "Changed content").
8. **Kalshi.** Its instruments have tick bands, not one tick; the flow row grid is defined only for a single
   recorder tick. Flow for Kalshi needs its own grid rule before it gets chunks.
9. **Coarser rows** (`price_step` > 1): the key has the slot; the composition rule (sum of native rows per
   coarse row) and when the server prebuilds them are not specified.
