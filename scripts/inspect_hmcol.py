#!/usr/bin/env python3
"""
inspect_hmcol.py — dump headers, slot stats, and per-slot summaries for one or
more .hmcol files. Read-only; safe to run against a live server's data dir.

Format reference: libs/core/servermodel/HmcolFormat.hpp (HMCL v1).

Usage:
    python3 scripts/inspect_hmcol.py path/to/file.hmcol [more.hmcol ...]
    python3 scripts/inspect_hmcol.py data/heatmap/BTC-USD/60000/v1/2026-05-08.hmcol
    python3 scripts/inspect_hmcol.py --slots 0,5,1437 file.hmcol
    python3 scripts/inspect_hmcol.py --verify file.hmcol     # CRC every record
"""

from __future__ import annotations

import argparse
import binascii
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

MAGIC = 0x484D434C  # 'HMCL' little-endian
VERSION = 1
INTENSITY_FORMAT_U16 = 1
LIQUIDITY_FORMAT_NONE = 0
LIQUIDITY_FORMAT_U16 = 1
FLAG_HAS_LIQUIDITY = 1 << 0

# struct FileHeader { u32 magic; u16 version; u16 _; i64 timeframeMs;
#                     i32 gridHeight; u8 intensityFormat; u8 liquidityFormat;
#                     u16 _; i64 dayStartMs; char symbol[24];
#                     u32 headerCrc32; u32 _; }  -> 64 bytes
FILE_HEADER_FMT = "<IHHqiBBHq24sII"
FILE_HEADER_SIZE = struct.calcsize(FILE_HEADER_FMT)
assert FILE_HEADER_SIZE == 64, FILE_HEADER_SIZE

# struct RecordHeader { i64 bucketStartMs; i64 bucketEndMs; double minPrice;
#                       double maxPrice; double tickSize; double liquidityScale;
#                       u32 flags; u32 recordCrc32; }  -> 56 bytes
RECORD_HEADER_FMT = "<qqddddII"
RECORD_HEADER_SIZE = struct.calcsize(RECORD_HEADER_FMT)
assert RECORD_HEADER_SIZE == 56, RECORD_HEADER_SIZE

# Header CRC covers all bytes preceding the headerCrc32 field (offset 56 of 64).
HEADER_CRC_SPAN = 56


@dataclass
class FileHeader:
    magic: int
    version: int
    timeframe_ms: int
    grid_height: int
    intensity_format: int
    liquidity_format: int
    day_start_ms: int
    symbol: str
    header_crc32: int
    raw: bytes  # leading HEADER_CRC_SPAN bytes for verification

    def verify(self) -> bool:
        if self.magic != MAGIC or self.version != VERSION:
            return False
        return binascii.crc32(self.raw) == self.header_crc32


def read_file_header(buf: bytes) -> FileHeader:
    if len(buf) < FILE_HEADER_SIZE:
        raise ValueError(f"file shorter than header ({len(buf)} bytes)")
    fields = struct.unpack(FILE_HEADER_FMT, buf[:FILE_HEADER_SIZE])
    (magic, version, _r0, tf_ms, grid_h, ifmt, lfmt, _r1,
     day_start, symbol, hdr_crc, _r2) = fields
    sym = symbol.split(b"\x00", 1)[0].decode("utf-8", errors="replace")
    return FileHeader(
        magic=magic, version=version, timeframe_ms=tf_ms, grid_height=grid_h,
        intensity_format=ifmt, liquidity_format=lfmt, day_start_ms=day_start,
        symbol=sym, header_crc32=hdr_crc, raw=buf[:HEADER_CRC_SPAN],
    )


def record_stride(grid_height: int, liquidity_format: int) -> int:
    intensity_bytes = grid_height * 2
    liquidity_bytes = grid_height * 2 if liquidity_format == LIQUIDITY_FORMAT_U16 else 0
    return RECORD_HEADER_SIZE + intensity_bytes + liquidity_bytes


def slots_per_day(timeframe_ms: int) -> int:
    return 86_400_000 // timeframe_ms


def fmt_ms(ms: int) -> str:
    """Render a UTC timestamp without pulling in datetime tz juggling."""
    if ms == 0:
        return "(empty)"
    sec, sub = divmod(ms, 1000)
    days, rem = divmod(sec, 86_400)
    h, rem = divmod(rem, 3600)
    m, s = divmod(rem, 60)
    # Just return ISO-ish; consumers can convert if they want.
    return f"{ms} ({h:02d}:{m:02d}:{s:02d}.{sub:03d} day-of-epoch={days})"


def parse_slot_spec(spec: str, total: int) -> list[int]:
    out: list[int] = []
    for token in spec.split(","):
        token = token.strip()
        if not token:
            continue
        if "-" in token:
            a, b = token.split("-", 1)
            out.extend(range(int(a), int(b) + 1))
        else:
            out.append(int(token))
    return [s for s in out if 0 <= s < total]


def inspect(path: Path, slot_spec: str | None, verify: bool, max_dump: int) -> int:
    data = path.read_bytes()
    print(f"\n=== {path} ===")
    print(f"  size={len(data):,} bytes")

    try:
        header = read_file_header(data)
    except ValueError as e:
        print(f"  ! cannot read header: {e}")
        return 1

    crc_ok = header.verify()
    print(f"  magic=0x{header.magic:08X} version={header.version} crc_ok={crc_ok}")
    print(f"  symbol={header.symbol!r}")
    print(f"  timeframeMs={header.timeframe_ms} gridHeight={header.grid_height}")
    print(f"  intensityFormat={header.intensity_format} liquidityFormat={header.liquidity_format}")
    print(f"  dayStartMs={header.day_start_ms}")

    if not crc_ok:
        print("  ! header CRC invalid — refusing to walk records")
        return 1

    stride = record_stride(header.grid_height, header.liquidity_format)
    total_slots = slots_per_day(header.timeframe_ms)
    expected_size = FILE_HEADER_SIZE + total_slots * stride
    print(f"  recordStride={stride} slotsPerDay={total_slots} expectedSize={expected_size:,}")

    if len(data) < expected_size:
        print(f"  ! file is shorter than full-day expected size by "
              f"{expected_size - len(data):,} bytes")

    populated = 0
    crc_failures = 0
    earliest = None
    latest = None

    for slot in range(total_slots):
        off = FILE_HEADER_SIZE + slot * stride
        hdr = data[off:off + RECORD_HEADER_SIZE]
        if len(hdr) < RECORD_HEADER_SIZE:
            break
        bucket_start = struct.unpack_from("<q", hdr, 0)[0]
        if bucket_start == 0:
            continue
        populated += 1
        earliest = bucket_start if earliest is None else min(earliest, bucket_start)
        latest = bucket_start if latest is None else max(latest, bucket_start)

        if verify:
            (bs, be, mn, mx, tk, ls, flags, rcrc) = struct.unpack(RECORD_HEADER_FMT, hdr)
            payload_len = header.grid_height * 2
            if flags & FLAG_HAS_LIQUIDITY:
                payload_len += header.grid_height * 2
            payload = data[off + RECORD_HEADER_SIZE:off + RECORD_HEADER_SIZE + payload_len]
            if binascii.crc32(payload) != rcrc:
                crc_failures += 1

    print(f"  populated={populated}/{total_slots}  empty={total_slots - populated}")
    if populated:
        print(f"  earliestBucketMs={earliest}  latestBucketMs={latest}")
    if verify:
        print(f"  recordCrcFailures={crc_failures}")

    if slot_spec:
        slots = parse_slot_spec(slot_spec, total_slots)
        for slot in slots[:max_dump]:
            off = FILE_HEADER_SIZE + slot * stride
            hdr = data[off:off + RECORD_HEADER_SIZE]
            (bs, be, mn, mx, tk, ls, flags, rcrc) = struct.unpack(RECORD_HEADER_FMT, hdr)
            print(f"  -- slot {slot} @ off=0x{off:08X}")
            print(f"     bucketStartMs={fmt_ms(bs)}")
            print(f"     bucketEndMs  ={fmt_ms(be)}")
            print(f"     minPrice={mn:.6f}  maxPrice={mx:.6f}  tickSize={tk}")
            print(f"     liquidityScale={ls}  flags=0x{flags:08X}  recordCrc32=0x{rcrc:08X}")
            if bs != 0:
                payload_len = header.grid_height * 2
                if flags & FLAG_HAS_LIQUIDITY:
                    payload_len += header.grid_height * 2
                payload = data[off + RECORD_HEADER_SIZE:off + RECORD_HEADER_SIZE + payload_len]
                actual = binascii.crc32(payload)
                ok = actual == rcrc
                print(f"     payload crc match={ok} (computed=0x{actual:08X})")

    return 0 if crc_failures == 0 else 1


def main() -> int:
    p = argparse.ArgumentParser(description="Dump and verify HMCL files")
    p.add_argument("paths", nargs="+", type=Path)
    p.add_argument("--slots", help="comma/range slot spec to dump, e.g. '0,5,1430-1439'")
    p.add_argument("--verify", action="store_true",
                   help="recompute every record CRC and report failures")
    p.add_argument("--max-dump", type=int, default=20,
                   help="cap the number of slots dumped per file (default: 20)")
    args = p.parse_args()

    rc = 0
    for path in args.paths:
        if path.is_dir():
            for f in sorted(path.rglob("*.hmcol")):
                rc |= inspect(f, args.slots, args.verify, args.max_dump)
        else:
            rc |= inspect(path, args.slots, args.verify, args.max_dump)
    return rc


if __name__ == "__main__":
    sys.exit(main())
