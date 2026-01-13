# Sentinel Client-Server Architecture

This document details the distributed architecture of Sentinel. The legacy monolithic GUI has been gutted in favor of a high-performance server-client model that enables 24/7 data collection, eliminates GUI performance bottlenecks, and provides scalable GPU-resident market data streaming.

## Architecture Overview

### Data Flow

**Server Process**:
`Exchange → MarketDataCore → ServerDataModel → Persistence + SentinelStreamServer → WebSocket`

**Client Process**:
`WebSocket → SentinelStreamClient → RemoteGridDataSource → UnifiedGridRenderer → GPU`

### Core Benefits

- **Continuous Data Collection**: Server runs 24/7 without GUI dependencies.
- **Performance Isolation**: Heavy data aggregation (TWAP, Rollups) is offloaded to the server.
- **GPU-First Rendering**: Client receives pre-aggregated columns for direct GPU texture upload, bypassing legacy CPU-per-cell hot paths.
- **Scalability**: Multiple clients can connect to a single data source.

---

# CLIENT

## Overview

The client component (`sentinel-gui`) is a lightweight Qt6/QML application focused purely on high-fidelity visualization. It connects to the server via WebSocket to receive pre-processed, GPU-ready market data streams.

## Core Architecture

### Data Source Abstraction (`libs/gui/datasources/`)

**RemoteGridDataSource.hpp/.cpp**
- Implements remote data access via `SentinelStreamClient`.
- Manages local buffers for received data slices.
- Emits `heatmapColumnReady` for direct GPU upload.

### Network Communication (`libs/core/protocol/`)

**SentinelStreamClient.hpp/.cpp**
- WebSocket client implementation using Boost.Beast.
- Manages connection lifecycle and automatic reconnection.
- Handles protocol parsing (JSON v0 / Binary v1).

### Integration Layer

**UnifiedGridRenderer Enhancement**
- Consumes `heatmap_slice` messages directly.
- Bypasses legacy CPU aggregation logic (`DataProcessor`/`LTSE` gutted in client).
- Maps pre-aggregated server data to GPU-resident textures.

---

# SERVER

## Overview

The server component (`sentinel-server`) is a headless daemon designed for continuous operation. It ingests market data, performs TWAP aggregation, manages persistent storage, and streams processed data to connected clients.

## Core Architecture

### Server Data Model (`libs/core/servermodel/`)

**ServerDataModel.hpp/.cpp**
- Central data management hub for all market symbols.
- Maintains in-memory state and coordinates persistence.
- Thread-safe access to live order books and historical data.

### Persistence Layer

**TickBinaryLogger.hpp/.cpp**
- Append-only binary logging of raw market data.
- Segment-based storage with hourly rotation.

**TimeframeAggregator.hpp/.cpp**
- Timer-driven aggregation pipeline (100ms, 1s, 5s, 1m).
- Converts raw ticks to dense, GPU-ready slices.

---

# PROTOCOL & PERFORMANCE

## Message Protocol

**JSON Protocol (v0)**
- Used for initial subscription and snapshot handshake.
- Payload: `type`, `symbol`, `timeframe_ms`, `slices`, `columnBytes` (base64).

**Binary Protocol (v1 - Active)**
- Compact message framing for high-volatility live streaming.
- Efficient serialization of dense slice data.

## Performance Characteristics

- **Latency**: Direct WebSocket streaming bypasses Qt signal overhead.
- **Throughput**: Capable of streaming 8192x8192 (67M cells) depth grids with zero-lag visualization.
- **Zero-Tax Render**: CPU cost in the client remains constant regardless of the number of visible cells.

## Phase Implementation Status

**Phases 0-4: Distributed Core (✓ Completed)**
- Implemented `sentinel-server` and `SentinelStreamClient`.
- Implemented `TickBinaryLogger` and `TimeframeAggregator`.
- Gutted all legacy CPU-per-cell rendering paths.
- Established 100ms TWAP heatmap streaming with gap-filling.

**Phase 5: Performance & History (In Progress)**
- Implementation of `get_history` for startup backfill.
- Optimization of Binary v1 protocol for multi-symbol streaming.