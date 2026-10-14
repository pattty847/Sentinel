#!/usr/bin/env bash
# Isolated Sentinel/private fixtures; never writes to the real _agent symlink.
set -euo pipefail
SRC=$(cd "${1:-$(dirname "$0")/../..}" && pwd)
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
export GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_NOSYSTEM=1
export GIT_AUTHOR_NAME=t GIT_AUTHOR_EMAIL=t@example.invalid
export GIT_COMMITTER_NAME=t GIT_COMMITTER_EMAIL=t@example.invalid
failures=0
cases=0

setup_case() {
    local name
    case_name=$1
    repo="$scratch/$case_name/sentinel"
    private="$scratch/$case_name/private"
    mkdir -p "$repo/docs" "$repo/libs" "$private/_agent" "$private/archive/agent"
    git init -q -b main "$repo"
    git init -q -b main "$private"
    printf 'status\n' > "$repo/docs/STATUS.md"
    printf 'todo\n' > "$repo/docs/TODO.md"
    printf 'void KnownSymbol() {}\nTEST(KnownSuite, KnownCase) {}\nvoid Overlapping() {}\nvoid KnownClass::method() {}\n' > "$repo/libs/known.cpp"
    for name in INVARIANTS FAILURE_MODES DECISIONS REPO_MAP; do
        printf '# %s\n' "$name" > "$private/_agent/$name.md"
    done
    ln -s "$private/_agent" "$repo/_agent"
    git -C "$private" add -A
    git -C "$private" commit -qm baseline
    git -C "$repo" add -A
    git -C "$repo" commit -qm baseline
    git -C "$repo" switch -qc probe
}

pad() { python3 - "$1" "$2" <<'PY'
from pathlib import Path
import sys
Path(sys.argv[1]).write_bytes(b"#" * (int(sys.argv[2]) - 1) + b"\n")
PY
}

entry() { python3 - "$1" "$2" "$3" <<'PY'
from pathlib import Path
import sys
prefix = sys.argv[3]
with Path(sys.argv[1]).open("a") as out:
    out.write(prefix + "x" * (int(sys.argv[2]) - len(prefix)) + "\n")
PY
}

baseline_docs() {
    git -C "$repo" commit -qam 'fixture baseline'
    git -C "$repo" branch -f main HEAD
}
baseline_memory() { git -C "$private" commit -qam 'fixture baseline'; }

run_expect() {
    local expected=$1 pattern=$2 status=0
    shift 2
    (cd "$repo" && "$BASH" "$SRC/scripts/dev/doc-health.sh" "$@") > "$scratch/output" 2>&1 || status=$?
    cases=$((cases + 1))
    if [[ "$status" -ne "$expected" ]] || ! grep -q "$pattern" "$scratch/output"; then
        echo "FAIL [$case_name]: expected exit $expected and '$pattern', got $status" >&2
        cat "$scratch/output" >&2
        failures=$((failures + 1))
    fi
}

# Sentinel document caps reject growth, allow shrinkage, and apply without a baseline.
for name_cap in 'docs/STATUS.md:8192' 'docs/TODO.md:12288'; do
    name=${name_cap%:*}
    cap=${name_cap##*:}
    setup_case "grow-${name##*/}"
    pad "$repo/$name" "$((cap + 1))"
    baseline_docs
    pad "$repo/$name" "$((cap + 2))"
    run_expect 1 "FAIL size $name"

    setup_case "shrink-${name##*/}"
    pad "$repo/$name" "$((cap + 2))"
    baseline_docs
    pad "$repo/$name" "$((cap + 1))"
    run_expect 0 'over cap, ratchet holds'

    setup_case "missing-baseline-${name##*/}"
    rm "$repo/$name"
    baseline_docs
    pad "$repo/$name" "$((cap + 1))"
    run_expect 1 "FAIL size $name"
done

# Memory caps are absolute, including committed growth. Only the explicit
# pending-prune names bypass the size failure; their entry checks still apply.
for name_cap in 'FAILURE_MODES.md:25600' 'INVARIANTS.md:25600' \
                'DECISIONS.md:25600' 'REPO_MAP.md:5120'; do
    name=${name_cap%:*}
    cap=${name_cap##*:}
    expected=1
    pattern="FAIL size _agent/$name"
    case "$name" in
        INVARIANTS.md|DECISIONS.md) expected=0; pattern='over cap (pending prune)' ;;
    esac
    setup_case "absolute-$name"
    pad "$private/_agent/$name" "$((cap + 2))"
    baseline_memory
    run_expect "$expected" "$pattern"
    pad "$private/_agent/$name" "$((cap + 1))"
    run_expect "$expected" "$pattern"
    pad "$private/_agent/$name" "$((cap + 3))"
    baseline_memory
    run_expect "$expected" "$pattern"

    setup_case "missing-baseline-$name"
    rm "$private/_agent/$name"
    baseline_memory
    pad "$private/_agent/$name" "$((cap + 1))"
    run_expect "$expected" "$pattern"
done

setup_case committed-growth
pad "$repo/docs/TODO.md" 12289
git -C "$repo" commit -qam 'feature growth'
run_expect 1 'FAIL size docs/TODO.md'

# main advances independently; the baseline is the merge base, not main's tip.
setup_case merge-base
pad "$repo/docs/TODO.md" 12290
git -C "$repo" switch -q main
git -C "$repo" commit -qam 'main grows independently'
git -C "$repo" switch -q probe
pad "$repo/docs/TODO.md" 12289
run_expect 1 'FAIL size docs/TODO.md'

for prefix in '- FM-001 | ' '- INV-001 | ' '- 2026-10-14 | ' '- AREA: libs/foo.cpp | '; do
    setup_case "new-entry-$cases"
    entry "$private/_agent/FAILURE_MODES.md" 351 "$prefix"
    run_expect 1 '351 > 350 characters'
done

setup_case entry-boundary
entry "$private/_agent/FAILURE_MODES.md" 350 '- FM-001 | '
run_expect 0 'doc-health: PASS'

setup_case unchanged-long
entry "$private/_agent/FAILURE_MODES.md" 600 '- FM-001 | '
baseline_memory
printf '\n' >> "$private/_agent/FAILURE_MODES.md"
run_expect 0 'doc-health: PASS'

setup_case changed-long
entry "$private/_agent/FAILURE_MODES.md" 600 '- FM-001 | '
baseline_memory
printf '# FAILURE_MODES\n' > "$private/_agent/FAILURE_MODES.md"
entry "$private/_agent/FAILURE_MODES.md" 351 '- FM-001 | '
run_expect 1 '351 > 350 characters'

setup_case wrapped-entry
entry "$private/_agent/FAILURE_MODES.md" 200 '- FM-001 | '
entry "$private/_agent/FAILURE_MODES.md" 151 '  '
run_expect 1 '351 > 350 characters'

setup_case wrapped-unchanged
entry "$private/_agent/FAILURE_MODES.md" 200 '- FM-001 | '
entry "$private/_agent/FAILURE_MODES.md" 151 '  '
baseline_memory
run_expect 0 'doc-health: PASS'
printf 'extra continuation\n' >> "$private/_agent/FAILURE_MODES.md"
run_expect 1 'characters'

setup_case wrapped-boundary
entry "$private/_agent/FAILURE_MODES.md" 200 '- FM-001 | '
entry "$private/_agent/FAILURE_MODES.md" 150 '  '
run_expect 0 'doc-health: PASS'

setup_case blank-ends-entry
entry "$private/_agent/FAILURE_MODES.md" 200 '- FM-001 | '
printf '\n' >> "$private/_agent/FAILURE_MODES.md"
entry "$private/_agent/FAILURE_MODES.md" 151 'not a continuation '
run_expect 0 'doc-health: PASS'

setup_case pending-prune-entry-check
pad "$private/_agent/INVARIANTS.md" 25601
baseline_memory
entry "$private/_agent/INVARIANTS.md" 351 '- INV-001 | '
run_expect 1 '351 > 350 characters'

for kind in FM INV; do
    setup_case "duplicate-$kind"
    printf -- '- %s-001 | first\n- %s-001 | second\n' "$kind" "$kind" >> "$private/_agent/FAILURE_MODES.md"
    run_expect 1 "duplicate _agent/FAILURE_MODES.md: $kind-001"
    setup_case "archive-$kind"
    printf -- '- %s-001 | active\n' "$kind" >> "$private/_agent/FAILURE_MODES.md"
    printf -- '- %s-001 | old | Archived 2026-10-14: fixture\n' "$kind" > "$private/archive/agent/FAILURE_MODES.md"
    run_expect 1 "$kind-001 also appears in archive"

    setup_case "colon-duplicate-$kind"
    printf -- '- %s-001: first\n- %s-001 | second\n' "$kind" "$kind" >> "$private/_agent/FAILURE_MODES.md"
    run_expect 1 "duplicate _agent/FAILURE_MODES.md: $kind-001"
    setup_case "colon-archive-$kind"
    printf -- '- %s-001: active\n' "$kind" >> "$private/_agent/FAILURE_MODES.md"
    printf -- '- %s-001: archived\n' "$kind" > "$private/archive/agent/FAILURE_MODES.md"
    run_expect 1 "$kind-001 also appears in archive"
done

setup_case id-boundary
printf -- '- FM-001: real\n- FM-001-suffix | not the same id\n- FM-001_extra | not the same id\n' >> "$private/_agent/FAILURE_MODES.md"
run_expect 0 'doc-health: PASS'

setup_case archive-reference
printf -- '- FM-001 | active\n' >> "$private/_agent/FAILURE_MODES.md"
printf -- '- FM-002 | refers to FM-001\n' > "$private/archive/agent/FAILURE_MODES.md"
run_expect 0 'doc-health: PASS'

setup_case non-ascii
printf '\303\251\n' >> "$private/_agent/DECISIONS.md"
baseline_memory
run_expect 1 'ASCII _agent/DECISIONS.md'

setup_case missing-memory
rm "$repo/_agent"
run_expect 0 '_agent missing; memory checks skipped'

setup_case dead-pointer
printf -- '- FM-001 | See `libs/missing.cpp:123` and MissingSuite.MissingCase\n' >> "$private/_agent/FAILURE_MODES.md"
run_expect 0 'warning: dead pointers: 1 (up to 20): .*FM-001 MissingSuite.MissingCase'
run_expect 0 'doc-health: PASS' --report

setup_case live-pointers
printf -- '- FM-001 | See libs/known.cpp:123\n- FM-002 | See `KnownSymbol`\n- FM-003 | See `TEST(KnownSuite, KnownCase)`\n- FM-004 | See KnownSuite.KnownCase\n- FM-005 | See `Overlap`\n- FM-006 | See `Overlapping`\n- FM-007 | See `lapping`\n' >> "$private/_agent/FAILURE_MODES.md"
run_expect 0 'warning: dead pointers: 0'

setup_case qualified-method
printf -- '- FM-001 | See KnownClass::method\n' >> "$private/_agent/FAILURE_MODES.md"
run_expect 0 'resolved pointers: 1'

setup_case function-parens
printf -- '- FM-001 | See KnownSymbol()\n' >> "$private/_agent/FAILURE_MODES.md"
run_expect 0 'resolved pointers: 1'

setup_case dotted-config
printf -- '- FM-001 | chart.timeframes_ms and `chart.timeframes_ms`\n' >> "$private/_agent/FAILURE_MODES.md"
run_expect 0 'no pointer: 1'
run_expect 0 'warning: dead pointers: 0'

setup_case uppercase-gtest
printf -- '- FM-001 | MissingSuite.MissingCase\n' >> "$private/_agent/FAILURE_MODES.md"
run_expect 0 'dead pointers: 1 (up to 20): .*FM-001 MissingSuite.MissingCase'

setup_case wrapped-pointer
printf -- '- FM-001 | no inline pointer\n  See KnownClass::method\n' >> "$private/_agent/FAILURE_MODES.md"
run_expect 0 'resolved pointers: 1'

setup_case no-pointer
printf -- '- FM-001 | prose only\n' >> "$private/_agent/FAILURE_MODES.md"
run_expect 0 'no pointer: 1'
if grep -q 'FM-001' "$scratch/output"; then
    echo "FAIL [$case_name]: no-pointer entry id leaked into warnings" >&2
    failures=$((failures + 1))
fi

setup_case warning-limit
for number in $(seq 1 21); do
    printf -- '- FM-%03d | See `libs/dead-%03d.cpp`\n' "$number" "$number" >> "$private/_agent/FAILURE_MODES.md"
done
run_expect 0 'warning: dead pointers: 21 (up to 20): .*FM-020 libs/dead-020.cpp'
if grep -q 'FM-021' "$scratch/output"; then
    echo "FAIL [$case_name]: printed more than 20 pointer ids" >&2
    failures=$((failures + 1))
fi

setup_case report-breaches
pad "$repo/docs/STATUS.md" 8193
entry "$private/_agent/FAILURE_MODES.md" 351 '- FM-001 | '
run_expect 0 'doc-health: FAIL (2)' --report

setup_case report-error
rm "$repo/_agent"
ln -s "$scratch/nonexistent" "$repo/_agent"
run_expect 2 '_agent symlink is broken'
run_expect 0 '_agent symlink is broken' --report

if [[ "$failures" -ne 0 ]]; then
    echo "test_doc_health: FAIL ($failures/$cases checks)" >&2
    exit 1
fi
echo "test_doc_health: all $cases checks passed"
