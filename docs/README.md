# Sentinel documentation index

Start with the maintained document for the area you are changing. This catalog covers tracked
project Markdown outside retired Cursor commands; generated dependency documentation is excluded.
The code and CONTRIBUTING.md establish current behavior and rules.
A link here preserves access; it does not certify that every older statement is still correct.

## Maintained documents

Update the relevant document when its contract changes. Do not update every file that mentions the
same subject. Add, move or remove the matching index entry in the same change as the document.
This index itself is maintained.

| Document | Purpose |
| --- | --- |
| [CONTRIBUTING.md](../CONTRIBUTING.md) | Build/test, ownership boundaries and rules for every change |
| [AGENTS.md](../AGENTS.md) | Entry point for coding agents |
| [README.md](../README.md) | Project overview and developer quick start |
| [docs/ARCHITECTURE.md](ARCHITECTURE.md) | System architecture and dependency direction |
| [docs/MARKETDATA.md](MARKETDATA.md) | Market-data protocol, DTOs and transport semantics |
| [docs/CONFIG.md](CONFIG.md) | Configuration keys and behavior |
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

## Supporting references

Read these only when they help the task. They are outside automatic maintenance: verify claims
against the current code and maintained references. Change or promote one only when a maintainer
explicitly asks; promotion means moving its entry into Maintained documents after checking it.

| Document | Scope |
| --- | --- |
| [docs/COORDINATE_SYSTEMS.md](COORDINATE_SYSTEMS.md) | Coordinate/render contracts; verify against current chart code |
| [docs/PAPER_TRADING_QUICKSTART.md](PAPER_TRADING_QUICKSTART.md) | Paper-trading walkthrough |

## Private documents

The maintainer's plans, workflow and agent tooling live in a separate private repository; Sentinel builds and tests without it.

## Searching documentation

Normal searches in Sentinel contain only public documentation. No `.rgignore` layer is needed.

```bash
rg -n '<pattern>' docs
```
