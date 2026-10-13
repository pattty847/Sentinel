# Sentinel documentation index

Start with the maintained document for the area you are changing. This catalog covers tracked
project Markdown outside retired Cursor commands; private local notes and generated dependency
documentation are excluded. The code, AGENTS.md and STATUS.md establish current behavior and rules.
A link here preserves access; it does not certify that every older statement is still correct.

## Maintained documents

Update the relevant document when its contract changes. Do not update every file that mentions the
same subject. Add, move or remove the matching index entry in the same change as the document.
This index itself is maintained; AGENTS.md controls which tasks may update STATUS.md and TODO.md.

| Document | Purpose |
| --- | --- |
| [AGENTS.md](../AGENTS.md) | Agent rules and ownership boundaries |
| [README.md](../README.md) | Project overview and developer quick start |
| [docs/STATUS.md](STATUS.md) | Live facts, conductor and in-flight work; conductor only |
| [docs/TODO.md](TODO.md) | Feature scope and priorities; read only for relevant tasks |
| [docs/ARCHITECTURE.md](ARCHITECTURE.md) | System architecture and dependency direction |
| [docs/MARKETDATA.md](MARKETDATA.md) | Market-data protocol, DTOs and transport semantics |
| [docs/CONFIG.md](CONFIG.md) | Configuration keys and behavior |
| [docs/AGENT_WORKFLOW.md](AGENT_WORKFLOW.md) | Agent tooling and isolated GUI workflow |
| [docs/AGENT_API.md](AGENT_API.md) | GUI Agent API |
| [docs/RAW_CAPTURE.md](RAW_CAPTURE.md) | Raw capture formats and operation |
| [docs/ROLLER.md](ROLLER.md) | Journal roller operation and storage |
| [docs/SENTINEL_STREAM_CLIENT.md](SENTINEL_STREAM_CLIENT.md) | Client streaming behavior |
| [docs/WINDOWS_GPU_TESTS.md](WINDOWS_GPU_TESTS.md) | Windows native GPU setup and verification |
| [docs/RELEASE_CHECKLIST.md](RELEASE_CHECKLIST.md) | Release verification |
| [ops/monitoring/README.md](../ops/monitoring/README.md) | Metrics, alerts and monitoring operations |
| [scripts/README.md](../scripts/README.md) | Python backend setup and subprocess contracts |
| [scripts/sec/README.md](../scripts/sec/README.md) | SEC backend entry points and output contracts |
| [scripts/release/README.md](../scripts/release/README.md) | Release packaging for maintainers |
| [scripts/release/README_RELEASE.md](../scripts/release/README_RELEASE.md) | Quick start shipped in Mac bundles |
| [scripts/release/LAUNCH_README.md](../scripts/release/LAUNCH_README.md) | Runtime instructions shipped in bundles |
| [tests/README.md](../tests/README.md) | Test execution and coverage guidance |
| [tests/roller/fixtures/README.md](../tests/roller/fixtures/README.md) | Roller fixture provenance and reproduction |
| [tools/SentinelBar/README.md](../tools/SentinelBar/README.md) | SentinelBar setup and local operation |

## Supporting references

Read these only when they help the task. They are outside automatic maintenance: verify claims
against the current code and maintained references. Change or promote one only when the owner
explicitly asks; promotion means moving its entry into Maintained documents after checking it.

| Document | Scope |
| --- | --- |
| [docs/COORDINATE_SYSTEMS.md](COORDINATE_SYSTEMS.md) | Coordinate/render contracts; verify against current chart code |
| [docs/UI_ARCHITECTURE.md](UI_ARCHITECTURE.md) | GUI architecture overview; verify against current chart architecture |
| [docs/PAPER_TRADING_QUICKSTART.md](PAPER_TRADING_QUICKSTART.md) | Paper-trading walkthrough |
| [scripts/setup/WINDOWS_SETUP.md](../scripts/setup/WINDOWS_SETUP.md) | Earlier Windows/WSL migration guide; use README and WINDOWS_GPU_TESTS for current setup |

## Plans and historical evidence

These are design proposals, snapshots and investigations, not a parallel set of current docs.
An unfinished proposal may still be useful; STATUS.md and the owner's task identify active work.
Completed plans remain frozen. Do not rewrite them when related code changes. An explicit planning
task may amend its named active plan. To delete an obsolete document, remove its entry and fix live
incoming links rather than leaving a dangling reference.

| Document | Scope |
| --- | --- |
| [docs/FEATURES.md](FEATURES.md) | Older feature snapshot; contains a link to missing FEATURE_ARCHIVE.md |
| [docs/REENTRY_REVIEW.md](REENTRY_REVIEW.md) | September 2026 source review |
| [docs/TRADING_SIMULATION_BLUEPRINT.md](TRADING_SIMULATION_BLUEPRINT.md) | Trading-simulation design and phases |
| [docs/initiatives/continuous-history/VISION.md](initiatives/continuous-history/VISION.md) | Continuous-history initiative brief |

### Dated research catalog

Open only the plan or evidence named by the task or relevant maintained reference.

- [docs/research/2026-09-agent-api-v1-spec.md](research/2026-09-agent-api-v1-spec.md)
- [docs/research/2026-09-agentic-prediction-evaluation.md](research/2026-09-agentic-prediction-evaluation.md)
- [docs/research/2026-09-b1-whole-chunk-benchmark.md](research/2026-09-b1-whole-chunk-benchmark.md)
- [docs/research/2026-09-gpu-bin-lab-followup.md](research/2026-09-gpu-bin-lab-followup.md)
- [docs/research/2026-09-gpu-bin-lab-parity.md](research/2026-09-gpu-bin-lab-parity.md)
- [docs/research/2026-09-gpu-bin-lab-round4.md](research/2026-09-gpu-bin-lab-round4.md)
- [docs/research/2026-09-gpu-heatmap-integration-plan.md](research/2026-09-gpu-heatmap-integration-plan.md)
- [docs/research/2026-09-heatmap-interaction-spec.md](research/2026-09-heatmap-interaction-spec.md)
- [docs/research/2026-09-legacy-heatmap-removal-inventory.md](research/2026-09-legacy-heatmap-removal-inventory.md)
- [docs/research/2026-09-marketlens-har.md](research/2026-09-marketlens-har.md)
- [docs/research/2026-09-recording-serve.md](research/2026-09-recording-serve.md)
- [docs/research/2026-09-recording-v2.md](research/2026-09-recording-v2.md)
- [docs/research/2026-09-s5-plan.md](research/2026-09-s5-plan.md)
- [docs/research/2026-09-s5l-plan.md](research/2026-09-s5l-plan.md)
- [docs/research/2026-09-storage-pyramid.md](research/2026-09-storage-pyramid.md)
- [docs/research/2026-10-07-audit-triage.md](research/2026-10-07-audit-triage.md)
- [docs/research/2026-10-07-db2-packet.md](research/2026-10-07-db2-packet.md)
- [docs/research/2026-10-07-s8b-config-map.md](research/2026-10-07-s8b-config-map.md)
- [docs/research/2026-10-08-whole-pixel-mapping.md](research/2026-10-08-whole-pixel-mapping.md)
- [docs/research/2026-10-11-chart-core-architecture.md](research/2026-10-11-chart-core-architecture.md)
- [docs/research/2026-10-11-chart-core-packets.md](research/2026-10-11-chart-core-packets.md)
- [docs/research/2026-10-11-code-health/audit-heatmap-mainwindow.md](research/2026-10-11-code-health/audit-heatmap-mainwindow.md)
- [docs/research/2026-10-11-code-health/audit-repo-sweep.md](research/2026-10-11-code-health/audit-repo-sweep.md)
- [docs/research/2026-10-11-code-health/audit-tests.md](research/2026-10-11-code-health/audit-tests.md)
- [docs/research/2026-10-11-code-health/audit-ugr.md](research/2026-10-11-code-health/audit-ugr.md)
- [docs/research/2026-10-11-code-health/tool-scan-2026-10-11.md](research/2026-10-11-code-health/tool-scan-2026-10-11.md)
- [docs/research/2026-10-11-s8-zoom-evidence.md](research/2026-10-11-s8-zoom-evidence.md)
- [docs/research/2026-10-11-sentinelbar.md](research/2026-10-11-sentinelbar.md)
- [docs/research/2026-10-copenet-boundary.md](research/2026-10-copenet-boundary.md)
- [docs/research/2026-10-dom-validation.md](research/2026-10-dom-validation.md)
- [docs/research/2026-10-journal-anchors-audit.md](research/2026-10-journal-anchors-audit.md)
- [docs/research/2026-10-observability.md](research/2026-10-observability.md)
- [docs/research/2026-10-one-world-db-packet.md](research/2026-10-one-world-db-packet.md)
- [docs/research/2026-10-one-world-pipeline.md](research/2026-10-one-world-pipeline.md)
- [docs/research/2026-10-per-symbol-connections.md](research/2026-10-per-symbol-connections.md)
- [docs/research/2026-10-s6-plan.md](research/2026-10-s6-plan.md)
- [docs/research/2026-10-s7-plan.md](research/2026-10-s7-plan.md)
- [docs/research/2026-10-s8-legacy-deletion.md](research/2026-10-s8-legacy-deletion.md)
- [docs/research/2026-10-viewport-autoscale.md](research/2026-10-viewport-autoscale.md)
- [docs/research/2026-10-widget-audit.md](research/2026-10-widget-audit.md)
- [docs/research/2026-10-widget-pass-directive.md](research/2026-10-widget-pass-directive.md)
- [docs/research/2026-10-widget-pass-plan.md](research/2026-10-widget-pass-plan.md)
- [docs/research/2026-10-workflow-audit.md](research/2026-10-workflow-audit.md)

## Searching documentation

`.rgignore` excludes research, initiatives, private/archive folders and the older snapshots above
from default ripgrep searches. It does not delete them or change what Git tracks. Avoid repository
wide `--no-ignore`, which also includes build output and dependencies.

```bash
# Current docs and supporting references, respecting normal ignore rules
rg -n '<pattern>' docs
# Historical plans/evidence, explicitly requested
rg --no-ignore -n '<pattern>' docs/research
# A specific older reference
rg --no-ignore -n '<pattern>' docs/TRADING_SIMULATION_BLUEPRINT.md
```

The index keeps links to excluded files so they remain discoverable without loading their contents.
Other search tools need equivalent exclusions; `.rgignore` controls ripgrep only.
Positive include globs such as `-g '*.md'` can override file exclusions; use the commands above for
the normal documentation search scope.
