# CopeNet dock boundary and microstructure context

Read-only investigation of `/Users/copeharder/Programming/Sentinel` on 2026-10-03. Paths and line numbers below are relative to that repository. This document proposes an interface; it does not change Sentinel.

## 1. Commentary dependency map and decision

The 2026-10 audit proposed deleting the whole commentary family because it found no producer (`docs/research/2026-10-widget-audit.md:104-109,134`). The later owner directive explicitly keeps CopeNet, permits removing the AI sibling, and requires a model-facing data boundary (`docs/research/2026-10-widget-pass-directive.md:112-125`). The later instruction governs this decision.

| Concern | Current relationship and evidence |
|---|---|
| Inheritance/shared UI | `CommentaryFeedDock` derives from `DockablePanel`, owns the read-only `QTextEdit`, `appendMessage(source,text)`, and a line-based 1,000-line prune (`libs/gui/widgets/CommentaryFeedDock.hpp:13-32`, `.cpp:6-50`). Both `CopenetFeedDock` and `AICommentaryFeedDock` derive directly from that base and override the same method (`libs/gui/widgets/CopenetFeedDock.hpp:3-16`; `libs/gui/widgets/AICommentaryFeedDock.hpp:3-16`). The base is not instantiated as a standalone dock (`libs/gui/mainwindow/DockFactory.cpp:23-26`). |
| Message behavior | CopeNet sets object name `CopenetFeedDock`, title `COPENET`, cyan style, and prints local `HH:mm` plus **text only**; it ignores `source` (`libs/gui/widgets/CopenetFeedDock.cpp:6-27`). AI sets `AICommentaryFeedDock`, title `AI Commentary`, magenta style, ignores `source`, and guesses a symbol from any hyphen in text (`libs/gui/widgets/AICommentaryFeedDock.cpp:7-41`). Both call the base prune; neither calls the base `appendMessage`. |
| Signals/models | These classes declare `Q_OBJECT` but no feed-specific signals or data model (`libs/gui/widgets/{CommentaryFeedDock,CopenetFeedDock,AICommentaryFeedDock}.hpp`). They inherit `QDockWidget` visibility/toggle behavior through `DockablePanel`, whose object name is the persistent Qt layout ID (`libs/gui/widgets/DockablePanel.cpp:4-16`). There is no typed message DTO, symbol field, transport subscription, or feed connection in the three classes. |
| Construction/wiring | Factory constructs the two siblings independently (`libs/gui/mainwindow/DockFactory.cpp:23-26`; `.h:21-36`). `MainWindowGPU` stores both pointers and copies them to menu/layout structs (`libs/gui/MainWindowGpu.h:35-36,153-154`; `.cpp:288-296,1165-1177,1593-1605`). Its `symbolChanged` connections target SEC, order book, and paper trading, not either commentary dock (`libs/gui/MainWindowGpu.cpp:315-338,957-971`). |
| Menu/layout | Each toggle action enters View independently (`libs/gui/mainwindow/MenuBuilder.h:22-38`; `.cpp:42-47`). Layout DTO has independent pointers (`libs/gui/mainwindow/LayoutOrchestrator.h:22-37`); API IDs are `copenet` and `aiCommentary` (`.cpp:24-30`); default layout puts CopeNet in the bottom area, tabs AI onto it, then hides both (`.cpp:149-157,217-220`). Remove/constraint paths reference both separately (`.cpp:100-101,183-184`). |
| API/settings | `MainWindowGPU` registers `apiDocks()` with `DockVisibilityController` (`libs/gui/MainWindowGpu.cpp:223-226`). Its one settings key is `agentApi/docks/visible`, containing a map by API ID; restore filters unknown IDs (`libs/gui/mainwindow/DockVisibilityController.cpp:8-9,89-115`). Saved named and `_last_session` layouts use `layouts/<name>/version` and `/state` as Qt `saveState()` bytes, keyed by dock object name (`libs/gui/widgets/LayoutManager.cpp:6-20,23-59`; `libs/gui/MainWindowGpu.cpp:1133-1160`). `APP_LAYOUT_VERSION` is 4 (`libs/gui/widgets/LayoutManager.hpp:13`). There is no separate AI or CopeNet content setting. |
| Build/tests/docs | GUI CMake lists all three `.cpp` files (`libs/gui/CMakeLists.txt:154-156`). `docs/AGENT_API.md:38-40` documents both API IDs and dock persistence. `tests/agentapi/test_AgentApiDocks.cpp:66-108,275-300` exercises the generic dock API and stale-ID restore with synthetic docks, not these concrete feed classes. Repository search of `libs`, `apps`, and `tests` found `appendMessage` only in the three feed declarations/definitions; no caller, producer, or CopeNet/AI feed test. This negative finding is limited to the in-tree repository; an external CopeNet project was not audited. |

**Recommendation: remove `AICommentaryFeedDock` only.** It has no in-tree producer, unique model, transport, or symbol workflow. Its only strategic distinction is a themed duplicate; the owner directive reserves the AI product role for CopeNet (`docs/research/2026-10-widget-pass-directive.md:112-125`). Keep `CopenetFeedDock` and `CommentaryFeedDock` because CopeNet still inherits and calls the base UI/prune contract. An external consumer may know the `aiCommentary` dock API ID; its removal is an intentional API change, not a data-path dependency of CopeNet.

**Exact removal/edit list for a future code patch:** delete `libs/gui/widgets/AICommentaryFeedDock.{hpp,cpp}`; remove its source entry from `libs/gui/CMakeLists.txt:154`; remove only AI include, pointer assignment, menu/layout transfer, and member/forward declaration in `libs/gui/MainWindowGpu.{cpp,h}`; remove only AI include, forward declaration, struct field, and construction in `libs/gui/mainwindow/DockFactory.{cpp,h}`; remove only AI include, struct field, and toggle in `libs/gui/mainwindow/MenuBuilder.{cpp,h}`; remove only AI include, struct field, `aiCommentary` API pair, remove/add/tab/constraint/hide branches in `libs/gui/mainwindow/LayoutOrchestrator.{cpp,h}`. Update the ID list in `docs/AGENT_API.md:38`. The generic `AgentApiDocks` tests need no mandatory ID edit; add a concrete-window assertion for `copenet` present and `aiCommentary` absent if that window is readily testable. Leave `CommentaryFeedDock.{hpp,cpp}`, `CopenetFeedDock.{hpp,cpp}`, their CMake entries, and all `copenet` registrations intact. Saved `agentApi/docks/visible` maps can retain an old `aiCommentary` entry because restore filters unknown IDs; saved Qt layout state deserves a compatibility check with a layout containing the deleted object name before changing `APP_LAYOUT_VERSION` (`DockVisibilityController.cpp:111-115`; `LayoutManager.cpp:30-59`).

## 2. CopeNet today

`CopenetFeedDock` expects an in-process call `appendMessage(const QString& source, const QString& text)` on a QWidget. It displays **only `text`** with a local minute timestamp, so the source identity and any structured symbol/time/provenance supplied separately would be lost (`libs/gui/widgets/CopenetFeedDock.hpp:13-16`; `.cpp:15-27`). It offers no network listener, stream decode, queue, source freshness, or producer connection. `MainWindowGPU` constructs/registers the dock but has no `symbolChanged` linkage to it (`libs/gui/MainWindowGpu.cpp:295,315-338`). A hyphen in AI sibling text is merely a display heuristic, not a CopeNet symbol contract (`libs/gui/widgets/AICommentaryFeedDock.cpp:19-33`). The external JS/Python CopeNet project mentioned by the directive is outside this in-tree audit. A future feed needs an explicit message DTO (symbol, observation time, source/provenance, text, severity/id), a bounded and authenticated delivery path into the GUI, selection/epoch handling, and a deliberate interaction for symbol-linked entries. Read-only market context is a separate interface from that eventual output delivery.

## 3. Proposed read-only microstructure context boundary

### Ownership and transport

- **Canonical computation:** put GUI-independent DTOs, bounded rolling reducers, validation, and serialization in `libs/core`; instantiate/publish them in `sentinel-server` beside `ServerDataModel`. This follows core's market-data/DTO ownership and keeps network/data work off the GUI thread (`AGENTS.md:29-59`; `docs/ARCHITECTURE.md:18-33,47-73`). The server already owns live books, trade samples, recorder availability/watermarks, and stream publication (`libs/core/servermodel/ServerDataModel.hpp:46-85,89-110`; `libs/core/protocol/SentinelStreamServer.cpp:794-818`). An optional CopeNet sidecar calls the boundary and handles model prompts; it does not own the canonical calculations.
- **Expose a new, narrowly scoped read-only server HTTP endpoint**, e.g. `GET /api/v1/microstructure/context?symbol=BTC-USD&windowMs=60000`, with a versioned JSON response. Do not overload `GET /metrics`: that listener currently serves Prometheus operational counters and `/ping`, on the server main thread, with no auth (`libs/core/metrics/MetricsHttpServer.hpp:14-25`; `apps/sentinel-server/SentinelServerApp.cpp:45-57`). The existing Sentinel WebSocket transports trade, book updates, and encoded heatmap slices at market/render cadence; it is useful as an input to the server reducer, not as an LLM payload (`docs/ARCHITECTURE.md:47-73,316-328`; `libs/core/protocol/SentinelStreamServer.cpp:794-818,1529-1558`).
- **Keep the current GUI Agent API as a bridge/validation source, not the canonical model boundary.** It already exposes `/state`, `/book`, `/trades`, and `/heatmap/walls` on `127.0.0.1:17100` (`docs/AGENT_API.md:3-26`). Its book is a band-limited GUI replica, trade tape is receive-time and capped at 10,000 rows/15 minutes, and walls scan client-held recording data (`docs/AGENT_API.md:140-156`). Reads run on the GUI thread (`docs/AGENT_API.md:32`); depending on that process would tie headless CopeNet to the workstation's current selection/cache. Its `/state` can optionally identify the owner's active instrument; the server context request must still take an explicit symbol and never infer “active” from a GUI object.
- **Delivery contract:** bounded snapshot on request; optional low-rate `context_changed` notification later that contains symbol, version, and changed-section names, prompting a fresh query. Do not stream every book/trade event to the model. Selective historical queries may later read HMC2/RAWL2 through bounded workers. Use separate freshness and coverage per section, not one misleading global “fresh” flag.

### First schema (v1 proposal)

Times are UTC epoch milliseconds; prices and sizes retain explicit units/precision. `null` means unknown, never zero. Every quantitative section carries `asOfMs`, `windowMs` where relevant, `coverage` (`complete|partial|unknown`), `source`, and `truncated`/`stale` flags. Preserve input basis (`exchange` versus `receive`) and provenance so model text cannot silently upgrade weak evidence to certainty (`docs/research/2026-09-agent-api-v1-spec.md:84-89`).

```json
{
  "schemaVersion": 1,
  "symbol": "BTC-USD",
  "observedAtMs": 1791028800000,
  "selection": {"requestedSymbol": "BTC-USD", "guiActiveSymbol": null},
  "status": {"connected": true, "bookValid": true, "stale": false,
             "coverage": "partial", "reasons": ["book-band-limited"]},
  "instrument": {"venue": "coinbase", "base": "BTC", "quote": "USD",
                 "priceTick": 0.01, "sizeUnit": "BTC"},
  "topOfBook": {"bid": {"price": 65000.00, "size": 1.2},
                "ask": {"price": 65000.01, "size": 0.9},
                "mid": 65000.005, "spread": 0.01, "spreadBps": 0.00154,
                "asOfMs": 1791028799950, "source": "live-book"},
  "depth": {"rangeBps": 25, "bands": [{"fromBps": 0, "toBps": 5,
             "bidSize": 12.4, "askSize": 11.1, "bidNotionalQuote": 806000,
             "askNotionalQuote": 722000}], "bandLimited": true,
             "asOfMs": 1791028799950, "coverage": "partial"},
  "liquidity": {"windowMs": 300000, "heatmapBasis": "recording-twap-sum",
                "hotspots": [{"side": "bid", "priceLow": 64950,
                  "priceHigh": 64960, "peakSize": 30, "meanSize": 18,
                  "firstSeenMs": 1791028500000, "lastSeenMs": 1791028740000}],
                "coverage": "partial", "source": "hmc2.near"},
  "trades": {"windowMs": 60000, "timeBasis": "exchange",
             "buyCount": 120, "sellCount": 90, "buySize": 8.2,
             "sellSize": 6.4, "buyNotionalQuote": 533000,
             "sellNotionalQuote": 416000, "lastTradePrice": 65000.01,
             "sideBasis": "aggressor", "coverage": "complete"},
  "pressure": {"depthImbalance": 0.07, "tradeDeltaSize": 1.8,
               "windowMs": 60000, "interpretation": "descriptive"},
  "structure": {"windowMs": 300000,
                "changes": [{"kind": "wall_pulled", "side": "ask",
                 "priceLow": 65050, "priceHigh": 65060,
                 "beforeSize": 40, "afterSize": 5,
                 "firstSeenMs": 1791028500000, "detectedAtMs": 1791028780000,
                 "confidence": "observed-aggregate-change"}]},
  "rolling": {"windowsMs": [10000, 60000, 300000],
              "spreadP50Bps": 0.8, "spreadP95Bps": 2.1,
              "depthImbalanceMean": 0.04, "aggressorDeltaSize": 1.8,
              "tradeCount": 210, "bookUpdateCount": 950}
}
```

The numbers are illustrative, not observed data. Define `depthImbalance=(bidSize-askSize)/(bidSize+askSize)` over the **same declared distance from mid**; return `null` if the denominator is zero or coverage inadequate. Define wall added/pulled only as a thresholded change in aggregated resting quantity at a stable price band across successive valid snapshots; it cannot establish order identity, intent, spoofing, or execution. Heatmap/HMC2 values are time-weighted recorded liquidity, not a live book or trade history (`docs/AGENT_API.md:150-156,213-217`; `docs/MARKETDATA.md:240-242,307-319`).

### Cadence, first slice, and limits

Compute compact rolling bins from live updates inside core with fixed memory, then publish at most one snapshot per symbol per second; cache the last result and coalesce identical versions. Give model orchestration at most one new context every 5-15 seconds or on a material threshold change, with a short token-budgeted summary and optional bounded drill-down query. Never pass the raw book/trade/heatmap stream to an LLM. Limit symbols, price bands, windows, top levels, response bytes, request rate, and concurrent work; never block the recorder or stream executor to satisfy a model query.

**Smallest server-side slice:** one explicitly requested symbol, current valid top of book/spread, fixed 5/25 bps depth bands, and 10/60-second aggressor-trade counts/size with per-field age and coverage. It can use the existing `ServerDataModel` live book and retained trade path (`libs/core/servermodel/ServerDataModel.hpp:49-50,66-72,89-105`; `.cpp:442-469,472-555`) and needs no HMC2 scan. A sidecar can compare it against the existing GUI Agent API `/book` and `/trades` in development. Add liquidity hotspots, wall changes, and HMC2/RAWL2 historical queries only after validating source semantics and measured cost. The HMC2 recorder already writes full unclipped L2 on its own worker, while the live book is band clipped (`docs/MARKETDATA.md:240-242`; `libs/core/servermodel/ServerDataModel.cpp:532-555`); do not put query work or extra per-message copies on that recorder worker. RAWL2/journal/roller are durable/offline sources for later replay and structural-change validation, not a prerequisite for this first slice (`docs/ARCHITECTURE.md:23-26,397-407`; `docs/MARKETDATA.md:513-520`).

Security and reliability: the owner's localhost APIs have no authentication by default (`libs/core/metrics/MetricsHttpServer.hpp:14-25`; `docs/AGENT_API.md:3,32`). Local binding and browser Origin/Host checks reduce exposure but do not authenticate another local process. A new context endpoint should default disabled, bind loopback, require a per-session bearer secret or equivalent local credential, allowlist symbols, and redact account/user data and raw message text. Avoid logging full context or prompts; mark provenance, invalidation, gaps, and stale periods. Keep HTTP serialization and any recording reads on bounded workers; publish immutable reduced snapshots to them so a slow CopeNet consumer cannot increase ingest latency or recorder load.
