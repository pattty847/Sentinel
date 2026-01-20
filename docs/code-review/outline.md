# Sentinel Repo Code Review Outline

## Scope
- Full repo, file-by-file.
- Prioritize architectural alignment, hot paths, and long-term maintainability.
- Focus on what can stay, what should be refactored, and what needs re-architecture.

## Invariants and Guardrails
- Core is pure C++ (no Qt contamination beyond QString/QDateTime if needed).
- GUI owns Qt/QML/QSG.
- Rendering is GPU-first, deterministic.
- Viewport updates must go through setViewport() to increment viewportVersion.

## Review Stages
1) Docs pass (architecture alignment + drift notes)
2) Core pass (marketdata -> servermodel -> protocol -> utils/logging)
3) GUI render/data path (render -> datasources -> renderer)
4) GUI dock + mainwindow plumbing
5) Apps + tests + scripts
6) Consolidated recommendations

## Docs Pass Notes
- docs/ARCHITECTURE.md: claims strict client-server, GPU-only rendering, and server-only heatmap columns.
- docs/MARKETDATA_ARCHITECTURE.md: detailed MarketDataCore layering and threading.
- docs/RENDERING.md: remote heatmap pipeline and single-quad GPU node.
- docs/MAINWINDOW.md: dock framework and mainwindow orchestration.

Open questions for drift checks (to confirm during code pass):
- Does the client ever emit local heatmap columns in remote mode?
- Are all viewport mutations funneled through setViewport()?
- Are any Qt types creeping into libs/core beyond allowed scope?
- Is DataBootstrapper actually present and used as described?

## File-by-file Pass Order (current)
- docs/* (done)
- libs/core/marketdata/*
- libs/core/servermodel/*
- libs/core/protocol/*
- libs/core/* (logging, utils, version)
- libs/gui/render/*
- libs/gui/datasources/*
- libs/gui/UnifiedGridRenderer.*
- libs/gui/widgets/*
- libs/gui/mainwindow/*
- libs/gui/themes/*
- libs/gui/qml/*
- apps/*
- tests/*
- scripts/* (if relevant to architecture and correctness)

## Output Plan
- Per file: brief notes (purpose, risks, keep/refactor/redo).
- Per subsystem: summary of architecture fit and change recommendations.
- Final prioritized action list.
