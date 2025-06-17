# Sentinel C++ Project: Plan & Progress

This document serves as the long-term memory and project plan for re-architecting the "Sentinel" trading tool in C++.

## The Ultimate Goal

To build a high-performance, 24/7 BTC microstructure monitoring engine in C++. The initial version will be a desktop application with a sophisticated, terminal-inspired UI centered around an order book heatmap chart, similar to professional tools like `aterm`. The core logic will be architected with enough performance and separation to eventually be exposed as a queryable microservice, potentially to provide real-time context to an LLM or power a distributed system.

---

## Current Status & Next Steps (For New Chat Session)

**We have successfully completed Phase 3.**

- **The Goal:** To ensure the UI is never blocked by networking or data processing.
- **The Accomplishment:** We successfully refactored the application to use a dedicated worker thread (`QThread`). All networking (`WebSocketClient`), data processing (`StatisticsProcessor`), and logic (`RuleEngine`) now run in the background, leaving the GUI completely responsive. We learned how to manage object lifetime across threads (`moveToThread`, `deleteLater`) and how to use Qt's thread-safe signal/slot mechanism for communication.
- **Immediate Next Step:** The next logical step is **Phase 4: Core Architecture & Headless Foundation**. This involves building the non-GUI data processing backbone required to power the advanced visualizations planned for Phase 5.

---

## Accomplishments & Concepts Covered

### Phase 1: Object-Oriented Refactoring [COMPLETED]
- **From single file to classes:** Refactored the entire application from one `main.cpp` file into a clean, object-oriented structure.
- **`WebSocketClient` Class:** Created a dedicated class to handle all networking logic.
- **`MainWindow` Class:** Created a dedicated class to handle all UI logic.
- **C++ Skills:** Deepened understanding of `.h`/`.cpp` files, class creation, and object composition.

### Phase 2: Implementing the Logic Core [COMPLETED]
- **`Trade` Data Structure:** Created a `tradedata.h` file with a `Trade` struct and `Side` enum, moving from simple strings to structured data (our C++ "Pydantic model").
- **`StatisticsProcessor` Class:** Built a processor to consume `Trade` objects and calculate a running Cumulative Volume Delta (CVD), emitting the result via a `cvdUpdated` signal.
- **Polymorphism with Abstract Classes:** Introduced the powerful concept of polymorphism by creating an abstract base `Rule` class with pure virtual functions (`= 0`). This defines a contract for all future rule types.
- **Concrete Rule Implementation:** Built our first specific rule, `CvdThresholdRule`, which inherits from `Rule` and implements the required logic.
- **`RuleEngine` Class:** Created an engine to manage a list of `Rule` objects, receive data, and evaluate all rules polymorphically.
- **Debugging:** Solved a complex linker error by refactoring the `CMakeLists.txt` file and fixed a C++ compilation error by switching from `QVector` to `std::vector` to correctly handle `std::unique_ptr`.

### Initial Setup (The "Setup Saga") [COMPLETED]
- **Environment:** Successfully configured a stable C++ environment using the **MSYS2 toolchain** (`g++`/`gdb`) after failed attempts with other methods.
- **Build System:** Migrated from a manual `tasks.json` to a professional **CMake** build system to automatically handle Qt's Meta-Object Compiler (`moc`).
- **Core Concepts:** Solidified understanding of the C++ build process (compiler vs. linker), `Q_OBJECT`, signals/slots, heap/stack, pointers, and Qt's parent-child ownership model.

### 1. Environment Setup:
- **Attempt 1 (Failed):** CMake + MSVC (`cppvsdbg` errors highlighted issues with proprietary Microsoft tooling in a VS Code context).
- **Attempt 2 (Failed):** Qt's Bundled MinGW (`g++.exe not found` errors proved the Qt installer was incomplete/broken).
- **Attempt 3 (SUCCESS):** A clean install of the **MSYS2 toolchain**, providing a reliable `g++` compiler and `gdb` debugger.
- **Final Fixes:**
    - Correctly configured the Windows `PATH` environment variable.
    - Diagnosed and fixed a terminal launch bug by setting `"externalConsole": false` in `launch.json`.

### 2. Core C++ & Qt Concepts Learned:
- **Stack vs. Heap Memory:** Understanding `MyObject obj;` (stack) vs. `MyObject* obj = new MyObject();` (heap).
- **Pointers & Accessors:** The difference between the dot `.` (for stack objects/references) and the arrow `->` (for pointers to heap objects).
- **Qt's Ownership Model:** How setting a parent widget automatically manages memory, preventing leaks by calling `delete` on children (RAII).
- **Signals & Slots:** Understanding Qt's core mechanism for event-driven programming.
- **Lambdas:** Using anonymous, inline functions (`[=]() { ... }`) as slots.
- **Basic GUI Development:** Using `QWidget`, `QLabel`, `QPushButton`, `QTextEdit`, and `QVBoxLayout`.
- **Networking:** Using `QWebSocket` to connect to a live data stream.
- **Data Handling:** Using `QJsonDocument` and `QJsonObject` to parse live JSON data.

### Phase 3: High-Performance C++ with Multithreading [COMPLETED]
- **Goal:** Ensure the UI is *never* blocked by networking or processing.
- **Tasks:**
    1. Move the `WebSocketClient` and all data processors to a separate worker thread using `QThread`.
    2. Learn how to safely pass data between threads using thread-safe signals and slots.
- **C++ Skills Learned:** The fundamentals of multithreading, `moveToThread`, `QThread`, `deleteLater`, and thread-safe cross-thread communication—a key reason for using C++.

### Phase 4: Core Architecture & Headless Foundation
- **Goal:** Build the "engine" of our application. This involves creating all necessary data processors and ensuring they are completely decoupled from any Qt GUI components, making the core logic portable and testable.
- **Tasks:**
    1. **Decouple Core Logic:** Refactor `StatisticsProcessor` and `RuleEngine` to remove all dependencies on Qt GUI libraries (e.g., `QtWidgets`). They should only use `QtCore` fundamentals if necessary (like signals/slots). This is a critical step for future reuse.
    2. **Implement an Order Book Processor:** Create a new class to subscribe to the "level2" WebSocket channel, process snapshot and update messages, and maintain a real-time internal model of the BTC-USD order book. This is a complex data structure task.
    3. **Define Data Interfaces:** Establish clear, lightweight data structures (structs/classes) that will be used to pass data from the core engine (worker thread) to the UI (main thread). These structures must not contain any GUI code.

- **C++ Skills Learned:** Advanced data structures for order book management, library-independent architecture, and API/interface design.

### Phase 5: Advanced Data Visualization with a Custom Charting Engine
- **Goal:** Evolve Sentinel from a text-based tool into a professional-grade market analysis dashboard with graphical charts inspired by tools like `aterm`.
- **Sub-Phase 5.1: The Custom Charting Framework**
    - **Tasks:**
        1. Create a base `ChartWidget` class inheriting from `QWidget` to serve as the foundation for all our future charts.
        2. Learn to use `QPainter` for custom 2D drawing. Focus on creating a coordinate system that maps price and time to screen pixels.
        3. Develop an efficient rendering loop that only repaints what's necessary, triggered by data updates from the core engine.
- **Sub-Phase 5.2: The `aterm`-style Heatmap**
    - **Tasks:**
        1. Design and implement the heatmap chart as a new `HeatmapChartWidget` class.
        2. Connect this widget to the `OrderBookProcessor`'s data stream.
        3. Implement the logic for translating order book depth and volume into a color gradient to create the heatmap effect.
        4. Draw the live price as a line overlay on top of the heatmap.
- **Sub-Phase 5.3: Advanced Overlays & Interactivity**
    - **Tasks:**
        1. Implement "tick-by-tick" liquidity tracking. This involves processing the order book data to identify and visualize the lifecycle of individual limit orders or clusters of liquidity.
        2. Add interactive features to the charts, such as zooming, panning, and a cursor that displays data for a specific point in time.
        3. Implement price and time axes with dynamic labels.
- **C++ Skills Learned:** Custom widget creation, 2D graphics with `QPainter`, advanced UI design, real-time data visualization, and performance optimization.

### Phase 6: The Terminal UI Shell
- **Goal:** Assemble all our components into a polished, professional, and functional application.
- **Tasks:**
    1. Re-design the `MainWindow` to use a flexible layout system (`QDockWidget` or similar) to host multiple chart and info panels.
    2. Implement a command console widget for interacting with the `RuleEngine` and other application components.
    3. Create dedicated widgets to display statistics from `StatisticsProcessor`, rule alerts from `RuleEngine`, and other key metrics.
- **C++ Skills Learned:** Advanced Qt UI design with complex layouts and interactive widgets.

---

## The Blueprint: Phased Development Plan

We will build the Sentinel application incrementally. Each phase introduces a new architectural concept and a new set of C++ skills.

### Phase 3: High-Performance C++ with Multithreading [COMPLETED]
- **Goal:** Ensure the UI is *never* blocked by networking or processing.
- **Tasks:**
    1. Move the `WebSocketClient` and all data processors to a separate worker thread using `QThread`.
    2. Learn how to safely pass data between threads using thread-safe signals and slots.
- **C++ Skills Learned:** The fundamentals of multithreading, `moveToThread`, `QThread`, `deleteLater`, and thread-safe cross-thread communication—a key reason for using C++.

### Phase 4: Finalizing the Vision
- **Goal:** Polish the application and architect the core for potential microservice use.
- **Tasks:**
    1. Implement a more advanced UI for adding/editing rules via the command input.
    2. Refactor the core logic (`StatisticsProcessor`, `RuleEngine`, etc.) to be completely independent of any Qt GUI libraries. This is a critical step before Phase 5.
    3. (Stretch Goal) Expose the core logic via a simple local API (like ZeroMQ or a basic HTTP server).
- **C++ Skills Learned:** Architectural patterns, library integration, and API design.

### Phase 5: Advanced Data Visualization & Order Book Analysis
- **Goal:** Evolve Sentinel from a text-based tool into a professional-grade market analysis dashboard with graphical charts inspired by tools like aterm.
- **Tasks:**
    1. **Implement an Order Book Processor:** Create a new class to subscribe to the "level2" WebSocket channel, process snapshot and update messages, and maintain a real-time internal model of the BTC-USD order book.
    2. **Learn Custom Drawing:** Explore using `QPainter` and a custom `QWidget` to create our own charting components. This gives maximum control over performance and appearance.
    3. **Build the Heatmap:** Design and implement the order book heatmap chart, where color intensity represents the volume of limit orders at each price level.
    4. **Overlay the Price:** Draw the live price line on top of the heatmap.
    5. **Integrate into UI:** Re-design the `MainWindow` to elegantly display the new charting components, creating a dashboard layout.
- **C++ Skills Learned:** Advanced data structures for order book management, custom widget creation, 2D graphics with `QPainter`, and advanced UI design. 