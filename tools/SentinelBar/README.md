# SentinelBar

Native macOS menu bar panel for agents, the active plan, build queue, milestones,
services and budget. It reads local data and writes only its own settings.

Build and install: `tools/SentinelBar/build-app.sh --install`. Development builds
and tests run through `scripts/dev/build-queue.sh`. The repo and optional pinned
plan are set in the panel's Settings sheet (`com.sentinel.bar` UserDefaults).
Tests use Swift Testing because this Mac's Command Line Tools do not include XCTest. Run them with `tools/SentinelBar/test.sh` (it adds the framework search paths SwiftPM misses).
