# Sentinel C++: Architecture Overview

graph TD
    subgraph "Program 1: GUI Application"
        A["main.cpp<br/>🚀 Entry Point"] --> B["MainWindow<br/>🖥️ Qt GUI"]
        C["❌ WebSocketClient<br/>(Old - Removed)"] -.- B
        B --> D["StatisticsController<br/>📊 Qt Wrapper"]
        B --> E["RuleEngine<br/>🚨 Alert System"]
        D --> F["StatisticsProcessor<br/>🧮 CVD Calculator"]
        E --> G["CvdThresholdRule<br/>📈 Threshold Alerts"]
    end
    
    subgraph "Program 2: Stream Test"
        H["stream_main.cpp<br/>🚀 Entry Point"] --> I["CoinbaseStreamClient<br/>🌊 High-Speed Stream"]
        I --> J["Console Output<br/>💻 Terminal Display"]
    end
    
    subgraph "Shared Components"
        K["tradedata.h<br/>📋 Trade Structure"]
        L["coinbasestreamclient.h/cpp<br/>🏗️ Stream Engine"]
    end
    
    subgraph "Build System"
        M["CMakeLists.txt<br/>⚙️ Build Config"]
        M --> N["sentinel<br/>📱 GUI Executable"]
        M --> O["coinbase_stream_test<br/>⚡ Stream Executable"]
    end
    
    style C stroke-dasharray: 5 5,color:#ff6b6b
    style I fill:#4ecdc4
    style L fill:#4ecdc4

This document provides a high-level overview of the Sentinel C++ application's architecture, design principles, and data flow. It is intended to be a guide for developers working on the codebase.

## The North Star: The Vision for Sentinel

The ultimate goal of Sentinel is to be a professional-grade, high-performance market microstructure analysis tool, beginning with the BTC-USD pair on Coinbase. The vision extends beyond a simple desktop application to a robust, 24/7 analysis engine.

The core principles are:
1.  **High Performance:** The application must be able to process high-frequency data streams (both trades and order book updates) without lagging or freezing. The C++/Qt stack was chosen specifically for this purpose.
2.  **Modularity:** The core analysis logic should be completely decoupled from the user interface. This allows the "engine" to be potentially compiled as a library, run as a headless daemon, or exposed as a microservice in the future.
3.  **Rich Visualization:** The UI should evolve beyond simple text readouts to include rich, graphical visualizations of market data, such as order book heatmaps, inspired by professional trading tools like aterm.

## Current Architecture: A Two-Thread Model

Sentinel is built on a multi-threaded architecture to ensure the user interface remains responsive at all times.

![Architecture Diagram](https://raw.githubusercontent.com/context-copilot/sentinel-cpp-assets/main/architecture-v1.png)

### The Main (GUI) Thread
-   **Responsibilities:** Manages and renders all UI elements. Handles user input (mouse clicks, keyboard entry).
-   **Key Class:** `MainWindow`. It is the root of the UI and acts as the coordinator that sets up the worker thread and orchestrates the creation of all other objects.

### The Worker Thread
-   **Responsibilities:** Handles all blocking operations and heavy computation. This includes all networking, data parsing, and statistical calculations.
-   **Key Classes:**
    -   `WebSocketClient`: Manages the connection to the Coinbase WebSocket API, receives raw data, and parses it into structured `Trade` objects.
    -   `RuleEngine`: Manages a collection of `Rule` objects and evaluates them against incoming data.
    -   `StatisticsController`: A Qt-based wrapper that owns the pure C++ `StatisticsProcessor`.
    -   `StatisticsProcessor`: A pure C++ class responsible for calculating the Cumulative Volume Delta (CVD).

### The "Controller" Pattern & Data Flow
The key to our architecture is the **Controller Pattern**. Our core logic (like `StatisticsProcessor`) is written in pure, standard C++, making it independent and reusable. However, to integrate with Qt's threading and signal/slot system, we wrap it in a "Controller" (`StatisticsController`).

The data flows as follows:
1.  `WebSocketClient` receives data and emits a Qt signal (`tradeReady`).
2.  This signal is received by slots in `RuleEngine` and `StatisticsController` (within the worker thread).
3.  `StatisticsController` passes the data to the pure C++ `StatisticsProcessor`.
4.  When the processor has a new CVD value, it invokes a standard C++ callback.
5.  The callback is implemented inside `StatisticsController`, which then emits a new Qt signal (`cvdUpdated`).
6.  This signal is safely sent across the thread boundary to a slot in `MainWindow`, which updates the UI.

## The Journey So Far: A Phased Approach
1.  **Phase 1: Setup & OO Refactoring:** Migrated from a single-file script to a clean, object-oriented design with separate classes for UI (`MainWindow`) and networking (`WebSocketClient`). Established a stable CMake-based build system with the MSYS2 toolchain.
2.  **Phase 2: Logic Core:** Implemented the "brains" of the application. This involved creating the polymorphic `Rule` / `CvdThresholdRule` system and the `StatisticsProcessor` to calculate CVD.
3.  **Phase 3: Multithreading:** Solved the critical UI-freeze problem by moving all networking and processing to a background `QThread`, ensuring a responsive user experience.
4.  **Phase 4: Decoupling:** Began refactoring the core logic to be independent of the Qt framework, starting with the `StatisticsProcessor` and introducing the "Controller" pattern. This is a crucial step towards the microservice goal.
5.  **Phase 5: High-Performance Streaming Rewrite (December 2024):** 🚀 **MAJOR OVERHAUL** - Replaced the basic `WebSocketClient` with a robust, production-grade `CoinbaseStreamClient` designed for high-frequency market data analysis.
6.  **Phase 6: Bridge Integration (December 2024):** 🎯 **BRIDGE COMPLETE** - Successfully integrated the pure C++ streaming engine with Qt using the `StreamController` bridge pattern, achieving real-time GUI updates with professional-grade performance.

### 🔥 Phase 5 Accomplishments: The Great Streaming Rewrite

**Problem Solved:** The original issue where removing `std::this_thread::sleep_for()` caused duplicate trade prints in high-frequency streaming.

**What We Built:**
- **`CoinbaseStreamClient`:** A production-grade WebSocket client with:
  - ✅ **Trade Deduplication:** Uses `trade_id` to prevent duplicate prints
  - ✅ **Dual Speed Modes:** Full-hose (microsecond polling) vs. controlled polling
  - ✅ **Robust JSON Parsing:** Handles Coinbase's mixed number/string trade_id formats
  - ✅ **Thread-Safe Buffers:** Lock-protected trade storage with configurable limits
  - ✅ **Auto-Reconnection:** Handles connection drops gracefully
  - ✅ **Trade Logging:** Automatic file rotation and structured logging

**Key Innovations:**
- **`getNewTrades(symbol, lastTradeId)`:** Returns only trades newer than specified ID
- **Smart JSON Handling:** Converts numeric trade_ids to strings seamlessly
- **Type-Safe Trade Structure:** Centralized `tradedata.h` with proper `Side` enum

**What We Removed:**
- ❌ Old `WebSocketClient` (basic, unreliable)
- ❌ Qt WebSocket dependency from core streaming
- ❌ Duplicate Trade struct definitions

### 🎯 Phase 6 Accomplishments: Bridge Integration Success

**Problem Solved:** How to integrate the pure C++ high-performance streaming engine with Qt's signal/slot system without compromising performance.

**What We Built:**
- **`StreamController`:** A Qt bridge that wraps `CoinbaseStreamClient`:
  - ✅ **Qt Signal/Slot Integration:** Emits `tradeReceived()` signals for GUI updates
  - ✅ **Thread-Safe Polling:** 100ms timer-based polling using `QTimer`
  - ✅ **Smart Pointer Management:** Uses `std::unique_ptr` for automatic memory management
  - ✅ **Multi-Symbol Tracking:** Maintains `lastTradeIds` map for deduplication
  - ✅ **Clean Lifecycle:** Proper start/stop methods with safe cleanup

**Architecture Pattern:**
- **Bridge Pattern Implementation:** Pure C++ engine + Qt wrapper = Best of both worlds
- **Producer-Consumer Flow:** StreamController polls → emits signals → GUI updates
- **Multi-Threading:** Stream processing in worker thread, GUI updates in main thread

**Integration Results:**
- **Real-Time Performance:** Trade data flowing at sub-100ms latency
- **Responsive GUI:** No UI freezing during high-frequency streaming
- **CVD Updates:** Live statistical calculations displaying in real-time
- **Production Stability:** Robust error handling and graceful shutdown

**Current State: 🚀 BRIDGE COMPLETE!**
- **Program 1:** `sentinel` (GUI app) - **FULLY INTEGRATED** with StreamController bridge ✅
- **Program 2:** `coinbase_stream_test` - High-performance console streaming (**WORKING FLAWLESSLY**)

**✅ PHASE 6 COMPLETE: Bridge Integration Success!**
- **Option B Implemented:** StreamController bridge pattern **WORKING PERFECTLY**
- **Real-time GUI:** Trade data flowing at 100ms intervals
- **CVD Calculation:** Live statistics updating in real-time
- **Multi-threaded:** Responsive UI with background streaming
- **Production Ready:** Robust error handling and clean shutdown

**🚀 NEXT PHASE: Advanced Visualizations**
- **Phase 7:** Custom Qt Charts - Real-time trade plotting with tick precision
- **Phase 8:** Order Book Heatmaps - Professional-grade liquidity visualization
- **Phase 9:** Multi-Timeframe Analysis - Zoom controls and aggregated views
- **Phase 10:** Performance Optimization - Efficient rendering and memory management

## The Build System
-   **CMake:** The cross-platform build system generator. `CMakeLists.txt` is our master blueprint.
-   **make:** The build tool that executes the blueprint generated by CMake.
-   **moc (Meta-Object Compiler):** A Qt tool that reads `Q_OBJECT` classes and generates necessary C++ source code to enable signals, slots, and other Qt features. These generated files are placed in the `build/sentinel_autogen` directory and compiled along with your own code. 