# Flow chunks

**Draft spec, not implemented.** Nothing here describes running code. Flow history today (FLD1 day
files, FHE1 pages, `flow_bar` live messages) is in [MARKETDATA.md](MARKETDATA.md) "Live flow bars" and
"Flow history". This spec replaces FLD1 and FHE1 and puts flow on heatmap-style chunks. The server
(plan step 3) and the client (step 5) implement it; step 4 moves the heatmap onto the same envelope.
Measurements are in section 12. They come from `flow_history_bench --lateness` and `--columnar`
(`tests/core/flow_history_bench.cpp`). `tests/core/flow_chunk_proto.hpp` is a prototype of the section 4
payload, the section 5 day file and the 16-day assembly, with golden bytes in `test_flow_chunk_proto.cpp`.

Decided rules this spec makes exact:
- RAWL2 is the only truth. Every flow file is a rebuildable index of it.
- A sealed bar is a deterministic function of the RAWL2 archive (the canonical hour scan, section 7). 1h and 1d
  bars are composed from sealed 1m bars only.
- History serves only sealed chunks. Live bars are a provisional preview that the sealed chunk replaces; they need
  not equal it.
- Sub-minute flow is never written to disk.

## 1. Units and primitives

One unit per quantity. Every price is in **price units**; rows are used only for row offsets.

| Quantity | Unit | Stored as | Source |
| --- | --- | --- | --- |
| Price, open, high, low, close | price units = price x `price_scale` | i64 | `price_scale` is at least 1 / quote increment (`roller/Grid.cpp:53`), so every trade price is an integer; conversion `recording::priceUnits` (`servermodel/RecordingCodec.hpp:89`) |
| Row | `floorDiv(price units, row_tick_units)` on the day's recorder grid | not stored | `flow/FlowBarBuilder.cpp:142` |
| Row offset | row - low row of its bar | i16, 0..32767 | `flow/FlowBar.hpp:21` |
| Low row | `floorDiv(low, row_tick_units)` | derived | holds for every FLD1 bar (section 12) |
| Base volume | size x `base_scale` (10^decimals of `base_increment`) | i64, >= 0 | `flow/FlowScales.cpp:8-27` |
| Quote volume | price x size x `quote_scale` (10^8), rounded once per trade | i64, >= 0 | `flow/FlowBarBuilder.cpp:95-116` |
| Trade count | trades, unknown side included | u32 | |
| Time | ms since the Unix epoch, UTC | i64 | |

- VWAP (derived): `(quote_volume / quote_scale) / (base_volume / base_scale)` in quote currency per base unit;
  x `price_scale` for price units. Every bar has a trade, so `base_volume` > 0; a composed bucket with no bars
  has no VWAP.
- varint: unsigned LEB128, 7 bits per byte, low group first, at most 10 bytes, the tenth byte at most 1, no
  trailing 0x00 byte except for the value 0 itself (the FLB1 rule, `flow/FlowCodec.cpp:33-45`). zigzag varint:
  varint of `(v << 1) ^ (v >> 63)`. Decoders reject everything else.
- Fixed-width integers are little-endian. `str8` = u8 length, then bytes.

## 2. Chunk key

`(instrument_id, channel, level_ms, price_step, start_ms)`

| Field | Form | Rule |
| --- | --- | --- |
| `instrument_id` | `<venue>:<symbol>`, ASCII, at most 72 bytes | venue `coinbase` or `kalshi` (`InstrumentSpec.hpp:15-21`); symbol as the venue spells it, checked by `validateInstrumentId` (`InstrumentSpec.hpp:23-32`). Example `coinbase:BTC-USD`. Required in every new file, key and cache. |
| `channel` | string, at most 32 bytes | `flow.summary`, `flow.rows`, `trades.big`. Step 4 adds `book.near`, `book.deep`. |
| `level_ms` | i64 | 60000, 3600000 or 86400000 |
| `price_step` | u32 | native rows per stored row: 1 = native grid; 0 for channels without rows (`flow.summary`, `trades.big`). v1 accepts only these two values. |
| `start_ms` | i64 | `heatmap::chunkStartOf(level, t)` (`heatmap/ChunkCodec.cpp:238-242`) |

| Level | Slots | Span | Alignment |
| --- | --- | --- | --- |
| 1m | 60 | 1 UTC hour | Unix-epoch hour |
| 1h | 24 | 1 UTC day | UTC midnight |
| 1d | 16 | 16 UTC days | whole 16-day spans after 2000-01-01 (`kDayChunkDays`, `heatmap/ChunkCodec.hpp:30`) |

`trades.big` uses the same levels; for it the level only selects the span (events are not bars).

Flow timeframes are the ones flow serves today: 1m multiples that divide a UTC day (MARKETDATA.md "Flow
history"). A composed bucket therefore never crosses midnight and never mixes grids; `composeFlowBars`
refuses mixed grids (`flow/FlowCompose.cpp:31-37`, test `ComposeRefusesMixedGrids`). 1w is open item 9.

## 3. Envelope and availability

The binary envelope stays SHE1 exactly as today: `SHE1`, u16 version 1, u64 request id, then the inner frame
(`heatmap/ChunkCodec.cpp:458-464`). The decoder dispatches on the inner magic: `SHC1` (heatmap wire v3, until
step 4) or `SCK1` (this spec).

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
| 13 | `epoch` | u64 | the channel's epoch domain (section 7) |
| 14 | `content_hash` | u64 | FNV-1a 64 over fields 5-11 then the raw payload (the SHC1 FNV rule, `heatmap/ChunkCodec.cpp:58-66`). Kind, state, revision and epoch are outside it, so equal content has equal hashes across kinds and epochs. |
| 15 | `payload_version` | u16 | per channel; 1 here |
| 16 | codec | u8 | 0 raw, 1 zstd |
| 17 | raw length | u32 | at most 16 MiB |
| 18 | stored length | u32 | equals the remaining frame bytes |
| 19 | payload | bytes | defined by (channel, kind, payload version) |

- `not_modified` (3): fields 1-14. Field 14 echoes the requester's `have_hash`, which equals the current chunk's
  content hash; fields 11-13 are the current chunk's (so a client holding an older epoch takes the current one).
  `error` (4): fields 1-9, u16 code, str8 message; codes as SHC1 (`heatmap/ChunkCodec.hpp:48-53`).
- `live` (2) frames and open chunks (state 0) exist only for channels whose availability levels carry
  `committed_through_ms` (the heatmap minute level). Each such channel defines its own kind 2 payload. Flow sends
  neither: flow history is sealed chunks only and flow live stays `flow_bar` (section 8).
- Limits and decoder checks as SHC1: frame and raw payload at most 16 MiB; the zstd content size is checked
  before allocation; every count is bounded by the remaining bytes before allocation.
- Codec: zstd level 1 when it shrinks the payload by at least 10%, otherwise raw (section 12). The server caches
  the finished frame.

Request (JSON): `{"type":"chunk_request","req":<u64>,"instrument":"coinbase:BTC-USD","channel":"flow.rows","level_ms":60000,"price_step":1,"starts":[<i64>...],"have_hash":["<16 hex>"|""...]}`.
1..64 starts, one binary reply each (chunk, not_modified or error); budgets, busy and the history pool as
`heatmap_chunk_request` (MARKETDATA.md "Heatmap chunk wire"). A flow key that is not sealed gets `unavailable`.

Availability (JSON) is a full snapshot per instrument that replaces the previous one, sent on subscribe and on
every change:

```
{"type":"chunk_availability","instrument":"coinbase:BTC-USD","wire_version":1,
 "live_from_ms":<i64>,                                    // flow, section 7
 "channels":{"flow.rows":{"epoch":"<16 hex>","meta":{},
   "levels":[{"level_ms":60000,"chunk_span_ms":3600000,"oldest_ms":..,"latest_ms":..,
              "sealed":[[<from>,<to>],...]}]}}}            // book levels add "committed_through_ms"
```

`sealed` lists every coalesced run of sealed chunk starts as half-open ranges, ascending. `meta` is
channel-specific; empty for flow.

Heatmap mapping (step 4 moves the heatmap; flow uses SCK1 from step 3):

| Today (`heatmap/ChunkCodec.cpp:326-336`, `protocol/ChunkWire.hpp:137-164`) | SCK1 |
| --- | --- |
| `SHC1` version 3; `heatmap_chunk_request`, `heatmap_availability` | `SCK1` version 1; `chunk_request`, `chunk_availability`; the old ones are deleted |
| str8 symbol, str8 source `hmc2.near` | `instrument_id`; channel `book.near` (`book.deep`) |
| - | `price_step` 1; `epoch` (domains `book.near`, `book.deep`) |
| grid identity, `SizeScale`, column and entry counts | heatmap payload header |
| live `tailStartMs` after `end` | head of the `book.*` kind 2 payload |
| zstd level 3 always | codec 1 at level 3; payload bytes unchanged |
| availability `grids` (config hash, row tick, price scale, size floor, codes per octave) | `channels.book.*.meta.grids`, unchanged |
| per-level `oldest_ms`, `latest_ms`, `committed_through_ms` (1m: recorder minute cutoff; 1h, 1d: RollupStore built interval) | the same per level; `sealed` = `[oldest, committed_through)` in whole chunks |
| open 1m chunks with revision and committed-through | unchanged (state 0) |
| a running GUI never revalidates sealed chunks | revalidation when the domain epoch changes (open item 7) |

Step 4 parity tests compare decoded `SparseColumns`, the availability fields above and the live frames before
and after the move.

## 4. Payloads

### flow.summary and flow.rows

`flow.rows` = header + summary section + rows section. `flow.summary` = header + summary section: a byte prefix
of the `flow.rows` payload, cut by the server without re-encoding. A held `flow.rows` chunk answers a
`flow.summary` want. The slot count comes from the key (section 2), not the payload.

| Header field | Type | Meaning |
| --- | --- | --- |
| `grid_count` | u8 | 0 only when the chunk has no bars; 1 in 1m and 1h chunks (one UTC day has one grid, `flow/FlowCodec.cpp:165-167`); up to 16 in 1d chunks |
| grids | `grid_count` x 32 | i64 `row_tick_units`, i64 `price_scale`, i64 `base_scale`, i64 `quote_scale` |
| `run_count` | u8 | coverage runs |
| runs | `run_count` x 5 | u16 from slot, u16 to slot (exclusive), u8 status: 1 complete, 2 partial, 3 unrecorded. Ascending, contiguous, covering `[0, slots)` exactly. A building slot never appears: a chunk with one is not sealed and not encoded. |

Summary section, field-major (every value of one field, then the next). `n` = bars with trades.

| Field | Count | Encoding | Meaning |
| --- | --- | --- | --- |
| `n` | 1 | varint | at most the slot count |
| slot gap | n | varint | slot - previous slot - 1 (previous starts at -1); bar start = chunk start + slot x level |
| grid index | n, only if `grid_count` > 1 | u8 | |
| open | n | zigzag varint | open - previous bar's close (the first bar: open - 0) |
| up | n | varint | high - open |
| down | n | varint | open - low |
| close | n | varint | close - low |
| base, quote, buy, sell volume | n each | varint | buy = aggressor buys, sell = aggressor sells |
| count | n | varint | all trades |

Rows section:

| Field | Count | Encoding | Meaning |
| --- | --- | --- | --- |
| row count | n | varint | 1..32768 per bar |
| offset gap | rows | varint | offset - previous offset - 1 within the bar (the first row: its offset, 0) |
| bid volume, ask volume | rows each | varint | sell-aggressor and buy-aggressor base volume at the row |
| bid count, ask count | rows each | varint | |

Derived on read, never stored: low row, delta = buy - sell, unknown-side volume = base - buy - sell, maximum row
volume, VWAP (section 1). A row with zero volumes exists where only unknown-side trades printed
(`flow/FlowBarBuilder.cpp:189-195`).

The decoder rejects: anything section 1 rejects; runs that do not tile `[0, slots)` or carry an unknown status;
`n` above the slot count; slots not ascending or outside the span; a grid index out of range; a price outside
i64; `low <= 0`; `high < close`; `buy + sell > base`; a bar without rows; a bid sum other than sell or an ask
sum other than buy; known row counts above `count`; offsets not ascending or above 32767; a first offset other
than 0; a last offset other than `floorDiv(high, row_tick_units) - low row` on the bar's own grid; trailing
bytes. A `complete` slot without a bar is an observed empty minute; `unrecorded` means no recording.

Golden bytes for one bar: `test_flow_chunk_proto.cpp`, `GoldenSpecBytes`.

### trades.big

Events of the chunk's span: one UTC hour (level 60000), one UTC day (3600000) or 16 days (86400000);
`price_step` 0. A coarser chunk holds exactly the events of its finer chunks.

- Order of steps per trade: the section 7 seal rule, trade-id deduplication, grouping, then the floor.
- Grouping (canonical, as notables today): within each 1m bucket, sort the accepted trades by exchange ms, then
  numeric trade id, then id text (`tradeLess`, `flow/FlowBarBuilder.cpp:20-26`). A sweep is a maximal run of
  neighbours in that order with consecutive numeric ids, equal exchange ms and equal side
  (`flow/FlowBarBuilder.cpp:202-225`). A trade with a non-numeric id is its own event.
- Floor: an event is kept when its notional is at least the instrument's `big_floor_quote` (quote units, server
  config). Section 12 proposes floors (owner decision, open item 2). The floors are part of the epoch.

| Field | Count | Encoding | Meaning |
| --- | --- | --- | --- |
| `grid_count`, grids | | as flow | one grid per UTC day in the span |
| floor | 1 | varint | `big_floor_quote` |
| `n` | 1 | varint | events, ascending by (exchange ms, first trade's sort key) |
| grid index | n, only if `grid_count` > 1 | u8 | |
| time | n | varint | exchange ms - previous event's (the first: - chunk start) |
| side | n | u8 | 0 unknown, 1 buy, 2 sell |
| price | n | zigzag varint | first trade's price units - previous event's (the first: - 0), on the event's grid |
| low, high | n each | varint | price - lowest sweep price; highest sweep price - price |
| size, notional | n each | varint | base units, quote units |
| trades, levels | n each | varint | trades collapsed; distinct prices |

Trade ids are not stored: sealed chunks and the live tape meet at the sealed boundary by exchange time.
`trades.big` is not prototyped.

## 5. Per-day file (replaces FLD1)

Path `<flow_root>/<venue>/<symbol>/<YYYY-MM-DD>.flc` (UTC day). Prototype: `encodeDayFile`, `parseDayFile`.

| Part | Layout |
| --- | --- |
| Header | `FLC1`; u16 version 1; u16 header length; u32 CRC-32 (IEEE) of the header with this field zeroed; str8 `instrument_id`; i64 day start; grid (32 bytes); u8 rule version; u32 seal window W (ms); u32 trades past their seal; u32 trades rejected by the builder; u32 receive-time regressions; u32 CRC-32 of the table |
| Table | 50 entries of 25 bytes: u32 offset, u32 summary length, u32 total length, u32 CRC-32 of the section, u8 state (0 absent, 1 sealed), u64 source fingerprint (hour entries: the RAWL2 prefix through the hour's seal point, from FlowStore's source identities, `servermodel/FlowStore.cpp:66-130`; 0 otherwise) |
| Sections | back to back in table order, starting right after the table |

| Entry | Content | Payload key |
| --- | --- | --- |
| 0-23 | `flow.rows` payload of hour h | (1m, hour start), 60 slots |
| 24 | `flow.rows` payload of the 1h chunk | (1h, day start), 24 slots |
| 25 | the day's 1d piece: a `flow.rows` payload with 1 slot | (1d, day start), 1 slot |
| 26-49 | `trades.big` payload of hour h | (60000, hour start) |

Coverage of a composed slot (a 1h slot from its 60 minutes, a 1d piece from its 24 hours): `complete` if all parts
are complete, `unrecorded` if all are unrecorded, otherwise `partial` (the FHE1 rule, MARKETDATA.md "Flow history").

- Validation on load: the header CRC; the table CRC; per entry, `state` 1 iff total length > 0, offset 0 iff empty,
  non-empty entries back to back from the end of the table, the section CRC; for a sealed flow entry (0-25) a summary
  length in `1..total`, the first summary-length bytes decode as a `flow.summary` payload and the whole section as
  a `flow.rows` payload of its key; for `trades.big` entries summary length 0; the file ends at the last section.
  Any failure makes the whole file unreadable: its keys leave `sealed` and the day is rebuilt. Tests:
  `DayFileTableCorruptionIsRefused`, `DayFileTableRules`.
- Serving 1m and 1h: the section bytes are the payload (cut at the summary length for `flow.summary`), without
  decode or re-encode.
- Serving the 16-day 1d chunk and the day and 16-day `trades.big` chunks: assembled once from pieces, encoded and
  cached (prototype `assembleDayChunk`):
  1. slot = (day - chunk start) / 1 day. A day with a sealed piece takes the piece's coverage status and bar. A day
     without a RAWL2 source is `unrecorded`. Any other day leaves the chunk unsealed (not served).
  2. Grids in slot order of first use; a grid index per bar when there is more than one.
  3. Coverage runs from the slot statuses, equal neighbours merged.
  4. Encode with section 4 from scratch (the open deltas restart at the first bar).
- An hour section is written when the hour is sealed; entries 24 and 25 when all 24 hours are; a `trades.big` hour
  with its flow hour. The file is rewritten atomically (temp file, fsync, rename, directory fsync, as `atomicFile`,
  `servermodel/FlowStore.cpp:471-488`). There is no `.state` sidecar.
- Raw on disk: zstd 1 would shrink a file by 16-38% (section 12) but cost a decompression on every cold read.

## 6. In-memory FlowChunk

One decoded chunk is one allocation with per-field arrays, on the server and the client. `FlowBar` stays the
builder's working type (`flow/FlowBar.hpp:38-51`).

```cpp
struct FlowChunkKey { std::string instrument, channel; int64_t levelMs; uint32_t priceStep; int64_t startMs; };
struct CoverageRun { uint16_t fromSlot, toSlot; uint8_t status; };
struct FlowGrid { int64_t rowTickUnits, priceScale, baseScale, quoteScale; };
static_assert(sizeof(FlowGrid) == 32);
struct FlowRow { int64_t bidVol, askVol; uint32_t bidCount, askCount; };  // no offset, no padding
static_assert(sizeof(FlowRow) == 24 && alignof(FlowRow) == 8);

struct FlowChunk {                        // arrays are spans into one allocation, widest type first
    FlowChunkKey key; uint64_t epoch; uint16_t slots, bars; uint32_t rows;
    std::span<const FlowGrid> grids;
    std::span<const CoverageRun> coverage;
    std::span<const int64_t> open, high, low, close;          // price units, one per bar
    std::span<const int64_t> baseVol, quoteVol, buyVol, sellVol;
    std::span<const uint32_t> count;
    std::span<const uint32_t> rowBegin;                       // bars + 1; empty for flow.summary
    std::span<const uint16_t> slot;
    std::span<const uint8_t> grid;                            // empty when grids.size() == 1
    std::span<const FlowRow> row;                             // one per row
    std::span<const int16_t> offset;
};
```

75 bytes per bar and 26 per row: a BTC 1m hour chunk (37.8 rows per bar on average) is about 63 KB. Rows stay
sparse: density is 0.75 for BTC 1m and 0.95-1.0 elsewhere (section 12); a dense layout would cost 22% more for
BTC 1m and save at most 8% elsewhere.

## 7. Seal, ownership, epoch

### Seal rule

A sealed bar is a deterministic function of the RAWL2 archive, computed by the canonical hour scan below. History
serves only sealed bars. Live bars, closing and live-final ones included, are a provisional preview: they follow the
same rule on what live saw, but nothing requires them to equal the sealed bar.

| Term | Definition |
| --- | --- |
| RAWL2 order | `JournalReader` order: files by (`run_started_system_ns`, run id, segment) (`roller/JournalReader.cpp:49,89`), records in file order |
| Canonical hour scan of hour h | read RAWL2 in that order from the first durable record with receive time >= h - 60 s, with a fresh `JournalFeed` (its trade-id window fills during the lead-in), until the first record with M >= h + 1 h + W |
| Watermark M(R) | the maximum receive time (`systemNs / 10^6`) over every record from the scan start through record R, all record kinds, R included |
| Membership | a trade of record R belongs to 1m bar `[s, s + 60 s)` iff its exchange time is in the bar and `M(R) < s + 60 s + W`; equality excludes |
| W | **15 s** (`flow.seal_window_ms`, written in the day file header) |

- An hour's bytes depend only on the archive, the rules and its own scan start, so any pass grouping and any rebuild
  give the same bytes. A pass may share one read across hours when the result is the same.
- Trades that miss their bar are counted (header, `sentinel_flow_late_trades_total`) and stay in RAWL2.
- Receive-time regressions (a record below M of its predecessor) are a diagnostic only: counted in the header and
  `sentinel_flow_receive_regressions_total`. They change neither coverage nor content.
- 1h and 1d bars are `flow::composeFlowBars` (`flow/FlowCompose.cpp:9-70`) of sealed 1m bars only.

**Live.** Step 3 delivers every applied record's receive time to the flow model before the record's trades,
unthrottled (today the roller sends it after the trades, at most once per journal second,
`roller/ShadowRoller.cpp:329-338,521-526`). The model keeps M per product, counts and drops a trade whose bar's seal
point is at or below M, and marks a bar final when M reaches the seal point. With one run and a monotone clock this
gives the sealed bar; after a clock step back, a capture restart or a retracted suffix it may not. When the store
seals an hour, the server compares each live-final 1m bar it still holds with the sealed bar (both are in the same
process) and counts every difference in `sentinel_flow_live_seal_mismatch_total{instrument}`. The client replaces
live bars with the sealed chunk in any case (section 8).

Fixtures (step 3 acceptance):
- Determinism: the same RAWL2 built in one pass, hour by hour and in reverse order gives identical bytes, including
  an archive with a backward clock step inside a run and a reordered, non-overlapping run (a restart after a clock
  step back whose run sorts earlier).
- Live: on archives without regressions the live finals equal the sealed bars (trade in the record that first
  reaches the seal point; seal point in the next hourly file; reconnect snapshots resending trades after the seal
  point). On the reordered-run archive live may differ; the mismatch counter counts each differing bar.

Why W = 15 s: on 4 measured days (3 BTC, 1 ETH, 6.5 M trades) M passed the end of a trade's bar by at most 1.3 s,
the only regression was 6 ms, and neither 15 s nor 60 s excludes a trade (section 12; a transit-distribution scan,
not a conformance run). 15 s is the smaller window with ten times the observed maximum as margin and absorbs the
observed clock offset (-0.14 s). A larger W only delays sealing. Open item 1 re-measures every day before the cutover.

Today's store differs: it reads until receive time passes the end of its whole build run plus 60 s
(`servermodel/FlowStore.cpp:455`, runs grouped at `:996-1032`), so a late trade's membership depends on how hours
were grouped into runs.

### Ownership

- History serves every sealed chunk, whether or not earlier hours are sealed. Availability lists the sealed runs.
- `live_from_ms` = UTC hour of (receive watermark - 60 s) - 6 h, today's retention bound
  (`servermodel/ServerDataModel.cpp:1152-1159`). Live keeps every 1m bucket at or after it and drops a bucket only
  when its hour is sealed on disk or it falls before `live_from_ms`.
- Live serves every bucket of a subscribed timeframe that intersects `[live_from_ms, now]`. A coarse bucket that
  starts before `live_from_ms` (for example today's 1d bucket) is composed from the sealed 1m bars before
  `live_from_ms` and the retained live minutes after it, as today's crossing bucket.
- The client draws a bucket from a held sealed chunk when it has one; else from live; else it is `building` (inside
  the recorded range) or `unrecorded`. Ownership never moves back into live: an unsealed hour before `live_from_ms`
  (a failed build, a store more than 6 h behind, newest-first recovery) stays `building` until it is sealed.
- A file that fails validation leaves `sealed` (its keys answer `unavailable`) and is rebuilt. A rebuild from the
  same RAWL2 gives the same bytes, so held copies stay valid.

### Epoch

An epoch names the rules and archive state behind sealed bytes, per domain: `flow` (the `.flc` files:
`flow.summary`, `flow.rows`, `trades.big`), and from step 4 `book.near` and `book.deep`.

- epoch = FNV-1a 64 of (domain name, builder rule version, W, scale rule version, big-trade floors, counter).
- The counter lives in `<flow_root>/epoch`: u64 counter, u32 count of pending days, i64 day starts, u32 CRC-32;
  always replaced atomically (temp file, fsync, rename, directory fsync).
- Triggers: a rebuild finds a sealed section whose new source fingerprint differs from its table entry (RAWL2
  changed under it), or the owner-approved RAWL2 repair procedure names days. A rule change changes the epoch by
  itself (it is in the hash).

Transition, in this order:

| Step | Action | Crash after this step |
| --- | --- | --- |
| 1 | Fence the affected days in memory: their keys leave `sealed`, requests get `unavailable`, their byte-cache entries are dropped, and replies for them still in the history pool are discarded when they finish. Frames already queued on a socket carry the old bytes with the old epoch, a consistent pair. | nothing durable changed; the next rescan finds the same mismatch and starts again |
| 2 | Write the epoch file: counter + 1, pending = the affected days | restart reads the pending list: step 6 |
| 3 | Advertise the new epoch. The byte cache is keyed by epoch, so every other entry is dropped. | step 6 |
| 4 | Rebuild the pending days and write their files | step 6 |
| 5 | Write the epoch file with an empty pending list; unfence | done |
| 6 (restart with pending days) | keep those days fenced, counter + 1 (a new epoch again), rebuild them, then step 5 | |

- New bytes are never served under the old epoch (the counter moves at step 2, before any new file exists), and old
  bytes of affected days are never served under the new epoch (fenced from step 1 to step 5).
- A missing or unreadable epoch file is recreated with counter = current Unix time in ms (far above any earlier
  counter) and every day pending: no day is served before its table fingerprints are checked against RAWL2.
- A day file lost while RAWL2 is intact is rebuilt to the same bytes; no transition is needed.
- Clients: every frame carries its epoch, availability the current one per channel. A held sealed chunk whose epoch
  differs is not drawn until revalidated: `have_hash` with its content hash; `not_modified` gives it the current
  epoch, `unavailable` keeps it hidden and retries, a chunk replaces it.
- A lost day file together with changed RAWL2 is not detected (open item 8).

## 8. Live tail

`flow_live_subscribe`, `flow_bar` and `flow_live_unsubscribe` keep today's fields and semantics (MARKETDATA.md "Live
flow bars"): revision as envelope metadata, higher revision wins, at equal revision `complete` wins. Changes:

1. Replay: on subscribe the server sends `flow_bar` for every bucket of the subscribed timeframe that intersects
   `[live_from_ms, now]` (section 7 Ownership; today: only the newest), oldest first, then one barrier:
   `{"type":"flow_live_ready","sub":<u64>,"live_from_ms":<i64>,"buckets":<count>}`. Each FLB1 carries its own grid,
   so a replay across midnight needs nothing new.
2. Admission: replay frames go through the session's normal write queue, at most 1 MiB of replay bytes queued at a
   time per subscription; the next frame is admitted when queued replay bytes drop below that. One frame is at most
   one FLB1 bar (at most 65,535 rows, about 1.6 MB of base64), so a frame always fits the 16 MiB queue. Updates to
   the series during the replay are coalesced per bucket (newest revision) and sent after the barrier. The
   slow-client disconnect policy is today's.
3. Correlation is the JSON `sub`. Re-subscribe replaces; unsubscribe or disconnect cancels the replay and discards
   queued frames. A refused subscribe (`error`, context `flow_live_subscribe`) is retried with exponential
   100 ms..5 s backoff.
4. A new subscription or connection resets revisions: at subscribe the client drops its live buckets of the series
   and rebuilds them from the replay.
5. The client replaces a live bucket with the sealed chunk once it holds one covering it, whatever the live
   bucket's revision or finality.
6. 1 s subscriptions are unchanged (RAM only, no chunks).

## 9. Client fetch

| Rule | Value |
| --- | --- |
| Wanted range | the viewport plus padding: 2 screens on each side, 4 toward the pan direction (sign of the last pan) |
| Requests | only keys inside `sealed` runs; never the live edge or the future; no edge throttles |
| Rounding | out to whole chunks of the level in use |
| Padding cap | 32 MiB of estimated bytes per chart (section 12 sizes; the decoded size once fetched); visible chunks are never capped |
| Level | the coarsest stored level dividing the timeframe; finer levels where a coarser chunk is not sealed (as `tiles::chunksFor`, `heatmap/HeatmapTiles.hpp:72`) |
| Channel | `flow.summary` for candles, volume, CVD and the footer; `flow.rows` for footprint, TPO and VP |
| Order | visible chunks newest first, then the pan side, then the other side |
| In flight | at most 8 chunks and 8 MiB of estimated bytes across all charts |
| Cache | one byte-budgeted LRU of decoded FlowChunks for all charts, 256 MiB to start; wanted keys are never evicted; survives timeframe and symbol switches |
| Draw | the viewport only |

Retry, busy backoff, deadlines and reconnect follow the heatmap fetcher (MARKETDATA.md "Process-wide chunk fetching").

## 10. Rebuild and migration

1. The server builds `.flc` files from RAWL2 in a background thread, newest day first, into a new root. FLD1 keeps
   serving FHE1 meanwhile.
2. Each day is diffed against FLD1 per 1m bar (summary and rows; notables excluded). Expected differences: bars
   whose membership changes under section 7; each is listed with the trades that moved.
3. Cutover (owner approval at the Mac): `chunk_request` serves from `.flc`, the client moves to chunks (step 5), and
   FHE1, FLB1 history, FLD1 files, `.state` sidecars, `encodeFlowDay` / `decodeFlowDay` and their tests are deleted
   in the same change. Clients start with an empty flow cache, so no epoch carries over.
4. The conductor deletes the FLD1 root after the cutover, with the owner present.

## 11. What stays as is

`flow_bar` revision semantics and FLB1 live bodies (open item 6); the 1 s live series and its journal replay;
`FlowBarBuilder` and `composeFlowBars`; the recorder grid as the row grid; quote scale 10^8.

## 12. Measurements

Run 2026-11-22 on `/Volumes/T7/sentinel-data` (read-only): `flow_history_bench --lateness <raw-l2> <symbol>
<days...>` and `flow_history_bench --columnar <hmc2-flow> <symbol> <days...>`.

### Lateness

A transit-distribution scan, not the canonical hour scan and not seal-conformance evidence: one continuous
`JournalFeed` for the day, deduplicating and ordering as the store builder does. It starts at the last journal file
opened at least an hour before the day (one to two hours of lead-in; two on these days) and stops when the running
maximum receive time is an hour past the day. All four scans are complete: no damage, no file-order regression.
"After bar end" = receive time - end of the trade's 1m bar; exclusions use the running maximum M.

| Day | Trades | Receive - exchange p50 / p99 / p99.9 / max, min | After bar end: 0-100 ms / 0.1-1 s / 1-5 s / >= 5 s | After bar end p99.9 / max (by M) | Excluded at 15 s / 60 s | Regressions | Reconnects |
| --- | --- | --- | --- | --- | --- | --- | --- |
| BTC 2026-11-19 | 1,894,339 | 67 / 393 / 868 / 12,942, -139 ms | 2,021 / 399 / 0 / 0 | 20 / 456 ms | 0 / 0 | 0 | 3 |
| BTC 2026-11-20 | 2,478,362 | 66 / 889 / 2,404 / 4,215, -58 ms | 2,159 / 698 / 1 / 0 | 7 / 1,278 ms | 0 / 0 | 0 | 8 |
| BTC 2026-11-21 | 1,695,851 | 50 / 258 / 742 / 3,345, -116 ms | 1,539 / 136 / 0 / 0 | -1 / 142 ms | 0 / 0 | 1 (6 ms) | 4 |
| ETH 2026-11-20 | 434,248 | 57 / 952 / 1,579 / 3,066, -58 ms | 320 / 180 / 0 / 0 | 17 / 880 ms | 0 / 0 | 0 | 8 |

- 99.8% of trades arrived before their bar ended. The one trade more than 1 s late (1,278 ms) came in a reconnect
  snapshot frame.
- The 6 ms regression on BTC 2026-11-21 moves no trade.
- Negative receive - exchange is clock offset between Coinbase and the capture host.
- Scan cost: 54-69 s per day.

### Sizes

"Spec" = the section 4 `flow.rows` payload, "summary" = its `flow.summary` prefix, both exactly as specified.
"Varint" (no deltas) and "fixed" (fixed width, the in-memory image) are measurement variants of the same fields.
FLB1 = today's FLB1 frames plus their 24-byte FLD1 index entries. Every chunk is compressed on its own. Every hour
present in FLD1 is taken as sealed and complete; `trades.big` is not measured. Bytes per 1m bar in brackets.

1m level, the 24 hour chunks of one day:

| Day | FLB1 | FLB1 no notables | Fixed | Varint | Spec | Spec zstd 1 / 3 | Summary |
| --- | --- | --- | --- | --- | --- | --- | --- |
| BTC 2026-11-20 (busy) | 2,179,776 (1,514) | 1,454,016 | 3,201,472 | 1,153,517 | 1,129,795 (785) | 830,431 / 819,264 | 45,217 (31.4) |
| BTC 2026-10-22 (median) | 1,429,529 (993) | 703,769 | 1,351,156 | 463,280 | 449,056 (312) | 353,036 / 352,379 | 43,781 (30.4) |
| ETH 2026-11-20 | 1,250,098 (868) | 535,858 | 712,128 | 316,098 | 306,579 (213) | 261,096 / 261,130 | 44,645 (31.0) |
| SOL 2026-11-20 | 1,196,490 (831) | 482,250 | 599,418 | 261,653 | 255,917 (178) | 215,181 / 215,205 | 43,256 (30.0) |
| PEPE 2026-10-13 (1,150 bars) | 477,597 (415) | 216,578 | 123,200 | 46,559 | 41,983 (37) | 27,692 / 27,305 | 27,021 (23.5) |

Day file (section 5: header and 50-entry table, 24 hour sections, the 1h section, the 1d piece):

| Day | FLD1 | `.flc` | of which header + table / 1h / 1d piece | Read: FLD1 decode / `.flc` parse + decode 24 hours |
| --- | --- | --- | --- | --- |
| BTC busy | 2,179,873 | 1,400,068 (1.6x smaller) | 1,340 / 206,439 / 62,494 | 4.9 / 4.0 ms |
| BTC median | 1,429,626 | 587,912 (2.4x) | 1,340 / 99,067 / 38,449 | 1.5 / 1.8 ms |
| ETH | 1,250,195 | 365,621 (3.4x) | 1,340 / 43,946 / 13,756 | 1.1 / 1.0 ms |
| SOL | 1,196,587 | 304,765 (3.9x) | 1,340 / 37,357 / 10,151 | 1.0 / 0.9 ms |
| PEPE | 477,695 | 46,942 (10.2x) | 1,341 / 2,895 / 723 | 0.34 / 0.17 ms |

Serving a 1m or 1h chunk reads its section and needs no decode. The read column was measured before the table CRC
was added (4 bytes, included above) and before loading decoded every section; with the full section 5 validation
the BTC busy day takes 6.5 ms, once per file load.

1h and 1d levels, all FLD1 days (BTC 54, others 53):

| Chunk | BTC | ETH | SOL | PEPE |
| --- | --- | --- | --- | --- |
| 1h day chunk, average spec / zstd 1 | 107 / 91 KB | 27 / 24 KB | 28 / 24 KB | 3.1 / 2.8 KB |
| 1d 16-day chunk (16 days), spec / zstd 1 | 403-410 / 348-353 KB | 99-118 / 88-104 KB | 101-119 / 89-104 KB | 7.6-12.2 / 6.8-10.9 KB |
| Summary per bar, 1h / 1d | 37 / 43 B | 37 / 42 B | 34 / 42 B | 31 / 35 B |

All 54 BTC days: 1h chunks 5.8 MB spec (4.9 MB zstd 1); the four 1d chunks 1.57 MB. Every 16-day chunk assembled
from day pieces equals the direct encoding.

Codec threshold (zstd 1 shrinks by at least 10%): `flow.rows` 1m chunks 24/24 on every measured day (15-34% saved);
1h chunks 54/54 BTC, 53/53 ETH, 52/53 SOL, 50/53 PEPE (11-15% in total); 1d chunks 11-14%, except two PEPE chunks
at 9.0% and 9.8% (sent raw).
`flow.summary` almost never pays (BTC, ETH 0/24, SOL 1/24 per day; 1h and 1d 0 of all), except PEPE 1m (24/24,
21%). zstd 3 saves at most 1.3% more than level 1.

### Rows and offsets (all FLD1 days)

| Instrument, level | Row density (rows / (max offset + 1)): total, p10, p50 | Widest offset range (day) | int16 headroom |
| --- | --- | --- | --- |
| BTC 1m | 0.755, 0.47, 0.74 | 1,408 (2026-11-13) | 23x |
| BTC 1h | 0.952, 0.88, 0.96 | 2,911 (2026-11-21) | 11x |
| BTC 1d | 0.992, 0.98, 0.99 | 5,809 (2026-11-05) | 5.6x |
| ETH 1d | 1.000 | 1,162 (2026-11-05) | 28x |
| SOL 1d | 0.999 | 1,116 (2026-10-08) | 29x |
| PEPE 1d | 1.000 | 131 (2026-10-29) | 250x |

BTC 1m averages 37.8 rows per bar. Checks over every bar: spec round trip equals FLD1 without notables, low row =
`floorDiv(low, row_tick_units)`, native tick = row tick units / price scale, day files parse back equal; 0
mismatches. No 1d composition overflowed int16.

### Big trades (from FLD1 notables)

Sweep-collapsed events at or above a floor, per day. Notables keep the top 8 per minute, so these are lower
bounds; "truncated" counts minutes whose eighth notable was at or above the floor. Proposed floors in bold
(about 50-500 events a day each; the owner decides, open item 2).

| Floor | BTC | ETH | SOL | PEPE |
| --- | --- | --- | --- | --- |
| $10,000 | 5,908 (22,255 truncated) | 3,334 | 1,020 | **51** (11) |
| $25,000 | 3,030 | 1,322 | **287** (170) | 15 |
| $50,000 | 1,567 | **344** (170) | 110 | 5.3 |
| $100,000 | **500** (324) | 98 | 40 | 1.4 |
| $250,000 | 98 | 23 | 8.6 | 0.08 |
| $1,000,000 | 7.3 | 1.2 | 0.4 | 0 |

Not measured: an event should take 15-20 bytes, so 500 a day is about 9 KB.

## 13. Open items

1. **Lateness on every day.** Four days were measured. Before the cutover, run `--lateness` over every recorded day
   and product (about a minute per day) and check that no day excludes a trade at 15 s. A conformance run of the
   canonical hour scan itself is the step 3 builder's job.
2. **Big-trade floors** (owner decision: what bubbles show far back). Section 12 proposes floors.
3. **Days without a recorder grid** (plan question 4). Live drops their trades
   (`sentinel_flow_missing_grid_trades_total`); the store has a journal grid fallback (`dayScales`,
   `servermodel/FlowStore.cpp:342-412`). Live needs the same before chunks replace FHE1.
4. **Server memory** (plan question 5): profile the ~877 MB before step 3 adds the sealed-frame cache.
5. **Startup gap.** After a restart live holds only buckets since its first live trade; earlier unsealed hours are
   `building` until sealed. Startup replay of the open hour (plan step 6) closes it.
6. **Live message format.** `flow_bar` stays JSON with FLB1 (rows always included). A summary-only live level of
   detail, binary live frames and dropping FLB1 notables when `trades.big` ships belong to the live rework.
7. **Heatmap on SCK1 (step 4).** The mapping in section 3 is fixed. Not settled, because it needs the step 4 code:
   which HMC2 repair tools run the section 7 epoch transition for `book.*` (today they delete rollup files,
   MARKETDATA.md "Changed content"), and the exact `book.*` kind 2 payload layout.
8. **Lost day file and changed RAWL2 together.** The epoch cannot see it (the old fingerprint is gone). RAWL2 is
   append-only and rewritten only by owner-approved repairs, which bump the epoch; an unannounced change of RAWL2
   bytes is outside the model.
9. **1w and other multi-day timeframes.** They mix grids, and `composeFlowBars` refuses that. They need a
   mixed-grid rule (common price scale, output row tick, base-scale conversion), not specified here.
10. **Kalshi.** Tick bands, not one tick; the flow row grid is defined only for one recorder tick.
11. **Coarser rows** (`price_step` > 1): composition rule and prebuilding not specified.
