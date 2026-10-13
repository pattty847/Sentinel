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

## Private planning documents

Research, initiative briefs and the older feature/re-entry/trading-simulation snapshots are kept
locally and excluded from future public commits. They are not part of a public checkout. Their
optional private catalog is `docs/private/INTERNAL_INDEX.md`; ask the owner for a copy only when the
task needs a named plan. Existing public Git history and deletion diffs still contain older versions.

## Searching documentation

`.rgignore` excludes private planning material from normal ripgrep searches, and `.gitignore` keeps
it out of future commits. Neither changes older Git history. Do not force-add private docs. Avoid
repository-wide `--no-ignore`, which also includes build output and dependencies.

```bash
# Public docs and supporting references, respecting normal ignore rules
rg -n '<pattern>' docs
# A private local research copy, only when the task requires it
rg --no-ignore -n '<pattern>' docs/research
```

Other search tools need equivalent exclusions; `.rgignore` controls ripgrep only. Positive include
globs such as `-g '*.md'` can override file exclusions; use the commands above for the normal scope.
