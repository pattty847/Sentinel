#!/usr/bin/env bash
# Tests for agent-worktree.sh land evidence and sentinel-event.sh.
#   bash scripts/dev/test_agent_worktree.sh [source-root]
# Every case runs in a throwaway git repo under a temp dir with stub build-queue,
# chart-health, doc-health, cmake and ctest; HOME, CODEX_HOME, the worktree root, the evidence
# root and the event journal all point into that temp dir.
set -uo pipefail
SRC=$(cd "${1:-$(dirname "$0")/../..}" && pwd)
ORIG_PATH=$PATH
scratch=$(mktemp -d)
trap 'chmod -R u+rwx "$scratch" 2>/dev/null; rm -rf "$scratch"' EXIT
failures=0
case_name=setup

fail() { echo "FAIL [$case_name]: $*" >&2; failures=$((failures + 1)); }
expect() { local msg=$1; shift; "$@" || fail "$msg"; }
jget() { python3 -c 'import json,sys; v=json.load(open(sys.argv[1])); exec("r="+sys.argv[2]); print(json.dumps(r) if not isinstance(r,str) else r)' "$1" "$2"; }
load() { local d; eval "$1=()"; while IFS= read -r d; do eval "$1+=(\"\$d\")"; done < <(attempts); }
attempts() { find "$SENTINEL_LAND_EVIDENCE_DIR" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | sort; }
events_with() { if [[ -f "$SENTINEL_EVENTS_FILE" ]]; then grep -c "$1" "$SENTINEL_EVENTS_FILE"; else echo 0; fi; }
legacy_tmp_fallback() { grep -q '/tmp/sentinel-events' "$SRC/scripts/dev/sentinel-event.sh"; }
arm_after_verify() {
    export REAL_PYTHON=$(command -v python3) INJECT_ONCE="$T/injected" INJECT_PAYLOAD="$T/inject.sh"
    cat > "$T/bin/python3" <<'EOF'
#!/bin/sh
if [ "$1" = -I ] && [ "$2" = - ] && [ "${4:-}" = feat/x ] && [ ! -e "$INJECT_ONCE" ]; then
    "$REAL_PYTHON" "$@"
    rc=$?
    if [ "$rc" -eq 0 ]; then /bin/sh "$INJECT_PAYLOAD"; touch "$INJECT_ONCE"; fi
    exit "$rc"
fi
exec "$REAL_PYTHON" "$@"
EOF
    chmod +x "$T/bin/python3"
}
arm_filter_on_remove() {
    git -C "$repo" config extensions.worktreeConfig true
    printf 'file.txt filter=evil\n' > "$wt/.gitattributes"
    git -C "$wt" add .gitattributes
    git -C "$wt" commit -q -m 'select clean filter for tracked file'
    export REAL_GIT=$(command -v git) FILTER_WT=$(cd "$wt" && pwd -P) FILTER_GD=$(git -C "$wt" rev-parse --absolute-git-dir) FILTER_STATE="$T"
    printf '#!/bin/sh\ntouch "%s/filter-executed"\ncat\n' "$T" > "$T/filter-clean.sh"
    chmod +x "$T/filter-clean.sh"
    export FILTER_SCRIPT="$T/filter-clean.sh"
    cat > "$T/bin/git" <<'EOF'
#!/bin/sh
case " $* " in
    *' worktree remove '*)
        for arg in "$@"; do
            if [ "$arg" = "$FILTER_WT" ]; then
                printf '[filter "evil"]\n clean = %s\n required = true\n' "$FILTER_SCRIPT" > "$FILTER_GD/config.worktree"
                echo late >> "$FILTER_WT/file.txt"
                touch "$FILTER_STATE/planted"
                break
            fi
        done ;;
esac
exec "$REAL_GIT" "$@"
EOF
    chmod +x "$T/bin/git"
}

setup_case() {  # $1 case name; builds $T, $repo and a worktree for branch feat/x with one commit
    case_name=$1
    T="$scratch/$1"
    mkdir -p "$T/bin" "$T/home" "$T/codex" "$T/evidence" "$T/trees-mount"
    export HOME="$T/home" CODEX_HOME="$T/codex" SENTINEL_LAND_EVIDENCE_DIR="$T/evidence"
    export SENTINEL_WORKTREE_ROOT="$T/trees-mount/trees" SENTINEL_EVENTS_FILE="$T/home/events.jsonl"
    land_root="$T/trees-mount/.sentinel-land-trees-repo"
    export GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_NOSYSTEM=1
    export GIT_AUTHOR_NAME=t GIT_AUTHOR_EMAIL=t@example.invalid GIT_COMMITTER_NAME=t GIT_COMMITTER_EMAIL=t@example.invalid
    export PATH="$T/bin:$ORIG_PATH"
    printf '#!/bin/sh\necho "stub cmake $*"\n' > "$T/bin/cmake"
    cat > "$T/bin/ctest" <<'EOF'
#!/bin/sh
if [ "${STUB_CTEST_FAIL:-0}" = 1 ]; then echo "50% tests passed, 1 tests failed out of 2"; exit 8; fi
echo "100% tests passed, 0 tests failed out of 2"
EOF
    chmod +x "$T/bin/cmake" "$T/bin/ctest"
    repo="$T/repo"
    git init -q -b main "$repo"
    mkdir -p "$repo/scripts/dev"
    cp "$SRC/scripts/dev/agent-worktree.sh" "$SRC/scripts/dev/sentinel-event.sh" "$repo/scripts/dev/"
    cp "$SRC/.gitignore" "$repo/.gitignore"
    printf '#!/usr/bin/env bash\nwhile [[ $# -gt 0 && $1 != -- ]]; do shift; done; shift; exec "$@"\n' > "$repo/scripts/dev/build-queue.sh"
    printf '#!/bin/sh\n[ ! -e build/mac-clang/build.ninja ] || touch "$STUB_HEALTH_PROBE"\nexit 0\n' > "$repo/scripts/dev/chart-health.sh"
    printf '#!/bin/sh\n[ ! -e build/mac-clang/build.ninja ] || touch "$STUB_HEALTH_PROBE"\nexit "${STUB_DOC_HEALTH_STATUS:-0}"\n' > "$repo/scripts/dev/doc-health.sh"
    chmod +x "$repo/scripts/dev/"*.sh
    seq 1 10 > "$repo/file.txt"
    git -C "$repo" add -A && git -C "$repo" commit -q -m base
    (cd "$repo" && scripts/dev/agent-worktree.sh create feat/x > /dev/null 2>&1) || fail "create failed"
    wt="$SENTINEL_WORKTREE_ROOT/feat-x"
    perl -pi -e 's/^5$/five/' "$wt/file.txt"
    git -C "$wt" commit -q -am "feature"
}

land() { (cd "$repo" && scripts/dev/agent-worktree.sh land feat/x --trailer "Writer: fixture" --trailer "Reviewer: fixture" --trailer "Risk: high" "$@") > "$T/land.out" 2>&1; }

# Security preflight uses filesystem reads before worktree Git loads any config.
setup_case gitdir
verify() { (cd "$repo" && scripts/dev/agent-worktree.sh verify-gitdir feat/x) > "$T/verify.out" 2>&1; }
gd=$(git -C "$wt" rev-parse --absolute-git-dir)
git -C "$repo" config extensions.worktreeConfig true
expect "fresh gitdir passes" verify
"$wt/scripts/dev/agent-worktree.sh" --export-gitdir-check > "$T/linked.out" 2>&1
expect "linked tooling checkout refused" [ "$?" -ne 0 ]
expect "linked tooling refusal explains required checkout" grep -q 'run from the main checkout' "$T/linked.out"
printf '[core]\n fsmonitor = touch %s/executed\n' "$T" > "$gd/config.worktree"
verify; expect "config.worktree rejected" [ "$?" -ne 0 ]
expect "config named" grep -q config.worktree "$T/verify.out"
land; expect "land rejects config" [ "$?" -ne 0 ]
expect "config not executed" [ ! -e "$T/executed" ]
expect "land names config" grep -q config.worktree "$T/land.out"
expect "rebase not reached" [ ! -d "$gd/rebase-merge" ]
rm "$gd/config.worktree"
mkdir -p "$T/hooks"
printf '#!/bin/sh\ntouch "%s/hook-executed"\n' "$T" > "$T/hooks/pre-rebase"
chmod +x "$T/hooks/pre-rebase"
printf '[core]\n hooksPath = %s/hooks\n' "$T" > "$gd/config.worktree"
land; expect "land rejects hooksPath config" [ "$?" -ne 0 ]
expect "planted hook not run" [ ! -e "$T/hook-executed" ]
rm "$gd/config.worktree"
mkdir "$gd/rebase-merge"
verify; expect "rebase state rejected" [ "$?" -ne 0 ]
rmdir "$gd/rebase-merge"
cp "$gd/commondir" "$T/common.saved"
echo "$T" > "$gd/commondir"
verify; expect "rewritten commondir rejected" [ "$?" -ne 0 ]
cp "$T/common.saved" "$gd/commondir"
touch "$repo/.git/objects/info/alternates"
verify; expect "alternates rejected" [ "$?" -ne 0 ]
rm "$repo/.git/objects/info/alternates"
cp "$gd/gitdir" "$T/gitdir.saved"
echo "$repo/.git" > "$gd/gitdir"
verify; expect "rewritten backlink rejected" [ "$?" -ne 0 ]
cp "$T/gitdir.saved" "$gd/gitdir"
rm -f "$gd/ORIG_HEAD"
ln -s "$T/common.saved" "$gd/ORIG_HEAD"
verify; expect "symlink rejected" [ "$?" -ne 0 ]
rm "$gd/ORIG_HEAD"
expect "restored gitdir passes" verify
original_main=$(git -C "$repo" rev-parse main)
for missing in Writer Reviewer Risk all; do
    args=()
    for key in Writer Reviewer Risk; do
        [[ "$key" == "$missing" || "$missing" == all ]] || args+=(--trailer "$key: fixture")
    done
    (cd "$repo" && scripts/dev/agent-worktree.sh land feat/x "${args[@]}") > "$T/missing.out" 2>&1
    expect "missing $missing exits 2" [ "$?" -eq 2 ]
    expect "missing keys named" grep -q 'missing required land trailers:' "$T/missing.out"
    expect "main unchanged" [ "$(git -C "$repo" rev-parse main)" = "$original_main" ]
done

# Compare the writer's index and tracked files without loading its Git config.
setup_case dirty-writer
main_before=$(git -C "$repo" rev-parse main)
echo dirty >> "$wt/file.txt"
land; rc=$?
expect "tracked writer change rejected" [ "$rc" -ne 0 ]
expect "dirty writer error" grep -q 'has uncommitted changes' "$T/land.out"
expect "dirty writer not merged" [ "$(git -C "$repo" rev-parse main)" = "$main_before" ]
expect "dirty writer not checked out" [ ! -e "$land_root" ]
setup_case staged-writer
echo staged >> "$wt/file.txt"
git -C "$wt" add file.txt
land; rc=$?
expect "staged writer change rejected" [ "$rc" -ne 0 ]
expect "staged writer error" grep -q 'has uncommitted changes' "$T/land.out"

# Build and health gates must never read ignored files left by the writer.
setup_case clean-checkout
export STUB_HEALTH_PROBE="$T/health-poison-ran" STUB_BUILD_PROBE="$T/build-poison-ran"
cat > "$T/bin/cmake" <<'EOF'
#!/bin/sh
[ ! -e build/mac-clang/build.ninja ] || touch "$STUB_BUILD_PROBE"
echo "stub cmake $*"
EOF
cat > "$T/bin/ctest" <<'EOF'
#!/bin/sh
[ ! -e build/mac-clang/build.ninja ] || touch "$STUB_BUILD_PROBE"
echo "100% tests passed, 0 tests failed out of 2"
exit "${STUB_CTEST_FAIL:-0}"
EOF
chmod +x "$T/bin/cmake" "$T/bin/ctest"
mkdir -p "$wt/build/mac-clang"
echo 'touch poison-marker' > "$wt/build/mac-clang/build.ninja"
STUB_CTEST_FAIL=1 land; rc=$?
expect "fixture ctest fails before merge" [ "$rc" -ne 0 ]
expect "health ignored writer build.ninja" [ ! -e "$STUB_HEALTH_PROBE" ]
expect "build and ctest ignored writer build.ninja" [ ! -e "$STUB_BUILD_PROBE" ]
expect "failed land removes clean checkout" [ ! -e "$land_root" ]
expect "failed land keeps writer worktree" [ -d "$wt" ]
root="$land_root"
mkdir "$root"
printf '%s\n' "$(cd "$repo/.git" && pwd -P)" > "$root/owner"
git -C "$repo" worktree add -q --detach "$root/tree" "$(git -C "$wt" rev-parse HEAD)"
mkdir -p "$root/tree/build/mac-clang"
echo stale > "$root/tree/build/mac-clang/build.ninja"
STUB_CTEST_FAIL=1 land; rc=$?
expect "fixture ctest fails after stale checkout cleanup" [ "$rc" -ne 0 ]
expect "stale checkout ignored by build" [ ! -e "$STUB_BUILD_PROBE" ]
expect "stale checkout removed after failure: $(tail -8 "$T/land.out")" [ ! -e "$root" ]
unset STUB_HEALTH_PROBE STUB_BUILD_PROBE

# A config.worktree planted immediately after the filesystem verifier must
# never be loaded by a Git command in the writer worktree.
setup_case config-race
echo main-only > "$repo/main-only.txt"
git -C "$repo" add main-only.txt
git -C "$repo" commit -q -m 'main moves before config race'
main_before=$(git -C "$repo" rev-parse main)
git -C "$repo" config extensions.worktreeConfig true
gd=$(git -C "$wt" rev-parse --absolute-git-dir)
mkdir "$T/hooks"
printf '#!/bin/sh\ntouch "%s/hook-executed"\n' "$T" > "$T/hooks/pre-rebase"
chmod +x "$T/hooks/pre-rebase"
cat > "$T/inject.sh" <<EOF
printf '[core]\n fsmonitor = touch $T/fsmonitor-executed\n hooksPath = $T/hooks\n' > "$gd/config.worktree"
EOF
arm_after_verify
land; rc=$?
expect "post-verification config rejected" [ "$rc" -ne 0 ]
expect "fsmonitor never executed" [ ! -e "$T/fsmonitor-executed" ]
expect "writer hook never executed" [ ! -e "$T/hook-executed" ]
expect "config change named" grep -q config.worktree "$T/land.out"
expect "config race did not merge" [ "$(git -C "$repo" rev-parse main)" = "$main_before" ]

# A missing or untrusted conductor checkout root must never be adopted.
for variant in symlink unmarked bad-marker; do
    setup_case "root-$variant"
    main_before=$(git -C "$repo" rev-parse main)
    case $variant in
        symlink) mkdir "$T/symlink-target"; ln -s "$T/symlink-target" "$land_root" ;;
        unmarked) mkdir "$land_root"; touch "$land_root/foreign" ;;
        bad-marker) mkdir "$land_root"; echo wrong > "$land_root/owner" ;;
    esac
    land; rc=$?
    expect "$variant land root rejected" [ "$rc" -ne 0 ]
    load dirs
    expect "$variant not built" [ ! -e "${dirs[0]:-/missing}/build.log" ]
    expect "$variant root kept" [ -e "$land_root" ]
    expect "$variant did not merge" [ "$(git -C "$repo" rev-parse main)" = "$main_before" ]
done

# A deleted checkout with a surviving Git registration is pruned before add.
setup_case stale-registration
mkdir "$land_root"
printf '%s\n' "$(cd "$repo/.git" && pwd -P)" > "$land_root/owner"
git -C "$repo" worktree add -q --detach "$land_root/tree" "$(git -C "$wt" rev-parse HEAD)"
rm -rf "$land_root"
STUB_CTEST_FAIL=1 land; rc=$?
expect "land reaches ctest after stale registration" [ "$rc" -ne 0 ]
expect "stale registration removed" [ "$(git -C "$repo" worktree list --porcelain | grep -Fc "$(basename "$land_root")" || true)" -eq 0 ]
expect "land root removed after failure" [ ! -e "$land_root" ]

# An object whose loose bytes do not match its name must block checkout.
setup_case corrupt-object
main_before=$(git -C "$repo" rev-parse main)
blob=$(git -C "$repo" rev-parse 'HEAD:scripts/dev/chart-health.sh')
python3 - "$repo/.git/objects/${blob:0:2}/${blob:2}" <<'PY'
from pathlib import Path
import os, sys, zlib
data = b'#!/bin/sh\necho forged\nexit 0\n'
os.chmod(sys.argv[1], 0o644)
Path(sys.argv[1]).write_bytes(zlib.compress(b'blob ' + str(len(data)).encode() + b'\0' + data))
PY
land; rc=$?
expect "corrupt object rejected" [ "$rc" -ne 0 ]
expect "fsck names corruption" grep -Eq 'object integrity check failed|corrupt|hash mismatch' "$T/land.out"
expect "corrupt object not checked out" [ ! -e "$land_root" ]
expect "corrupt object not merged" [ "$(git -C "$repo" rev-parse main)" = "$main_before" ]

# Change a shared blob after verification, then restore it before the old
# second fsck (or the new import). Only the private checkout has stable bytes.
setup_case swap-after-verification
blob=$(git -C "$repo" rev-parse 'HEAD:scripts/dev/chart-health.sh')
export SWAP_OBJECT="$repo/.git/objects/${blob:0:2}/${blob:2}" SWAP_REPO=$(cd "$repo" && pwd -P) SWAP_ROOT="$land_root"
export SWAP_BACKUP="$T/original-object" SWAP_MARKER="$T/forged-executed" SWAP_STATE="$T"
cp "$SWAP_OBJECT" "$SWAP_BACKUP"
cat > "$T/swap.py" <<'PY'
from pathlib import Path
import os, sys, zlib
target = Path(os.environ['SWAP_OBJECT'])
os.chmod(target, 0o644)
if sys.argv[1] == 'poison':
    script = f'#!/bin/sh\ntouch "{os.environ["SWAP_MARKER"]}"\nexit 0\n'.encode()
    target.write_bytes(zlib.compress(b'blob ' + str(len(script)).encode() + b'\0' + script))
else:
    target.write_bytes(Path(os.environ['SWAP_BACKUP']).read_bytes())
PY
export REAL_GIT=$(command -v git)
cat > "$T/bin/git" <<'EOF'
#!/bin/sh
case " $* " in
    *' worktree add --detach '*)
        for arg in "$@"; do
            if [ "$arg" = "$SWAP_ROOT/tree" ] && [ ! -e "$SWAP_STATE/poisoned" ]; then
                python3 "$SWAP_STATE/swap.py" poison
                touch "$SWAP_STATE/poisoned"
            fi
        done ;;
esac
if [ -e "$SWAP_STATE/poisoned" ] && [ ! -e "$SWAP_STATE/restored" ] &&
    [ "$1" = -C ] && [ "$2" = "$SWAP_REPO" ] && { [ "$3" = fsck ] || [ "$3" = fetch ]; }; then
    python3 "$SWAP_STATE/swap.py" restore
    touch "$SWAP_STATE/restored"
fi
exec "$REAL_GIT" "$@"
EOF
chmod +x "$T/bin/git"
land; rc=$?
expect "private-store land succeeds ($rc): $(tail -5 "$T/land.out")" [ "$rc" -eq 0 ]
expect "shared object swap occurred" [ -e "$T/poisoned" ]
expect "shared object restored before import" [ -e "$T/restored" ]
expect "forged blob never executed" [ ! -e "$SWAP_MARKER" ]
expect "private checkout removed after swap" [ ! -e "$land_root" ]

# Object alternates in the writer-writable object store are forbidden.
setup_case land-alternates
main_before=$(git -C "$repo" rev-parse main)
mkdir -p "$T/foreign-objects/info"
echo "$T/foreign-objects" > "$repo/.git/objects/info/alternates"
land; rc=$?
expect "land rejects alternates" [ "$rc" -ne 0 ]
expect "alternates named" grep -q alternates "$T/land.out"
expect "alternates not merged" [ "$(git -C "$repo" rev-parse main)" = "$main_before" ]

setup_case alternates-race
main_before=$(git -C "$repo" rev-parse main)
mkdir -p "$T/foreign-objects/info"
cat > "$T/inject.sh" <<EOF
echo "$T/foreign-objects" > "$repo/.git/objects/info/alternates"
EOF
arm_after_verify
land; rc=$?
expect "post-verification alternates rejected" [ "$rc" -ne 0 ]
expect "post-verification alternates named" grep -q alternates "$T/land.out"
expect "alternate store not checked out" [ ! -e "$land_root" ]
expect "alternate store not merged" [ "$(git -C "$repo" rev-parse main)" = "$main_before" ]

# Doc-health breaches block land; only exit 1 can be waived.
setup_case doc-health
original_main=$(git -C "$repo" rev-parse main)
STUB_DOC_HEALTH_STATUS=1 land; rc=$?
expect "doc-health blocks land" [ "$rc" -eq 1 ]
expect "main unchanged" [ "$(git -C "$repo" rev-parse main)" = "$original_main" ]
expect "doc-health failure reported" grep -q 'doc-health failed (exit 1)' "$T/land.out"
load dirs
rec="${dirs[0]:-/missing}/landing.json"
expect "doc-health recorded as failing gate" [ "$(jget "$rec" 'v["gate_reached"]')" = doc-health ]
expect "build not reached" [ ! -e "${dirs[0]:-/missing}/build.log" ]
STUB_DOC_HEALTH_STATUS=2 land --trailer 'Doc-Health-Waiver: fixture'; rc=$?
expect "tool errors cannot be waived" [ "$rc" -eq 2 ]
expect "main unchanged after tool error" [ "$(git -C "$repo" rev-parse main)" = "$original_main" ]
STUB_DOC_HEALTH_STATUS=1 land --trailer 'Doc-Health-Waiver: fixture'; rc=$?
expect "doc-health waiver permits land: $(tail -5 "$T/land.out")" [ "$rc" -eq 0 ]
expect "waiver reported" grep -q 'doc-health: waived by Doc-Health-Waiver trailer' "$T/land.out"
expect "waived feature merged" [ "$(git -C "$repo" rev-parse main)" != "$original_main" ]

# 1 + 9: successful land keeps logs and landing.json; Codex candidates by branch only.
setup_case success
sess="$CODEX_HOME/sessions/2026/10/14"
mkdir -p "$sess"
printf '{"timestamp":"2026-10-14T01:00:00Z","type":"session_meta","payload":{"id":"sess-match","timestamp":"2026-10-14T01:00:00Z","cwd":"%s","git":{"branch":"feat/x"}}}\n' "$wt" > "$sess/rollout-a.jsonl"
printf '{"timestamp":"2026-10-14T02:00:00Z","type":"session_meta","payload":{"id":"sess-other","cwd":"/x","git":{"branch":"feat/y"}}}\n' > "$sess/rollout-b.jsonl"
printf '{"timestamp":"2026-08-01T02:00:00Z","type":"session_meta","payload":{"id":"sess-old","cwd":"/x","git":{"branch":"feat/x"}}}\n' > "$sess/rollout-c.jsonl"
touch -t 202608010000 "$sess/rollout-c.jsonl"
mkdir -p "$T/evidence/20200101T000000Z-old-1" "$T/evidence/20200101T000000Z-nolanding-2" "$T/evidence/notes"
echo '{}' > "$T/evidence/20200101T000000Z-old-1/landing.json"
land --trailer 'Slice: S1' --trailer 'Risk: medium'; rc=$?
expect "exit 0 (got $rc): $(tail -5 "$T/land.out")" [ "$rc" -eq 0 ]
case_name=retention
expect "expired attempt removed" [ ! -e "$T/evidence/20200101T000000Z-old-1" ]
expect "dir without landing.json kept" [ -d "$T/evidence/20200101T000000Z-nolanding-2" ]
expect "foreign dir kept" [ -d "$T/evidence/notes" ]
rm -rf "$T/evidence/20200101T000000Z-nolanding-2" "$T/evidence/notes"
case_name=success
load dirs
expect "one attempt dir" [ "${#dirs[@]}" -eq 1 ]
rec="${dirs[0]:-/missing}/landing.json"
expect "landing.json exists" [ -f "$rec" ]
expect "outcome landed" [ "$(jget "$rec" 'v["outcome"]')" = landed ]
expect "merge_sha is main" [ "$(jget "$rec" 'v["merge_sha"]')" = "$(git -C "$repo" rev-parse main)" ]
expect "schema 1" [ "$(jget "$rec" 'v["schema"]')" = 1 ]
expect "trailers" [ "$(jget "$rec" 'v["trailers"]')" = '["Writer: fixture", "Reviewer: fixture", "Risk: high", "Slice: S1", "Risk: medium"]' ]
expect "ctest summary" [ "$(jget "$rec" 'v["ctest_summary"]')" = "100% tests passed, 0 tests failed out of 2" ]
expect "build log kept" [ -s "${dirs[0]:-/missing}/build.log" ]
expect "ctest log kept" grep -q "tests passed" "${dirs[0]:-/missing}/ctest.log"
expect "logs listed" [ "$(jget "$rec" 'v["logs"]')" = '["build.log", "ctest.log"]' ]
expect "gate_reached remove" [ "$(jget "$rec" 'v["gate_reached"]')" = remove ]
expect "gates all ok" [ "$(jget "$rec" 'sorted({g["result"] for g in v["gates"]})')" = '["ok"]' ]
expect "worktree removed" [ ! -e "$wt" ]
expect "successful land removes clean checkout" [ ! -e "$land_root" ]
expect "successful land clears checkout registration" [ "$(git -C "$repo" worktree list --porcelain | grep -Fc "$(basename "$land_root")" || true)" -eq 0 ]
expect "landed event" [ "$(events_with '"kind":"landed"')" -eq 1 ]
expect "landed event carries slice" grep -q '"slice":"S1"' "$SENTINEL_EVENTS_FILE"
case_name=codex-candidates
expect "matching session listed" [ "$(jget "$rec" '[c["session_id"] for c in v["codex_session_candidates"]]')" = '["sess-match"]' ]
expect "candidate is branch match only" [ "$(jget "$rec" 'v["codex_session_candidates"][0]["match"]')" = branch ]
expect "no role written" [ "$(jget "$rec" 'any("role" in c for c in v["codex_session_candidates"])')" = false ]
example_record=$rec

# 2: failed land keeps evidence and the worktree.
setup_case failed
STUB_CTEST_FAIL=1 land; rc=$?
expect "exit 1 (got $rc)" [ "$rc" -eq 1 ]
load dirs
rec="${dirs[0]:-/missing}/landing.json"
expect "outcome land_failed" [ "$(jget "$rec" 'v["outcome"]')" = land_failed ]
expect "exit_code preserved" [ "$(jget "$rec" 'v["exit_code"]')" = 1 ]
expect "gate_reached ctest" [ "$(jget "$rec" 'v["gate_reached"]')" = ctest ]
expect "merge_sha null" [ "$(jget "$rec" 'v["merge_sha"]')" = null ]
expect "ctest log kept" grep -q "1 tests failed" "${dirs[0]:-/missing}/ctest.log"
expect "build log kept" [ -f "${dirs[0]:-/missing}/build.log" ]
expect "worktree kept" [ -d "$wt" ]
expect "main not merged" [ "$(git -C "$repo" rev-list --count main)" -eq 1 ]
expect "land_failed event" [ "$(events_with '"kind":"land_failed"')" -eq 1 ]

# 3 + 5: fail then succeed gives two dirs, first unchanged; event transfer is deduplicated.
setup_case retry
printf '{"id":"evt-1","at":1000,"kind":"review","text":"r1","tone":"info"}\n{"at":2000,"kind":"plan","text":"no id","tone":"info"}\n' > "$wt/.sentinel-events.jsonl"
STUB_CTEST_FAIL=1 land; rc1=$?
load first
before=$(cd "${first[0]:-/}" && shasum landing.json ctest.log build.log 2>/dev/null)
land; rc2=$?
expect "first attempt failed ($rc1)" [ "$rc1" -eq 1 ]
expect "second attempt landed ($rc2): $(tail -3 "$T/land.out")" [ "$rc2" -eq 0 ]
load dirs
expect "two distinct attempt dirs" [ "${#dirs[@]}" -eq 2 ]
expect "first attempt unchanged" [ "$(cd "${first[0]:-/}" && shasum landing.json ctest.log build.log 2>/dev/null)" = "$before" ]
second=$(for d in "${dirs[@]}"; do [[ "$d" == "${first[0]:-}" ]] || echo "$d"; done)
case_name=dedup
expect "evt-1 once in journal" [ "$(events_with '"id":"evt-1"')" -eq 1 ]
expect "id-less line once in journal" [ "$(events_with '"text":"no id"')" -eq 1 ]
expect "second transfer skipped duplicates" [ "$(jget "$second/landing.json" '[v["events_transfer"][k] for k in ("status","lines_read","appended","duplicates_skipped")]')" = '["ok", 2, 0, 2]' ]

# 4: merge succeeds, event transfer fails -> landed_evidence_incomplete, exit 4, worktree kept.
setup_case evidence-fault
printf '{"id":"evt-x","at":1000,"kind":"review","text":"r","tone":"info"}\n' > "$wt/.sentinel-events.jsonl"
chmod 000 "$wt/.sentinel-events.jsonl"
land; rc=$?
chmod 644 "$wt/.sentinel-events.jsonl"
expect "exit 4 (got $rc)" [ "$rc" -eq 4 ]
load dirs
rec="${dirs[0]:-/missing}/landing.json"
expect "outcome landed_evidence_incomplete" [ "$(jget "$rec" 'v["outcome"]')" = landed_evidence_incomplete ]
expect "merge_sha recorded" [ "$(jget "$rec" 'v["merge_sha"]')" = "$(git -C "$repo" rev-parse main)" ]
expect "transfer failed recorded" [ "$(jget "$rec" 'v["events_transfer"]["status"]')" = failed ]
expect "problems recorded" [ "$(jget "$rec" 'len(v["evidence_problems"]) > 0')" = true ]
expect "exactly one merge on main" [ "$(git -C "$repo" rev-list --merges --count main)" -eq 1 ]
expect "worktree kept" [ -d "$wt" ]
expect "no land_failed event" [ "$(events_with '"kind":"land_failed"')" -eq 0 ]
expect "warn event" [ "$(events_with '"kind":"landed_evidence_incomplete","text":"[^"]*","tone":"warn"')" -eq 1 ]
expect "finish instruction printed" grep -q "agent-worktree.sh remove feat/x" "$T/land.out"

# 4b: merge succeeds, worktree remove fails -> still reported as merged, never land_failed.
setup_case remove-fault
export REAL_GIT=$(command -v git)
cat > "$T/bin/git" <<'EOF'
#!/bin/sh
case " $* " in
    *' worktree remove --force '*)
        if [ "${STUB_REMOVE_FAIL:-0}" = 1 ]; then echo 'stub worktree remove failed' >&2; exit 9; fi ;;
esac
exec "$REAL_GIT" "$@"
EOF
chmod +x "$T/bin/git"
STUB_REMOVE_FAIL=1 land; rc=$?
expect "exit 4 (got $rc)" [ "$rc" -eq 4 ]
load dirs
rec="${dirs[0]:-/missing}/landing.json"
expect "outcome landed_evidence_incomplete" [ "$(jget "$rec" 'v["outcome"]')" = landed_evidence_incomplete ]
expect "gate_reached remove" [ "$(jget "$rec" 'v["gate_reached"]')" = remove ]
expect "exactly one merge on main" [ "$(git -C "$repo" rev-list --merges --count main)" -eq 1 ]
expect "worktree kept" [ -d "$wt" ]
expect "no land_failed event" [ "$(events_with '"kind":"land_failed"')" -eq 0 ]

# A filter planted after the last gitdir check must not run during either removal.
setup_case land-remove-filter
arm_filter_on_remove
land; rc=$?
expect "forced land removal succeeds ($rc): $(tail -5 "$T/land.out")" [ "$rc" -eq 0 ]
expect "land removal planted filter" [ -e "$T/planted" ]
expect "land removal did not run clean filter" [ ! -e "$T/filter-executed" ]
expect "land removed writer worktree" [ ! -e "$wt" ]
setup_case manual-remove-filter
arm_filter_on_remove
(cd "$repo" && scripts/dev/agent-worktree.sh remove feat/x) > "$T/remove.out" 2>&1; rc=$?
expect "forced manual removal succeeds ($rc): $(tail -5 "$T/remove.out")" [ "$rc" -eq 0 ]
expect "manual removal planted filter" [ -e "$T/planted" ]
expect "manual removal did not run clean filter" [ ! -e "$T/filter-executed" ]
expect "manual removal removed writer worktree" [ ! -e "$wt" ]

# 4c: journal unwritable and no sandbox events: the landed event cannot be recorded -> exit 4, worktree kept.
setup_case journal-unwritable
mkdir -p "$T/ro"; chmod 555 "$T/ro"
SENTINEL_EVENTS_FILE="$T/ro/events.jsonl" land; rc=$?
expect "exit 4 (got $rc)" [ "$rc" -eq 4 ]
load dirs
rec="${dirs[0]:-/missing}/landing.json"
expect "outcome landed_evidence_incomplete" [ "$(jget "$rec" 'v["outcome"]')" = landed_evidence_incomplete ]
expect "event problem recorded" [ "$(jget "$rec" 'any("outcome event landed" in p for p in v["evidence_problems"])')" = true ]
expect "exactly one merge on main" [ "$(git -C "$repo" rev-list --merges --count main)" -eq 1 ]
expect "worktree kept" [ -d "$wt" ]

# 4d: record write fails after the merge (post-merge hook makes the attempt dir read-only) -> exit 4.
setup_case record-fault
printf '#!/bin/sh
chmod 555 "$SENTINEL_LAND_EVIDENCE_DIR"/*
' > "$repo/.git/hooks/post-merge"
chmod +x "$repo/.git/hooks/post-merge"
land; rc=$?
chmod -R u+w "$SENTINEL_LAND_EVIDENCE_DIR"
expect "exit 4 (got $rc)" [ "$rc" -eq 4 ]
expect "exactly one merge on main" [ "$(git -C "$repo" rev-list --merges --count main)" -eq 1 ]
expect "worktree kept" [ -d "$wt" ]
expect "no land_failed event" [ "$(events_with '"kind":"land_failed"')" -eq 0 ]
expect "no landed event" [ "$(events_with '"kind":"landed"')" -eq 0 ]
expect "record failure on stderr" grep -q "landing.json" "$T/land.out"

# 4e: invalid sandbox event lines: nothing transferred, source kept, merged attempt exits 4.
for variant in truncated missing-field bad-tone nan; do
    setup_case "invalid-$variant"
    good='{"id":"evt-good","at":1000,"kind":"review","text":"ok","tone":"info"}'
    case $variant in
        truncated) printf '%s\n{"id":"evt-cut","at":2000,"kind":"rev' "$good" ;;
        missing-field) printf '%s\n{"id":"evt-nokind","at":2000,"text":"t","tone":"info"}\n' "$good" ;;
        bad-tone) printf '%s\n{"id":"evt-tone","at":2000,"kind":"k","text":"t","tone":"loud"}\n' "$good" ;;
        nan) printf '%s\n{"id":"bad-number","at":NaN,"kind":"review","text":"note","tone":"info"}\n' "$good" ;;
    esac > "$wt/.sentinel-events.jsonl"
    cp "$wt/.sentinel-events.jsonl" "$T/source.copy"
    land; rc=$?
    expect "exit 4 (got $rc)" [ "$rc" -eq 4 ]
    load dirs
    rec="${dirs[0]:-/missing}/landing.json"
    expect "transfer failed" [ "$(jget "$rec" 'v["events_transfer"]["status"]')" = failed ]
    expect "nothing appended" [ "$(events_with '"id":"evt-good"')" -eq 0 ]
    expect "source kept unchanged" cmp -s "$wt/.sentinel-events.jsonl" "$T/source.copy"
    expect "worktree kept" [ -d "$wt" ]
done

# 4f: unexpected failure after the merge, before removal (a `date` stub fails on the gate-remove
# call; a post-merge hook arms it) -> merged attempt is never reported landed or land_failed.
setup_case unexpected-fault
printf '#!/bin/sh\ntouch "%s"\n' "$T/merged" > "$repo/.git/hooks/post-merge"
cat > "$T/bin/date" <<EOF
#!/bin/sh
# Second date call after the merge is gate remove (gate evidence is the first).
if [ -e "$T/merged" ]; then
    n=\$(( \$(cat "$T/merged.count" 2>/dev/null || echo 0) + 1 )); echo \$n > "$T/merged.count"
    [ \$n -eq 2 ] && exit 1
fi
exec /bin/date "\$@"
EOF
chmod +x "$repo/.git/hooks/post-merge" "$T/bin/date"
land; rc=$?
expect "date stub fired" [ "$(cat "$T/merged.count" 2>/dev/null || echo 0)" -ge 2 ]
expect "exit 4 (got $rc)" [ "$rc" -eq 4 ]
expect "exactly one merge on main" [ "$(git -C "$repo" rev-list --merges --count main)" -eq 1 ]
expect "worktree kept" [ -d "$wt" ]
expect "no land_failed event" [ "$(events_with '"kind":"land_failed"')" -eq 0 ]
expect "warn event" [ "$(events_with '"kind":"landed_evidence_incomplete"')" -eq 1 ]
load dirs
rec="${dirs[0]:-/missing}/landing.json"
expect "outcome landed_evidence_incomplete" [ "$(jget "$rec" 'v["outcome"]')" = landed_evidence_incomplete ]

# 2b: the embedded helper never imports modules from the caller's cwd or PYTHONPATH.
setup_case python-isolation
mkdir -p "$T/pypath"
for mod in json glob; do
    printf 'open(%s, "a").write("%s\\n")\n' "'$T/imported.marker'" "$mod" > "$repo/$mod.py"
    cp "$repo/$mod.py" "$T/pypath/$mod.py"
done
PYTHONPATH="$T/pypath" land; rc=$?
expect "land exit 0 (got $rc): $(tail -3 "$T/land.out")" [ "$rc" -eq 0 ]
expect "no shadow module imported" [ ! -e "$T/imported.marker" ]

# 6: sandbox: primary journal unwritable -> worktree file; land moves it into the journal.
setup_case sandbox
if legacy_tmp_fallback; then
    fail "sentinel-event.sh still falls back to /tmp (not run, to keep the real /tmp journal clean)"
else
    mkdir -p "$T/ro"; chmod 555 "$T/ro"
    SENTINEL_EVENTS_FILE="$T/ro/events.jsonl" bash -c 'cd "$1" && scripts/dev/sentinel-event.sh review info "writer note" --branch feat/x' _ "$wt"; rc=$?
    expect "fallback write exits 0 (got $rc)" [ "$rc" -eq 0 ]
    expect "event in worktree file" grep -q '"text":"writer note"' "$wt/.sentinel-events.jsonl"
    id=$(python3 -c 'import json,sys; print(json.loads(open(sys.argv[1]).readline())["id"])' "$wt/.sentinel-events.jsonl" 2>/dev/null)
    land; rc=$?
    expect "land exit 0 (got $rc): $(tail -3 "$T/land.out")" [ "$rc" -eq 0 ]
    expect "fallback event has an id" [ -n "$id" ]
    expect "event id in primary journal" [ "$(events_with "\"id\":\"${id:-none}\"")" -eq 1 ]
    expect "worktree removed" [ ! -e "$wt" ]
fi

# 7: no writable location -> non-zero exit with a stderr message.
case_name=no-writable
if legacy_tmp_fallback; then
    fail "sentinel-event.sh still falls back to /tmp (not run)"
else
    mkdir -p "$T/ro2/nowhere"; chmod 555 "$T/ro2/nowhere" "$T/ro2"
    err=$(cd "$T" && SENTINEL_EVENTS_FILE="$T/ro2/nowhere/events.jsonl" "$repo/scripts/dev/sentinel-event.sh" note info "lost" 2>&1 >/dev/null); rc=$?
    expect "outside a worktree exits non-zero" [ "$rc" -ne 0 ]
    expect "stderr names the problem" grep -q "sentinel-event: cannot write" <<< "$err"
    err=$(cd "$repo" && SENTINEL_EVENTS_FILE="$T/ro2/nowhere/events.jsonl" scripts/dev/sentinel-event.sh note info "lost" 2>&1 >/dev/null); rc=$?
    expect "main checkout exits non-zero" [ "$rc" -ne 0 ]
    expect "main checkout is not a fallback" [ ! -e "$repo/.sentinel-events.jsonl" ]
fi

# 10: range-diff stop still exits 3 with outcome land_review and no merge.
setup_case range-diff
perl -pi -e 's/^3$/three/' "$repo/file.txt"
git -C "$repo" commit -q -am "main moves line 3"
main_before=$(git -C "$repo" rev-parse main)
land; rc=$?
expect "exit 3 (got $rc): $(tail -3 "$T/land.out")" [ "$rc" -eq 3 ]
load dirs
rec="${dirs[0]:-/missing}/landing.json"
expect "outcome land_review" [ "$(jget "$rec" 'v["outcome"]')" = land_review ]
new_tip=$(jget "$rec" 'v["rebased_tip"]')
expect "rebased tip SHA recorded" [ "${#new_tip}" -eq 40 ]
expect "writer branch stayed at old tip" [ "$(git -C "$wt" rev-parse HEAD)" = "$(jget "$rec" 'v["branch_tip_before"]')" ]
expect "main unchanged" [ "$(git -C "$repo" rev-parse main)" = "$main_before" ]
expect "land_review event" [ "$(events_with '"kind":"land_review"')" -eq 1 ]
expect "no land_failed event" [ "$(events_with '"kind":"land_failed"')" -eq 0 ]
expect "review prompt pins main" grep -q -- "--yes=$main_before" "$T/land.out"
land --yes; rc=$?
expect "bare --yes refused" [ "$rc" -eq 2 ]
land "--yes=$main_before"; rc=$?
expect "matching main approval lands ($rc): $(tail -5 "$T/land.out")" [ "$rc" -eq 0 ]

setup_case range-diff-stale-approval
perl -pi -e 's/^3$/three/' "$repo/file.txt"
git -C "$repo" commit -q -am 'main moves before review'
reviewed_main=$(git -C "$repo" rev-parse main)
land; rc=$?
expect "range-diff review required" [ "$rc" -eq 3 ]
echo later > "$repo/later.txt"
git -C "$repo" add later.txt
git -C "$repo" commit -q -m 'main moves after review'
current_main=$(git -C "$repo" rev-parse main)
land "--yes=$reviewed_main"; rc=$?
expect "stale main approval refused" [ "$rc" -eq 2 ]
expect "stale approval explains both SHAs" grep -q "$reviewed_main.*$current_main" "$T/land.out"
expect "stale approval did not merge" [ "$(git -C "$repo" rev-parse main)" = "$current_main" ]

# A clean rebase merges the detached tested tip and removes the original branch.
setup_case rebase-success
old_tip=$(git -C "$wt" rev-parse HEAD)
echo main-only > "$repo/main-only.txt"
git -C "$repo" add main-only.txt
git -C "$repo" commit -q -m 'main moves before successful rebase'
land; rc=$?
expect "rebased land succeeds ($rc): $(tail -5 "$T/land.out")" [ "$rc" -eq 0 ]
load dirs
rec="${dirs[0]:-/missing}/landing.json"
expect "rebased tip differs from old tip" [ "$(jget "$rec" 'v["rebased_tip"]')" != "$old_tip" ]
expect "merge contains tested tip" [ "$(git -C "$repo" rev-parse HEAD^2)" = "$(jget "$rec" 'v["rebased_tip"]')" ]
expect "rebased writer worktree removed" [ ! -e "$wt" ]
expect "rebased branch removed" [ -z "$(git -C "$repo" branch --list feat/x)" ]
expect "rebased clean checkout removed" [ ! -e "$land_root" ]

# 8: every journal line written above parses with SentinelBar's EventParser rules and has an id.
case_name=journal-format
SENTINEL_EVENTS_FILE="$scratch/success/home/events.jsonl" "$SRC/scripts/dev/sentinel-event.sh" note info $'tab\there "q" \\ back\x01ctl\nline' > /dev/null 2>&1 ||
    fail "control-character event not written"
python3 - "$scratch" <<'PY' || fail "journal lines do not parse"
import glob, json, math, sys
files = glob.glob(sys.argv[1] + '/*/home/events.jsonl')
count = 0
for path in files:
    for line in open(path):
        obj = json.loads(line, parse_constant=lambda name: (_ for _ in ()).throw(ValueError(name)))
        at = obj['at']
        assert isinstance(at, (int, float)) and not isinstance(at, bool), line
        assert isinstance(at, int) or math.isfinite(at), line
        assert isinstance(obj['kind'], str) and isinstance(obj['text'], str), line
        assert obj['tone'] in ('info', 'ok', 'warn', 'bad'), line
        if obj['text'] != 'no id':  # the id-less fixture line is kept as given
            assert isinstance(obj.get('id'), str) and obj['id'], line
        count += 1
assert count >= 8, 'only %d journal lines found' % count
PY

if [[ $failures -eq 0 ]]; then
    echo "test_agent_worktree: all cases passed"
    [[ -z "${SHOW_EXAMPLE:-}" ]] || cat "$example_record"
    exit 0
fi
echo "test_agent_worktree: $failures check(s) failed" >&2
exit 1
