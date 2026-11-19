#!/usr/bin/env python3
"""Run through the build queue. Each isolated regression must fail its test;
restore, rebuild and pass the same test before proceeding. No network or data.
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
BAD = "KalshiFeed.MalformedAndUnanchoredFramesInvalidateWithoutPartialCallbacks"
STATE = "KalshiFeed.StateRoundTripAtEveryCaptureBoundaryPreservesCallbacks"
CASES = [
    ("K0 reader refusal (a-f,h)", READER,
     '            const auto& scan = reader_->result();',
     '            if (record.kind == capture::Kind::Frame && venueFromHeader(f.header) == Venue::Kalshi)\n'
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
    ("allow delta without snapshot (d)", DECODER,
     'if (!valid_) throw', 'if (false && !valid_) throw', BAD),
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


def build():
    result = run(["cmake", "--build", "--preset", "mac-clang", "-j", "6", "--target",
                  "test_kalshi_feed", "test_coinbase_feed_state"])
    if result.returncode:
        print(result.stdout, flush=True)
        raise RuntimeError("mutation build failed")


def check(test, should_pass):
    exe = "test_coinbase_feed_state" if test.startswith("Coinbase") else "test_kalshi_feed"
    result = run([str(BUILD / "tests/roller" / exe), "--gtest_filter=" + test])
    # An assertion failure proves the regression; a crash/timeout is not a kill.
    passed = result.returncode == 0
    if passed != should_pass or (not should_pass and "[  FAILED  ]" not in result.stdout):
        print(result.stdout, flush=True)
        raise RuntimeError("unexpected mutation result: " + test)
    failed = [line.strip() for line in result.stdout.splitlines() if line.startswith("[  FAILED  ]")]
    if failed:
        print("  " + "; ".join(failed[:3]), flush=True)


def main():
    os.environ["CCACHE_DIR"] = str(BUILD / ".ccache")
    for name, path, before, after, test in CASES:
        file = ROOT / path
        original = file.read_text()
        if original.count(before) != 1:
            raise RuntimeError("mutation match is not unique: " + name)
        print("MUTATION " + name, flush=True)
        try:
            file.write_text(original.replace(before, after))
            build()
            check(test, False)
        finally:
            file.write_text(original)
            build()
        check(test, True)
        print("KILLED + RESTORED PASS " + name, flush=True)
    print(f"kalshi mutations: {len(CASES)}/{len(CASES)} killed; restored checks passed", flush=True)


if __name__ == "__main__":
    main()
