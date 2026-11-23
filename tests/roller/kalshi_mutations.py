#!/usr/bin/env python3
"""Run the WHOLE script inside one build-queue invocation; never queue nested builds.

The tree must be clean. Each isolated regression must fail its test; restore,
rebuild and pass the same test before proceeding. No network or production data.
"""
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / "build/mac-clang"
DECODER = "libs/core/roller/KalshiDecoder.cpp"
FEED = "libs/core/roller/JournalFeed.cpp"
READER = "libs/core/roller/JournalReader.cpp"
REAL = "KalshiFeed.RealCaptureEveryBookSequenceStepAndTrade"
BAD = "KalshiFeed.MalformedBookFramesInvalidateWithoutPartialCallbacks"
STATE = "KalshiFeed.StateRoundTripAtEveryCaptureBoundaryPreservesCallbacks"
CASES = [
    ("K0 reader refusal (a-f,h)", READER,
     '            const auto& scan = reader_->result();',
     '            if (record.kind == capture::Kind::Frame && f.venue == Venue::Kalshi)\n'
     '                throw std::runtime_error("Kalshi journal payload decoding is not supported");\n'
     '            const auto& scan = reader_->result();', "KalshiFeed.*"),
    ("ignore sequenced ok (a,b)", DECODER,
     'if (book || frame.contains("seq")) advance(frame);', 'if (book) advance(frame);', REAL),
    ("ignore sequence integrity (b,c)", DECODER,
     'if (broken) throw', 'if (false && broken) throw',
     "KalshiFeed.DroppedOkDeltaDuplicateAndReorderInvalidateUntilSnapshot"),
    ("emit signed change instead of absolute (a)", DECODER,
     'const auto q = old + delta;', 'const auto q = delta;', REAL),
    ("permit negative quantities (d)", DECODER,
     '(delta < 0 && old < -delta)', '(false && delta < 0 && old < -delta)', BAD),
    ("accept malformed fixed-point syntax (d)", DECODER,
     'throw std::runtime_error("malformed Kalshi fixed point");\n    int64_t n = 0;',
     'return 0;\n    int64_t n = 0;', BAD),
    ("accept out-of-range price (d)", DECODER,
     'if (p > 10000) throw', 'if (false && p > 10000) throw', BAD),
    ("invalidate every waiting delta (1)", DECODER,
     'if (type == "orderbook_delta" && !valid_) return {};',
     'if (type == "orderbook_delta" && !valid_) throw std::runtime_error("delta before snapshot");',
     "KalshiFeed.WaitingForSnapshotIgnoresDeltasWithoutRepeatedInvalidation"),
    ("drop selected snapshot on unannounced sid (2)", DECODER,
     'if (ours && type == "orderbook_snapshot") {', 'if (false && ours && type == "orderbook_snapshot") {',
     "KalshiFeed.UnannouncedSidSnapshotsAdoptAndDeltasInvalidateOnce"),
    ("silently ignore selected delta on wrong sid (2)", DECODER,
     'if (ours && valid_) throw', 'if (false && ours && valid_) throw',
     "KalshiFeed.UnannouncedSidSnapshotsAdoptAndDeltasInvalidateOnce"),
    ("malformed trade invalidates book (3)", FEED,
     'if (!tradeFrame) invalid(local, e.what());', 'invalid(local, e.what());',
     "KalshiFeed.TradeSideAlternativesAndMalformedFields"),
    ("remove legacy trade side fallback (3)", DECODER,
     'if (!buy) read("taker_side", "yes", "no");', 'if (false && !buy) read("taker_side", "yes", "no");',
     "KalshiFeed.LegacyTradeSideIsUsedOnlyWithoutModernFields"),
    ("reject authoritative snapshot sequence discontinuity (4)", DECODER,
     'sequence_ = integer(frame.at("seq"));', 'if (sid_ == sid) advance(frame); sequence_ = integer(frame.at("seq"));',
     "KalshiFeed.SelectedSnapshotsReanchorSequenceAndRecoverRetiredSubscriptions"),
    ("lose typed header venue (5)", READER,
     'f.venue, *f.instrument};',
     'Venue::Coinbase, *f.instrument};',
     "KalshiFeed.ReaderPreservesOpaqueMetadataAndExposesTypedHeaderIdentity"),
    ("lose header instrument (5)", READER,
     'f.venue, *f.instrument};',
     'f.venue, json()};',
     "KalshiFeed.ReaderPreservesOpaqueMetadataAndExposesTypedHeaderIdentity"),
    ("rewrite opaque product metadata (5)", READER,
     'out = {std::move(record), std::move(pos), *f.metadata,',
     'out = {std::move(record), std::move(pos), json::object(),',
     "KalshiFeed.ReaderPreservesOpaqueMetadataAndExposesTypedHeaderIdentity"),
    ("allow Kalshi trade-only history (6)", "libs/core/trading/MarketEventSource.cpp",
     'if (files[i].venue == sentinel::Venue::Kalshi)',
     'if (false && files[i].venue == sentinel::Venue::Kalshi)',
     "BacktestCore.KalshiJournalTradeHistoryRefusesInsteadOfReturningEmpty"),
    ("mix venues across state import (8)", FEED,
     'if (venue != venue_) {', 'if (false && venue != venue_) {',
     "KalshiFeed.VenueChangesInvalidateIncludingAfterStateImport"),
    ("floating-point complement (e)", DECODER,
     'units(bid ? p : 10000 - p, 10000)',
     '(bid ? units(p, 10000) : 1.0 - units(p, 10000))',
     "KalshiFeed.EveryYesAndComplementPriceHasIdenticalBits"),
    ("flip aggressor (f)", DECODER,
     'return *buy ? AggressorSide::Buy : AggressorSide::Sell;',
     'return *buy ? AggressorSide::Sell : AggressorSide::Buy;', REAL),
    ("alter Coinbase state bytes (g)", FEED,
     '    return state;', '    state["unexpected"] = true;\n    return state;',
     "CoinbaseFeedState.CapturedFixturesMatchBaseBytes"),
    ("lose restored quantities (h)", DECODER,
     'q == 0 || !levels.emplace(p, q).second', 'q == 0 || !levels.emplace(p, 1).second', STATE),
    ("lose restored sequence (h)", DECODER,
     '    return out;\n}\n} // namespace sentinel::roller',
     '    out.sequence_.reset();\n    return out;\n}\n} // namespace sentinel::roller', STATE),
]


def run(command):
    return subprocess.run(command, cwd=ROOT, stdin=subprocess.DEVNULL,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          text=True, timeout=600)


def executable(test):
    if test.startswith("BacktestCore."):
        return "trading", "test_backtest_core"
    return "roller", "test_coinbase_feed_state" if test.startswith("Coinbase") else "test_kalshi_feed"


def build(test):
    result = run(["cmake", "--build", "--preset", "mac-clang", "-j", "6", "--target", executable(test)[1]])
    if result.returncode:
        print(result.stdout, flush=True)
        raise RuntimeError("mutation build failed")


def check(test, should_pass):
    directory, exe = executable(test)
    result = run([str(BUILD / "tests" / directory / exe), "--gtest_filter=" + test])
    # An assertion failure proves the regression; a crash/timeout is not a kill.
    passed = result.returncode == 0
    if passed != should_pass or (not should_pass and "[  FAILED  ]" not in result.stdout):
        print(result.stdout, flush=True)
        raise RuntimeError("unexpected mutation result: " + test)
    failed = [line.strip() for line in result.stdout.splitlines() if line.startswith("[  FAILED  ]")]
    if failed:
        print("  " + "; ".join(failed[:3]), flush=True)


def main():
    status = run(["git", "status", "--porcelain", "--untracked-files=normal"])
    if status.returncode or status.stdout.strip():
        raise RuntimeError("mutation runner requires a clean tree; commit or remove changes first")
    os.environ["CCACHE_DIR"] = str(BUILD / ".ccache")
    for name, path, before, after, test in CASES:
        file = ROOT / path
        original = file.read_text()
        if original.count(before) != 1:
            raise RuntimeError("mutation match is not unique: " + name)
        print("MUTATION " + name, flush=True)
        try:
            file.write_text(original.replace(before, after))
            build(test)
            check(test, False)
        finally:
            file.write_text(original)
            build(test)
        check(test, True)
        print("KILLED + RESTORED PASS " + name, flush=True)
    print(f"kalshi mutations: {len(CASES)}/{len(CASES)} killed; restored checks passed", flush=True)


if __name__ == "__main__":
    main()
