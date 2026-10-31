#!/usr/bin/env bash
# Fake Codex only; isolated repos test argv, post-run checks and client push policy.
set -euo pipefail
SRC=$(cd "${1:-$(dirname "$0")/../..}" && pwd -P)
scratch=$(mktemp -d)
scratch=$(cd "$scratch" && pwd -P)
trap 'rm -rf "$scratch"' EXIT
export GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_NOSYSTEM=1
export GIT_AUTHOR_NAME=t GIT_AUTHOR_EMAIL=t@example.invalid GIT_COMMITTER_NAME=t GIT_COMMITTER_EMAIL=t@example.invalid
repo="$scratch/repo"
wt="$scratch/linked tree"
git init -q -b main "$repo"
mkdir -p "$repo/scripts/dev/hooks"
cp "$SRC/scripts/dev/"{codex-exec.sh,agents.env,agent-worktree.sh,install-hooks.sh} "$repo/scripts/dev/"
cp "$SRC/scripts/dev/hooks/pre-push" "$repo/scripts/dev/hooks/"
git -C "$repo" add .
git -C "$repo" commit -qm base
git -C "$repo" config extensions.worktreeConfig true
git -C "$repo" worktree add -qb agent/test "$wt"
gd=$(git -C "$wt" rev-parse --absolute-git-dir)
export CODEX_BIN="$scratch/fake-codex" CODEX_LOG_DIR="$scratch/logs"
unset CODEX_MODEL CODEX_EFFORT CODEX_APPROVAL || true
cat > "$CODEX_BIN" <<'FAKE'
#!/usr/bin/env bash
set -eu
printf '%s\n' "$PWD" > "$FAKE_CWD"
printf '%s\n' "$@" > "$FAKE_ARGS"
cat > "$FAKE_PROMPT"
while [[ $# -gt 0 ]]; do
    if [[ "$1" == -o ]]; then echo done > "$2"; shift; fi
    shift
done
printf '{"type":"thread.started","thread_id":"fixture-thread"}\n'
if [[ -n "${FAKE_TAMPER:-}" ]]; then touch "$FAKE_TAMPER"; fi
exit "${FAKE_RC:-0}"
FAKE
chmod +x "$CODEX_BIN"
export FAKE_CWD="$scratch/cwd" FAKE_ARGS="$scratch/args" FAKE_PROMPT="$scratch/prompt"
wrapper="$repo/scripts/dev/codex-exec.sh"
fail() { echo "FAIL: $*" >&2; exit 1; }
has() { grep -Fxq -- "$1" "$scratch/out" || fail "missing argv: $1"; }
absent() { if grep -Fq -- "$1" "$scratch/out"; then fail "unexpected argv: $1"; fi; }
dry() { "$wrapper" --worktree "$wt" --role writer --dry-run "$@" > "$scratch/out" 2> "$scratch/err"; }
reject() { if "$wrapper" "$@" > "$scratch/out" 2> "$scratch/err"; then fail "accepted: $*"; fi; }
dry
has workspace-write
has --add-dir
has "$gd"
has "$repo/.git/objects"
has "$repo/.git/refs/heads/agent"
has "$repo/.git/logs/refs/heads/agent"
has 'approval_policy="never"'
has --json
has -o
has -
[[ $(grep -c '^--add-dir$' "$scratch/out") == 4 ]] || fail 'four commit roots'
mkdir "$scratch/extra"
ln -s "$scratch/extra" "$scratch/extra-link"
touch "$scratch/img 1.png" "$scratch/img 2.png"
dry --resume thread-123 --model custom --effort low --add-dir "$scratch/extra-link" --image "$scratch/img 1.png" --image "$scratch/img 2.png"
has resume
has thread-123
has 'model="custom"'
has 'model_reasoning_effort="low"'
has 'sandbox_mode="workspace-write"'
has 'approval_policy="never"'
absent --add-dir
absent extra-link
has "$scratch/img 1.png"
has "$scratch/img 2.png"
grep -q '^sandbox_workspace_write.writable_roots=\[' "$scratch/out" || fail 'resume roots'
grep -Fxq "cwd=$wt" "$scratch/err" || fail 'resume cwd'
dry --last
has --last
CODEX_APPROVAL=on-request dry
has 'approval_policy="on-request"'
CODEX_APPROVAL=on-request dry --approval never
has 'approval_policy="never"'
CODEX_MODEL=environment CODEX_EFFORT=low dry
has environment
has 'model_reasoning_effort="low"'
CODEX_MODEL=environment dry --model flag
has flag
"$wrapper" --worktree "$wt" --role reviewer --dry-run > "$scratch/out" 2> "$scratch/err"
has read-only
absent --add-dir
absent "$gd"
dry --no-commit
absent --add-dir
reject --role writer
reject --worktree "$wt"
reject --worktree "$wt" --role danger-full-access
reject --worktree "$wt" --role writer --approval danger-full-access
reject --worktree "$wt" --role writer --dangerously-bypass-approvals-and-sandbox
reject --worktree "$wt" --role writer --sandbox danger-full-access
reject --worktree "$wt" --role reviewer --add-dir "$scratch/extra"
reject --worktree "$wt" --role reviewer --no-commit
reject --worktree "$wt" --role writer --resume a --last
reject --worktree "$repo" --role writer --dry-run
for branch in master plain; do
    git -C "$wt" switch -qc "$branch"
    reject --worktree "$wt" --role writer --dry-run
done
# Move main only in the fixture so a linked worktree can hold it.
git -C "$repo" switch -qc holder
git -C "$wt" switch -q main
reject --worktree "$wt" --role writer --dry-run
git -C "$wt" switch --detach -q
reject --worktree "$wt" --role writer --dry-run
git -C "$wt" switch -q agent/test
git -C "$repo" switch -q main
touch "$gd/config.worktree"
reject --worktree "$wt" --role writer --dry-run
rm "$gd/config.worktree"
printf 'a prompt with `literal` $characters\n' > "$scratch/input"
"$wrapper" --worktree "$wt" --role writer --resume thread-123 --prompt-file "$scratch/input" > "$scratch/events" 2> "$scratch/err"
cmp "$scratch/input" "$FAKE_PROMPT" || fail 'prompt changed'
[[ $(cat "$FAKE_CWD") == "$wt" ]] || fail 'actual resume cwd'
grep -q 'thread_id=fixture-thread' "$scratch/err" || fail 'thread id not announced'
[[ $(find "$CODEX_LOG_DIR" -name '*.jsonl' | wc -l) -eq 1 ]] || fail 'jsonl log missing'
[[ $(find "$CODEX_LOG_DIR" -name '*.last.txt' | wc -l) -eq 1 ]] || fail 'last message missing'
FAKE_RC=7 "$wrapper" --worktree "$wt" --role reviewer < "$scratch/input" > "$scratch/events" 2> "$scratch/err" && fail 'failed Codex passed' || rc=$?
[[ "$rc" == 7 ]] || fail 'Codex exit code not preserved'
for code in 0 7; do
    FAKE_RC=$code FAKE_TAMPER="$gd/config.worktree" "$wrapper" --worktree "$wt" --role writer --prompt-file "$scratch/input" > "$scratch/events" 2> "$scratch/err" && fail 'tampering passed' || true
    grep -q 'GITDIR TAMPERING DETECTED' "$scratch/err" || fail 'post-run tampering not reported'
    rm "$gd/config.worktree"
done
echo 'test_codex_exec: argv, refusals, streaming and post-run checks passed'

# Installer runs only in this throwaway main checkout, never in the real repo.
(cd "$repo" && scripts/dev/install-hooks.sh > /dev/null)
baseline=$(git -C "$repo" rev-parse main)
[[ $(git -C "$repo" config core.hooksPath) == "$repo/scripts/dev/hooks" ]] || fail 'absolute hook path'
hook="$repo/scripts/dev/hooks/pre-push"
hook_run() { (cd "$repo" && printf 'refs/heads/main %s refs/heads/main %s\n' "$1" "$2" | "$hook") > "$scratch/hook.out" 2>&1; }
zero=0000000000000000000000000000000000000000
hook_run "$baseline" "$zero" || fail 'baseline history rejected'
mkdir -p "$repo/docs"
echo status > "$repo/docs/STATUS.md"
git -C "$repo" add docs/STATUS.md
git -C "$repo" commit -qm status
status=$(git -C "$repo" rev-parse HEAD)
hook_run "$status" "$baseline" || fail 'STATUS-only commit rejected'
(cd "$repo" && scripts/dev/install-hooks.sh > /dev/null)
[[ $(git -C "$repo" config workflow.pushBaseline) == "$baseline" ]] || fail 'reinstall moved baseline'
echo bad > "$repo/code.txt"
git -C "$repo" add code.txt
git -C "$repo" commit -qm direct
bad=$(git -C "$repo" rev-parse HEAD)
if hook_run "$bad" "$status"; then fail 'direct code commit accepted'; fi
grep -q "$bad.*non-allowlisted" "$scratch/hook.out" || fail 'offending direct commit missing'
git -C "$repo" config workflow.directPaths 'code.txt'
hook_run "$bad" "$status" || fail 'configured direct path rejected'
git -C "$repo" config --unset workflow.directPaths
# Fixture branches form two real merges, one with and one without audit trailers.
git -C "$repo" switch -qc feature/one "$status"
echo one > "$repo/one"
git -C "$repo" add one && git -C "$repo" commit -qm one
git -C "$repo" switch -q main
git -C "$repo" reset --hard -q "$status"
git -C "$repo" merge --no-ff -q feature/one -m 'untrailered merge'
untrailered=$(git -C "$repo" rev-parse HEAD)
if hook_run "$untrailered" "$status"; then fail 'untrailered merge accepted'; fi
grep -q "$untrailered.*Writer, Reviewer, Risk" "$scratch/hook.out" || fail 'missing trailer keys not listed'
git -C "$repo" commit --amend -qm $'merge\n\nWriter: fixture\nReviewer: fixture\nRisk: high'
trailered=$(git -C "$repo" rev-parse HEAD)
hook_run "$trailered" "$status" || fail 'trailered merge rejected'
if hook_run "$status" "$trailered"; then fail 'non-fast-forward accepted'; fi
if hook_run "$zero" "$trailered"; then fail 'main deletion accepted'; fi
(cd "$repo" && printf 'refs/heads/feature/x %s refs/heads/feature/x %s\n' "$bad" "$zero" | "$hook") || fail 'other ref rejected'
echo 'test_codex_exec: hook policy and idempotent installer passed'
