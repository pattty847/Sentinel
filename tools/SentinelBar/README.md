# SentinelBar

Menu bar panel for the dev box (macOS 13+, SwiftUI `MenuBarExtra`). Separate from the Qt app; read-only.

- **Icon:** pulse when idle; agent count (cpu, or hammer while the build queue has tickets, with `qN`); warning triangle when
  `sentinel-server` (:8090) or capture (:8091) stops answering `/metrics`.
- **Panel:** agents (every running `claude` and `codex` process: model, worktree from its cwd, and its last
  tool call or message from its own transcript, `~/.claude/projects/...` or `~/.codex/sessions/...`), build queue (from `/tmp/sentinel-build-queue` tickets, same order as `build-queue.sh status`),
  `git worktree list` with commits ahead of `main`, service health (server, capture, GUI host :17190),
  `scripts/dev/budget.sh` output (every 5 min), and buttons for Grafana, logs, Terminal at the repo.

Build and install: `tools/SentinelBar/build-app.sh --install`. Dev loop: `swift run` in this directory.

Repo path defaults to `~/Programming/Sentinel`; override with `SENTINEL_REPO` or
`defaults write com.sentinel.bar repoPath /path/to/Sentinel`.
