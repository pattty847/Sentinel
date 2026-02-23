# Thread Safety Audit

Audit the current file or selected code for threading violations according to Sentinel's rules.

**Check for:**
- Direct QObject access from non-main threads (violates Qt threading rules)
- Missing Qt::QueuedConnection for cross-thread signals/slots
- QSG node access from GUI thread (must be render thread only)
- MarketDataCoreEngine access from GUI thread (must be worker thread)
- Shared state without proper synchronization
- Heap allocations in hot rendering paths (QSGGeometry should be preallocated)

**Rules to enforce:**
- GUI widgets: main thread only
- MarketDataCoreEngine: worker threads only
- QSG rendering: render thread only
- Cross-thread communication: Qt::QueuedConnection only
- No direct widget-to-widget calls (use hub-and-spoke via MainWindowGPU)

**Output format:**
- List each violation with file:line
- Explain why it's a problem
- Suggest the correct pattern (reference AGENTS.md if needed)
- Flag any performance risks (allocations in hot paths, etc.)

Be specific and actionable. Reference Sentinel's threading model from AGENTS.md section 5.
