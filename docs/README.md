# Sentinel documentation index

Start with the maintained document for the area you are changing. This catalog covers tracked
project Markdown outside retired Cursor commands; the separate private repository and generated dependency
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
| [docs/README_CLAIMS.md](README_CLAIMS.md) | Evidence ledger for every README claim; update with the README |
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
| [docs/PAPER_TRADING_QUICKSTART.md](PAPER_TRADING_QUICKSTART.md) | Paper-trading walkthrough |

## Private planning documents

Research, initiative briefs, personal notes and older planning snapshots are versioned in the
separate PRIVATE repository [Sentinel-Private](https://github.com/pattty847/Sentinel-Private).
Clone it as a sibling of the main Sentinel checkout, `../Sentinel-Private` (agent worktrees live elsewhere; see AGENTS.md section 0 for the path from a worktree). Its layout is `research/`,
`initiatives/`, `private/` and the top-level planning snapshots. Its catalog is
`../Sentinel-Private/private/INTERNAL_INDEX.md`; read only the file a task names.
Commit edits to private plans in that repository. `_agent/` stays local to Sentinel.
Sentinel builds and tests do not require the private repository. Older public history still
contains the moved documents; this change does not rewrite that history.

## Searching documentation

Normal searches in Sentinel contain only public documentation. No `.rgignore` layer is needed.
Do not search the sibling private repository unless the task names a relevant file.

```bash
rg -n '<pattern>' docs
# A named private plan, explicitly required by the task
rg -n '<pattern>' ../Sentinel-Private/research/<named-plan>.md
```
