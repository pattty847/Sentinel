# Recommended architecture
- Ingestion/Storage (server/headless core):
  - Subscribe to exchanges; normalize and write immutable “tick buckets” (e.g., 50/100ms) to append-only segments on disk (and/or a time-series DB).
  - Background rollup jobs build a multi-resolution pyramid (e.g., 50ms → 100ms → 250ms → 1s → 5s → 30s).
  - Keep hot windows in memory (ring buffers) for all TFs; evict by retention.
- API (server):
  - Stream: WebSocket (binary) for live deltas per TF.
  - Snapshot: gRPC/HTTP to fetch historical slices for [t0, t1], TF=auto or specific.
  - Query shape matches client viewport: {timeStart, timeEnd, priceMin, priceMax, timeframe}.
- Client:
  - Requests best TF via pixels-per-slot; pulls snapshot; then subscribes to live TF stream.
  - World-coords render (we have this), append-only, transform-only pan/zoom, rebuild only on LOD change.

Why precompute TFs
- Computing higher TFs on the fly is costly and repeats across clients. A small pyramid amortizes cost once, enables caching, and ensures consistent results.
- You don’t need “all” TFs; 6–8 well-chosen TFs with hysteresis cover the spectrum.

Data/compute estimate (rule-of-thumb)
- Storage: base TF + ~1/3 per level above (downsampling shrinks). With 6 levels, expect ~1.6–1.8× the base volume.
- CPU: base ingest + low, steady background rollups (batched).
- Bandwidth: snapshot is O(visible columns × visible price buckets), live stream is incremental per TF.

Minimal path to start
- Headless C++ core service (reuse LTSE and pipeline), expose gRPC/WebSocket.
- Append-only segment writer + in-memory ring for hot window.
- One background rollup (e.g., 50→250ms and 250→1s) to prove the flow; expand later.
- Client: auto-LOD request + live subscribe per TF (we already have LOD plumbing).

Python gateway?
- Fine as a control/management plane (FastAPI) and for orchestration, but keep the hot path (ingest/rollup/serve snapshots) in C++ for latency and memory locality. If you prefer Python-only to bootstrap, wrap the C++ core via gRPC and keep data-plane in C++.

Historical visibility
- Yes, this unlocks it: snapshots fetch from disk segments (with simple index), rollups ensure large ranges are cheap. You’ll finally get smooth history browsing and instant follow.