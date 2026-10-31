#!/usr/bin/env bash
# Launch from trusted tooling; role fixes the sandbox, never bypasses it.
set -euo pipefail
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd -P)
SCRIPT_REPO=$(cd "$SCRIPT_DIR/../.." && pwd -P)
# Check before sourcing defaults or exporting/evaluating the gitdir validator.
[[ -d "$SCRIPT_REPO/.git" && ! -L "$SCRIPT_REPO/.git" ]] || {
    echo 'codex-exec: run from the main checkout' >&2
    exit 2
}
COMMON_DIR=$(cd "$SCRIPT_REPO/.git" && pwd -P)
# shellcheck source=agents.env
source "$SCRIPT_DIR/agents.env"
worktree= role= resume= last=0 prompt_file= no_commit=0 dry_run=0
extras=() images=()
die() { echo "codex-exec: $*" >&2; exit 2; }
while [[ $# -gt 0 ]]; do
    case "$1" in
        --worktree|--role|--model|--effort|--approval|--resume|--prompt-file|--image|--add-dir|--log-dir|--codex)
            [[ $# -ge 2 && -n "$2" ]] || die "$1 requires a value"
            case "$1" in
                --worktree) worktree=$2 ;; --role) role=$2 ;;
                --model) CODEX_MODEL=$2 ;; --effort) CODEX_EFFORT=$2 ;;
                --approval) CODEX_APPROVAL=$2 ;; --resume) resume=$2 ;;
                --prompt-file) prompt_file=$2 ;; --image) images+=("$2") ;;
                --add-dir) extras+=("$2") ;; --log-dir) CODEX_LOG_DIR=$2 ;;
                --codex) CODEX_BIN=$2 ;;
            esac
            shift 2 ;;
        --last) last=1; shift ;; --no-commit) no_commit=1; shift ;;
        --dry-run) dry_run=1; shift ;;
        --dangerously-*) die "sandbox bypass flags are forbidden" ;;
        *) die "unknown option: $1" ;;
    esac
done
[[ -n "$worktree" && -n "$role" ]] || die '--worktree and --role are required'
[[ "$role" == writer || "$role" == reviewer ]] || die 'role must be writer or reviewer'
[[ -z "$resume" || "$last" == 0 ]] || die '--resume and --last are mutually exclusive'
[[ "$role" == writer || ( ${#extras[@]} == 0 && "$no_commit" == 0 ) ]] || die '--add-dir and --no-commit require writer role'
case "$CODEX_APPROVAL" in never|on-request|on-failure|untrusted) ;; *) die "invalid approval policy: $CODEX_APPROVAL" ;; esac
for value in "$CODEX_MODEL" "$CODEX_EFFORT" "$CODEX_BIN" "$resume"; do
    [[ "$value" != danger-full-access && "$value" != --dangerously-* ]] || die 'unsafe option value'
done
realpath() { python3 -I -c 'import pathlib,sys; print(pathlib.Path(sys.argv[1]).resolve(strict=True))' "$1"; }
worktree=$(realpath "$worktree")
[[ "$worktree" != "$SCRIPT_REPO" ]] || die 'main worktree is forbidden'
# Capture the trusted validator before the agent can edit its target worktree.
eval "$(bash "$SCRIPT_DIR/agent-worktree.sh" --export-gitdir-check)"
verify_gitdir --path "$worktree" >/dev/null
# Only now may Git load the target. Use its canonical paths for sandbox grants.
gitdir=$(git -C "$worktree" rev-parse --absolute-git-dir)
common=$(git -C "$worktree" rev-parse --path-format=absolute --git-common-dir)
gitdir=$(realpath "$gitdir")
common=$(realpath "$common")
[[ "$gitdir" != "$common" ]] || die 'main worktree is forbidden'
[[ "$common" == "$COMMON_DIR" ]] || die 'unexpected common gitdir'
branch=$(git -C "$worktree" symbolic-ref --quiet --short HEAD) || die 'detached HEAD is forbidden'
[[ "$branch" != main && "$branch" != master && "$branch" == */* ]] || die 'branch must have a non-main/master namespace prefix (e.g. agent/task)'
roots=()
if [[ "$role" == writer && "$no_commit" == 0 ]]; then
    prefix=${branch%%/*}
    for parent in "$common/refs/heads" "$common/logs/refs/heads"; do
        # Do not turn a pre-existing symlink into a broader sandbox grant.
        python3 -I - "$parent/$prefix" <<'PY'
import pathlib, sys
p = pathlib.Path(sys.argv[1])
for part in (p, *p.parents):
    if part.is_symlink():
        sys.exit('codex-exec: symlink in git writable root: ' + str(part))
PY
        mkdir -p "$parent/$prefix"
    done
    roots+=("$gitdir" "$(realpath "$common/objects")" "$(realpath "$common/refs/heads/$prefix")" "$(realpath "$common/logs/refs/heads/$prefix")")
fi
for extra in "${extras[@]}"; do roots+=("$(realpath "$extra")"); done
for i in "${!images[@]}"; do images[$i]=$(realpath "${images[$i]}"); done
[[ -z "$prompt_file" ]] || prompt_file=$(realpath "$prompt_file")
if [[ "$CODEX_BIN" == */* ]]; then CODEX_BIN=$(realpath "$CODEX_BIN"); fi
mkdir -p "$CODEX_LOG_DIR"
CODEX_LOG_DIR=$(realpath "$CODEX_LOG_DIR")
stem="$CODEX_LOG_DIR/$(date -u +%Y%m%dT%H%M%SZ)-${branch//\//-}-$$"
sandbox=read-only
[[ "$role" != writer ]] || sandbox=workspace-write
# JSON strings/arrays are also TOML values; no interpolation into config syntax.
quote() { python3 -I -c 'import json,sys; print(json.dumps(sys.argv[1]))' "$1"; }
argv=("$CODEX_BIN" exec)
if [[ -n "$resume" || "$last" == 1 ]]; then
    argv+=(resume)
    [[ "$last" == 0 ]] || argv+=(--last)
    [[ -z "$resume" ]] || argv+=("$resume")
    argv+=(-c "model=$(quote "$CODEX_MODEL")" -c "sandbox_mode=$(quote "$sandbox")")
    if [[ ${#roots[@]} -gt 0 ]]; then
        encoded=$(python3 -I -c 'import json,sys; print(json.dumps(sys.argv[1:]))' "${roots[@]}")
        argv+=(-c "sandbox_workspace_write.writable_roots=$encoded")
    fi
else
    argv+=(-C "$worktree" -m "$CODEX_MODEL" -s "$sandbox")
    for root in "${roots[@]}"; do argv+=(--add-dir "$root"); done
fi
argv+=(-c "model_reasoning_effort=$(quote "$CODEX_EFFORT")" -c "approval_policy=$(quote "$CODEX_APPROVAL")" --json -o "$stem.last.txt")
for img in "${images[@]}"; do argv+=(--image "$img"); done
argv+=(-)
if [[ "$dry_run" == 1 ]]; then
    printf 'cwd=%s\n' "$worktree" >&2
    printf '%s\n' "${argv[@]}"
    exit 0
fi
prompt_tmp=$(mktemp)
finish() {
    rc=$?
    trap - EXIT
    rm -f "$prompt_tmp"
    if ! verify_gitdir "$branch" "$worktree" >/dev/null; then
        echo 'codex-exec: gitdir check failed: inspect before running git in this worktree' >&2
        rc=1
    fi
    exit "$rc"
}
trap finish EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
if [[ -n "$prompt_file" ]]; then
    cat "$prompt_file" > "$prompt_tmp"
else
    [[ ! -t 0 ]] || die 'provide --prompt-file or pipe a prompt (no TTY input)'
    cat > "$prompt_tmp"
fi
cd "$worktree"
# Stream events to disk and stdout, announcing the first thread id immediately.
"${argv[@]}" < "$prompt_tmp" | python3 -I -u -c '
import json, sys
seen = False
with open(sys.argv[1], "x") as log:
    for line in sys.stdin:
        log.write(line); log.flush()
        sys.stdout.write(line); sys.stdout.flush()
        try:
            event = json.loads(line)
        except ValueError:
            continue
        if not seen and event.get("type") == "thread.started":
            print("codex-exec: thread_id=" + event["thread_id"], file=sys.stderr, flush=True)
            seen = True
' "$stem.jsonl"
