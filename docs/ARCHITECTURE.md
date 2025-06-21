# Sentinel C++: Architecture Overview

```mermaid
graph TD
    subgraph "Worker Thread"
        subgraph "Network I/O (Boost.Asio)"
            WS["Coinbase WebSocket API"];
        end
        
        CS[("CoinbaseStreamClient <br> C++ Networking Core <br> (Boost.Beast)")] -- "Raw Trade Data" --> SC[("StreamController <br> Qt Bridge")];
    end

    subgraph "Data Fan-out (Qt Signals/Slots)"
        SC -- "tradeReceived(trade)" --> STATS["StatisticsController"];
        SC -- "tradeReceived(trade)" --> RULES["RuleEngine"];
        SC -- "tradeReceived(trade)" --> CHART["TradeChartWidget"];
    end

    subgraph "Processing Logic"
        STATS -- "trade object" --> PROC["StatisticsProcessor (CVD)"];
    end

    subgraph "Main (GUI) Thread"
        MW["MainWindow"] -- "owns/displays" --> CHART;
        MW -- "owns/displays" --> CVD_LABEL["QLabel (CVD)"];
        MW -- "owns/displays" --> ALERT_LABEL["QLabel (Alerts)"];
        
        STATS -- "Qt Signal: cvdUpdated(value)" --> MW;
        RULES -- "Qt Signal: alertTriggered(msg)" --> MW;
    end
    
    WS --> CS;

    style CS fill:#4ecdc4,stroke:#333,stroke-width:2px
    style SC fill:#f9d71c,stroke:#333,stroke-width:2px
    style CHART fill:#ff6b6b,stroke:#333,stroke-width:2px
```

This document provides a high-level overview of the Sentinel C++ application's architecture, design principles, and data flow. It is intended to be a guide for developers working on the codebase.

## The North Star: The Vision for Sentinel

The ultimate goal of Sentinel is to be a professional-grade, high-performance market microstructure analysis tool, beginning with the BTC-USD pair on Coinbase and expand from there. The vision extends beyond a simple desktop application to a robust, 24/7 analysis engine.

The core principles are:
1.  **High Performance:** The application must be able to process high-frequency data streams. The choice of C++, Boost.Beast, and Qt ensures we can handle demanding workloads without compromising on speed or responsiveness.
2.  **Modularity:** The core analysis logic is completely decoupled from the user interface. This is achieved by separating code into distinct libraries (`sentinel_core`, `sentinel_gui_lib`).
3.  **Portability & Maintainability:** By using `vcpkg` for dependency management and `CMake` for builds, the project can be easily set up and compiled on different platforms (Windows, macOS, Linux).

## Current Architecture: A Two-Thread Model

Sentinel is built on a multi-threaded architecture to ensure the user interface remains responsive at all times.

### The Main (GUI) Thread
-   **Responsibilities:** Manages and renders all UI elements. Handles user input.
-   **Key Class:** `MainWindow`. It is the root of the UI and orchestrates the creation of all other objects.

### The Worker Thread
-   **Responsibilities:** Handles all blocking operations and heavy computation. This includes all networking, data parsing, and statistical calculations.
-   **Key Classes:**
    -   `CoinbaseStreamClient`: A high-performance, asynchronous networking client built with **Boost.Beast**. It runs on its own I/O context and communicates with the rest of the application via the `StreamController`.
    -   `RuleEngine`: Manages a collection of `Rule` objects and evaluates them against incoming data.
    -   `StatisticsController`: A Qt-based wrapper that owns the pure C++ `StatisticsProcessor`.
    -   `StatisticsProcessor`: A pure C++ class responsible for calculating the Cumulative Volume Delta (CVD).

### The "Controller" Pattern & Data Flow
The key to our architecture is the **Controller Pattern**. Our core logic (like `StatisticsProcessor`) is written in pure, standard C++, making it independent and reusable. However, to integrate with Qt's threading and signal/slot system, we wrap it in a "Controller" (`StatisticsController` or `StreamController`).

The data flows as follows:
1.  `CoinbaseStreamClient` receives data on its own thread.
2.  It uses a thread-safe queue to pass the data to the `StreamController`.
3.  The `StreamController` emits a Qt signal (`tradeReceived`).
4.  This signal is received by slots in `RuleEngine`, `StatisticsController`, and `TradeChartWidget` (within the main worker thread).
5.  When a controller has new data for the UI, it emits another Qt signal (e.g., `cvdUpdated`).
6.  This signal is safely sent across the thread boundary to a slot in `MainWindow`, which updates the UI.

## The Journey So Far: A Phased Approach
1.  **Phase 1-4:** Initial setup, OO refactoring, and multithreading.
2.  **Phase 5-7:** High-performance client, bridge integration, and real-time charting.
3.  **Phase 8: Modern Tooling & Networking Refactor:**
    - **Replaced Manual Dependencies with `vcpkg`:** Introduced `vcpkg.json` as a single source of truth for all C++ libraries.
    - **Replaced WebSocket++ with `Boost.Beast`:** Rewritten the entire networking client to use a modern, asynchronous, and more robust library.
    - **Standardized Builds with `CMakePresets`:** Made the build process simple, reproducible, and platform-agnostic.
    - **Refined Project Structure:** Re-organized the codebase into a professional `libs/` and `apps/` structure to improve modularity and decoupling.

### 🔥 Phase 7 Accomplishments: Real-Time Charting Engine

**Problem Solved:** How to visualize high-frequency trade data in real-time without using slow, pre-packaged chart libraries.

**What We Built:**
- **`TradeChartWidget`:** A fully custom `QWidget` that takes complete control of the rendering process.
- **Dynamic Scaling:** The chart automatically calculates the min/max of the current data window and scales the axes accordingly.
- **Price and Time Axes:** We now draw proper Y-axis (price) and X-axis (time) labels with grid lines for context.
- **Multi-layered Drawing:**
  - ✅ **Layer 1:** Black background and faint grid.
  - ✅ **Layer 2:** A continuous white line representing the price action.
  - ✅ **Layer 3:** Red/Green dots overlaid at each data point to show aggressive buy/sell flow.
- **Symbol Filtering:** The chart is now "symbol-aware" and correctly filters to display only `BTC-USD`, solving the scaling problem.

**Architecture Pattern:**
- **Direct Signal-to-Slot Connection:** The `StreamController`'s `tradeReceived` signal is piped directly to the `TradeChartWidget`'s `addTrade` slot, making the data flow incredibly efficient.
- **`QPainter` Mastery:** All rendering is done using `QPainter`, giving us pixel-perfect control over the final output.

**Integration Results:**
- **The "Good Squigglies":** We have a beautiful, real-time chart showing live market microstructure.
- **Readable & Contextual:** The addition of axes makes the chart instantly understandable.
- **High Performance:** The custom widget can handle the high-frequency data stream with ease.

**🚀 NEXT PHASE: Advanced Visualizations & UI Polish**
- **Phase 8:** Order Book Heatmaps - The next major step toward our `aterm` goal.
- **Phase 9:** UI/UX Polish - Add zoom/pan controls, a proper status bar, and improved aesthetics.
- **Phase 10:** Performance Optimization - Explore OpenGL rendering for even greater speed.

## The Build System
-   **CMake:** The cross-platform build system generator. `CMakeLists.txt` is our master blueprint.
-   **make:** The build tool that executes the blueprint generated by CMake.
-   **moc (Meta-Object Compiler):** A Qt tool that reads `Q_OBJECT` classes and generates necessary C++ source code to enable signals, slots, and other Qt features. These generated files are placed in the `build/sentinel_autogen` directory and compiled along with your own code. 