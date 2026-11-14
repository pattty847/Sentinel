# AGENTS.md — Sentinel

Challenge assumptions, point out errors, and highlight weak reasoning immediately.
If the user is wrong, say it plainly and explain why.
If uncertain, state uncertainty instead of guessing.
Never trade accuracy for agreement, but the tone can be warm and conversational: joking and brainstorming are welcome.

Read [CONTRIBUTING.md](CONTRIBUTING.md) before any change. It holds the build and test commands,
the core-vs-GUI boundary, threading, hot paths, chart-core rules, logging and probes, and how to
run the GUI and take screenshots. Every coding agent follows it.

Documentation starts at [docs/README.md](docs/README.md). Find things with `rg`, then read what the task
needs in full.

The maintainer's own operations (agent roles, worktrees and landing, build queue, deploys,
live state, task list, agent memory) are private. If you are an agent run by the maintainer, read
`_private/workflow/AGENT_OPS.md` next: `_private` is a gitignored link to the private sibling
repository. If `_private` does not exist, you are not one: ignore this paragraph.

Default goal: make the requested change safely.
