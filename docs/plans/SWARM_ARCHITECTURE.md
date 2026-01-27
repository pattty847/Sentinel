# Sentinel Swarm Architecture: The Self-Driving Engineering Hive

## 1. Vision
The Sentinel Swarm is a multi-agent orchestration layer designed to automate the full lifecycle of feature development for the Sentinel GPU Trading Terminal. It moves beyond "AI code generation" into "AI engineering," where agents own the planning, implementation, visual verification, and peer review of features.

## 2. Core Pillars
- **Git as Truth:** Every feature is an isolated branch. Communication happens via commits, PR comments, and state files.
- **Closed-Loop Verification:** Implementation is not complete until it compiles and passes visual inspection (Vision QA).
- **Architectural Purity:** Strict adherence to `AGENTS.md` (Core vs. GUI separation, Modern C++20).

---

## 3. The Swarm Cast (Specialized Agents)

### A. The Hive Mind (Orchestrator)
- **Host:** Python-based CLI (`tools/swarm/main.py`).
- **Function:** Manages state transitions, spawns agents, and handles Git operations.
- **State:** Tracks feature progress in `swarm_state.json`.

### B. The Architect (Planner)
- **Role:** Structural design.
- **Logic:** Inherits from `.cursor/commands/plan.msg.md`.
- **Output:** A multi-step blueprint defining integration points in `libs/core` and `libs/gui`.

### C. The Engineer (Implementer)
- **Role:** Code fabrication.
- **Logic:** Iterative *Edit -> Compile -> Debug* loop.
- **Mandate:** Modern C++20, RAII, zero-allocation render paths.

### D. The Eye (Visual QA)
- **Role:** GUI verification.
- **Function:** Launches `sentinel_gui`, captures screenshots via OS-level tools or Qt hooks, and uses a Vision LLM (VLM) to verify layout and rendering.
- **Success Criteria:** "Does the rendered geometry match the intended viewport state?"

### E. The Critic (Code Reviewer)
- **Role:** Quality gatekeeper.
- **Logic:** Inherits from `.cursor/commands/code-review.msg.md`.
- **Power:** Can `REJECT` a branch, forcing the Engineer back to the fabrication phase.

---

## 4. The Loop (Feature Lifecycle)

1. **Inception:** User provides a high-level goal (e.g., "Add Volume Profile rendering").
2. **Isolation:** Hive Mind creates `feature/volume-profile`.
3. **Drafting:** **Architect** scans the repo and writes `SWARM_PLAN.md` in the branch root.
4. **Execution:** **Engineer** modifies files. After each change, it runs `cmake --build build/`.
   - *Failure:* Stderr is sent back to Engineer for immediate fix.
5. **Observation:** **Eye** runs the binary. It checks for:
   - GUI crashes or stalls.
   - Correct pixel placement of new elements.
6. **Peer Review:** **Critic** runs `git diff main` and checks against `AGENTS.md`.
7. **Consolidation:** Hive Mind notifies the user for final manual approval or performs a rebase-merge into `dev`.

---

## 5. Technology Stack
- **Orchestration:** Python 3.11+
- **Agent Interop:** Structured JSON logs and Markdown files.
- **C++ Verification:** CMake + Ninja + Clang-Tidy.
- **Visual Testing:** PySide6 (for GUI control) + OpenAI/Google Vision APIs.
- **Version Control:** Git (Subprocesses).

---

## 6. Phase 1 Implementation Goals
- [ ] Scaffold `tools/swarm/`.
- [ ] Implement `GitDriver` (branching, staging, committing).
- [ ] Implement `ArchitectAgent` to generate automated plans.
- [ ] Integrate a basic `CompileHook` to verify code changes.
