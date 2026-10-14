#!/usr/bin/env bash
# Tests for agent-worktree.sh land evidence and sentinel-event.sh.
#   bash scripts/dev/test_agent_worktree.sh [source-root]
# Every case runs in a throwaway git repo under a temp dir with stub build-queue,
# chart-health, cmake and ctest; HOME, CODEX_HOME, the worktree root, the evidence
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

setup_case() {  # $1 case name; builds $T, $repo and a worktree for branch feat/x with one commit
    case_name=$1
    T="$scratch/$1"
    mkdir -p "$T/bin" "$T/home" "$T/codex" "$T/evidence" "$T/trees-mount"
    export HOME="$T/home" CODEX_HOME="$T/codex" SENTINEL_LAND_EVIDENCE_DIR="$T/evidence"
    export SENTINEL_WORKTREE_ROOT="$T/trees-mount/trees" SENTINEL_EVENTS_FILE="$T/home/events.jsonl"
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
    printf '#!/bin/sh\nexit 0\n' > "$repo/scripts/dev/chart-health.sh"
    chmod +x "$repo/scripts/dev/"*.sh
    seq 1 10 > "$repo/file.txt"
    git -C "$repo" add -A && git -C "$repo" commit -q -m base
    (cd "$repo" && scripts/dev/agent-worktree.sh create feat/x > /dev/null 2>&1) || fail "create failed"
    wt="$SENTINEL_WORKTREE_ROOT/feat-x"
    perl -pi -e 's/^5$/five/' "$wt/file.txt"
    git -C "$wt" commit -q -am "feature"
}

land() { (cd "$repo" && scripts/dev/agent-worktree.sh land feat/x "$@") > "$T/land.out" 2>&1; }

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
expect "trailers" [ "$(jget "$rec" 'v["trailers"]')" = '["Slice: S1", "Risk: medium"]' ]
expect "ctest summary" [ "$(jget "$rec" 'v["ctest_summary"]')" = "100% tests passed, 0 tests failed out of 2" ]
expect "build log kept" [ -s "${dirs[0]:-/missing}/build.log" ]
expect "ctest log kept" grep -q "tests passed" "${dirs[0]:-/missing}/ctest.log"
expect "logs listed" [ "$(jget "$rec" 'v["logs"]')" = '["build.log", "ctest.log"]' ]
expect "gate_reached remove" [ "$(jget "$rec" 'v["gate_reached"]')" = remove ]
expect "gates all ok" [ "$(jget "$rec" 'sorted({g["result"] for g in v["gates"]})')" = '["ok"]' ]
expect "worktree removed" [ ! -e "$wt" ]
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
echo stray > "$wt/stray-untracked.txt"
land; rc=$?
expect "exit 4 (got $rc)" [ "$rc" -eq 4 ]
load dirs
rec="${dirs[0]:-/missing}/landing.json"
expect "outcome landed_evidence_incomplete" [ "$(jget "$rec" 'v["outcome"]')" = landed_evidence_incomplete ]
expect "gate_reached remove" [ "$(jget "$rec" 'v["gate_reached"]')" = remove ]
expect "exactly one merge on main" [ "$(git -C "$repo" rev-list --merges --count main)" -eq 1 ]
expect "worktree kept" [ -d "$wt" ]
expect "no land_failed event" [ "$(events_with '"kind":"land_failed"')" -eq 0 ]

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
expect "rebased tip recorded" [ "$(jget "$rec" 'v["rebased_tip"]')" = "$(git -C "$wt" rev-parse HEAD)" ]
expect "main unchanged" [ "$(git -C "$repo" rev-parse main)" = "$main_before" ]
expect "land_review event" [ "$(events_with '"kind":"land_review"')" -eq 1 ]
expect "no land_failed event" [ "$(events_with '"kind":"land_failed"')" -eq 0 ]

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
