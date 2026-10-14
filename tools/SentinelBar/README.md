# SentinelBar

Native macOS menu bar panel for agents, the active plan, build queue, milestones,
services and budget. It reads local data and writes only its own settings.

Build and install: `tools/SentinelBar/build-app.sh --install`. Development builds
and tests run through `scripts/dev/build-queue.sh`. The repo, research directory and
optional pinned plan are set in the panel's Settings sheet (`com.sentinel.bar` UserDefaults).
Plans come from the private sibling repository, `../Sentinel-Private/research`, by default.
Set Research directory for a different clone location; a relative directory is resolved
from the configured Sentinel checkout. Plan selection still uses the files named by Git
trailers, or the pinned file; it does not scan all private notes. Historical trailer paths
identify the corresponding plan under the private research directory, including subfolders.
Pinned plans may be relative to that directory or an explicit absolute path. A missing private
checkout leaves the Plan panel empty. Selected plan contents refresh every ten seconds,
including private edits that do not change Sentinel's main commit.
Tests use Swift Testing because this Mac's Command Line Tools do not include XCTest. Run them with `tools/SentinelBar/test.sh` (it adds the framework search paths SwiftPM misses).
