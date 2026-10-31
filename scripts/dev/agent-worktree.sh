#!/usr/bin/env bash
# agent-worktree.sh — isolated Sentinel worktrees with a ready build, for agents and humans.
#
#   scripts/dev/agent-worktree.sh create <branch> [base-ref]   # default base: main
#   scripts/dev/agent-worktree.sh remove <branch> [--force]
#   scripts/dev/agent-worktree.sh verify-gitdir <branch>
#   scripts/dev/agent-worktree.sh land <branch> [--yes] [-m <message>] [--trailer 'Key: value']...
#   scripts/dev/agent-worktree.sh list
#
# land = the merge queue, one branch at a time: rebase the branch onto the
# current main (rerere on), build and run ctest in its worktree, stop for review
# if the rebase changed any commit (git range-diff), refuse if main moved, then
# merge --no-ff into main and remove the worktree. The rebased tip contains main,
# so the tested tree is exactly what lands. Run it from the main checkout.
# --trailer (repeatable) appends git trailers to the merge commit for the workflow
# audit (AGENTS.md section 10); read them with
# git log --first-parent main --format='%h %s%n%(trailers:only,unfold)'.
# Each land attempt keeps its evidence (landing.json, build.log, ctest.log) in
# its own directory under SENTINEL_LAND_EVIDENCE_DIR (default
# ~/Library/Logs/Sentinel/land), and moves the worktree's sandbox events
# (.sentinel-events.jsonl) into the primary journal before removing it. Exit
# codes: 0 landed, 3 stopped for range-diff review, 4 merged but evidence
# incomplete (worktree kept; finish with `remove <branch>`), other = not merged.
#
# Worktrees live on the external drive when it is mounted
# (SENTINEL_WORKTREE_ROOT, default /Volumes/T7/sentinel-worktrees), otherwise
# under <repo>/.claude/worktrees. External worktrees are locked so a
# `git worktree prune` while the drive is unplugged cannot drop them.
# Non-interactive shells lack VCPKG_ROOT; this script supplies it.
set -euo pipefail

# Discover from the script's checkout without loading agent-writable Git config.
SCRIPT_REPO=$(cd "$(dirname "$0")/../.." && pwd -P)
COMMON_DIR=$(python3 -I - "$SCRIPT_REPO" <<'PYROOT'
import pathlib, sys
root = pathlib.Path(sys.argv[1])
p = root / '.git'
if p.is_dir():
    print(p.resolve())
else:
    line = p.read_text().strip()
    if not line.startswith('gitdir: '):
        sys.exit('error: invalid checkout .git file')
    gd = (root / line[8:]).resolve()
    if gd.parent.name != 'worktrees':
        sys.exit('error: expected a standard linked worktree gitdir')
    print(gd.parent.parent)
PYROOT
)
REPO=$(dirname "$COMMON_DIR")
EXTERNAL_ROOT=${SENTINEL_WORKTREE_ROOT:-/Volumes/T7/sentinel-worktrees}
export VCPKG_ROOT=${VCPKG_ROOT:-$HOME/vcpkg}
NINJA=$(command -v ninja || echo /opt/homebrew/bin/ninja)

worktree_root() {
    local mount
    mount=$(dirname "$EXTERNAL_ROOT")
    if [[ -d "$mount" && -w "$mount" ]]; then
        mkdir -p "$EXTERNAL_ROOT"
        echo "$EXTERNAL_ROOT"
    else
        echo "warning: $mount not mounted; using $REPO/.claude/worktrees" >&2
        echo "$REPO/.claude/worktrees"
    fi
}

dir_for() {
    local root
    root=$(worktree_root)
    echo "$root/${1//\//-}"
}

path_of_branch() {
    git -C "$REPO" worktree list --porcelain |
        awk -v b="refs/heads/$1" '/^worktree /{p=substr($0,10)} $0=="branch " b {print p}'
}

# This checker uses only filesystem reads. Keep it self-contained: codex-exec
# snapshots this function before launch so an agent cannot replace its exit check.
verify_gitdir() {
    python3 -I - "$COMMON_DIR" "$@" <<'PYVERIFY'
import os, pathlib, stat, sys

def fail(reason):
    sys.exit('verify-gitdir: ' + reason)

def regular(p):
    try:
        mode = p.lstat().st_mode
    except OSError as e:
        fail(str(e))
    if not stat.S_ISREG(mode):
        fail(str(p) + ': expected regular file (no symlinks)')
    return p.read_text().strip()

def real(p):
    return p.resolve(strict=True)

try:
    common = real(pathlib.Path(sys.argv[1]))
    selector = sys.argv[2]
    expected = real(pathlib.Path(sys.argv[3])) if len(sys.argv) > 3 else None
    if selector == '--path':
        if expected is None:
            fail('--path requires a worktree')
        pointer = regular(expected / '.git')
        if not pointer.startswith('gitdir: '):
            fail('invalid .git pointer: ' + str(expected / '.git'))
        gd = real(expected / pointer[8:])
        if gd.parent != common / 'worktrees':
            fail('worktree gitdir is outside ' + str(common / 'worktrees'))
    else:
        matches = []
        for candidate in (common / 'worktrees').iterdir():
            if candidate.is_symlink():
                fail('symlinked gitdir: ' + str(candidate))
            head = candidate / 'HEAD'
            if head.exists() and regular(head) == 'ref: refs/heads/' + selector:
                matches.append(candidate)
        if len(matches) != 1:
            fail('expected one worktree for branch ' + selector)
        gd = matches[0]
    allowed_files = {'HEAD', 'ORIG_HEAD', 'index', 'commondir', 'gitdir', 'locked',
                     'COMMIT_EDITMSG', 'MERGE_RR', 'AUTO_MERGE'}
    for p in gd.iterdir():
        if p.name in {'logs', 'refs'}:
            if p.is_symlink() or not p.is_dir():
                fail(str(p) + ': expected directory (no symlinks)')
            for base, dirs, files in os.walk(p, followlinks=False):
                for name in dirs + files:
                    child = pathlib.Path(base) / name
                    mode = child.lstat().st_mode
                    if not (stat.S_ISDIR(mode) or stat.S_ISREG(mode)):
                        fail(str(child) + ': unexpected file type')
        elif p.name in allowed_files:
            if not stat.S_ISREG(p.lstat().st_mode):
                fail(str(p) + ': expected regular file (no symlinks)')
        else:
            fail('unexpected gitdir entry: ' + str(p))
    if real(gd / regular(gd / 'commondir')) != common:
        fail('commondir does not resolve to ' + str(common))
    backlink = pathlib.Path(regular(gd / 'gitdir'))
    if not backlink.is_absolute():
        backlink = gd / backlink
    if backlink.name != '.git':
        fail('gitdir backlink does not name <worktree>/.git')
    wt = real(backlink.parent)
    if expected and wt != expected:
        fail('gitdir backlink does not point to ' + str(expected / '.git'))
    pointer = regular(wt / '.git')
    if not pointer.startswith('gitdir: ') or real(wt / pointer[8:]) != gd:
        fail('worktree .git does not point back to ' + str(gd))
    # No alternates are provisioned. Existence (even an empty file or dangling
    # symlink) is a change; never adopt an agent-supplied baseline.
    for name in ('alternates', 'http-alternates'):
        p = common / 'objects' / 'info' / name
        if os.path.lexists(p):
            fail('unexpected object alternates: ' + str(p))
    head = regular(gd / 'HEAD')
    if not head.startswith('ref: refs/heads/'):
        fail('detached HEAD in ' + str(gd))
    branch = head[len('ref: refs/heads/'):]
    if selector != '--path' and branch != selector:
        fail('HEAD branch changed')
    print(wt)
except (OSError, ValueError, UnicodeError) as e:
    fail(str(e))
PYVERIFY
}

# The wrapper sources only the checker definition, not the command dispatcher.
if [[ "${1:-}" == --export-gitdir-check ]]; then
    declare -f verify_gitdir
    exit 0
fi

# Land evidence. JSON work (landing.json, event transfer, Codex session scan,
# retention) is in this one Python block; the land flow below stays in bash.
LAND_PY=$(cat <<'PY'
import fcntl, glob, hashlib, json, math, os, re, shutil, sys, tempfile, time
from datetime import datetime, timedelta, timezone

def iso(epoch):
    return datetime.fromtimestamp(int(epoch), timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')

def env_lines(name):
    return [line for line in os.environ.get(name, '').split('\n') if line]

def env_json(name, default):
    value = os.environ.get(name, '')
    return json.loads(value) if value else default

def record(path):
    e = lambda name: os.environ.get(name) or None
    gates = [dict(zip(('name', 'started_at', 'ended_at', 'result'), g.split('\t'))) for g in env_lines('LAND_GATES')]
    for g in gates:
        g['started_at'], g['ended_at'] = iso(g['started_at']), iso(g['ended_at'])
    started, ended = int(e('LAND_STARTED')), int(e('LAND_ENDED'))
    data = {
        'schema': 1, 'attempt_id': e('LAND_ATTEMPT_ID'), 'branch': e('LAND_BRANCH'),
        'outcome': e('LAND_OUTCOME'),
        'exit_code': int(e('LAND_EXIT_CODE')) if e('LAND_EXIT_CODE') else None,
        'gate_reached': e('LAND_GATE'), 'gates': gates,
        'started_at': iso(started), 'ended_at': iso(ended), 'duration_s': ended - started,
        'main_tip_before': e('LAND_MAIN_TIP'), 'branch_tip_before': e('LAND_BRANCH_TIP'),
        'merge_base_before': e('LAND_MERGE_BASE'), 'rebased_tip': e('LAND_REBASED_TIP'),
        'merge_sha': e('LAND_MERGE_SHA'), 'trailers': env_lines('LAND_TRAILERS'),
        'ctest_summary': e('LAND_CTEST_SUMMARY'), 'logs': env_lines('LAND_LOGS'),
        'events_transfer': env_json('LAND_TRANSFER', {'status': 'none'}),
        'codex_session_candidates': env_json('LAND_CODEX', []),
        'evidence_problems': env_lines('LAND_PROBLEMS'),
    }
    fd, tmp = tempfile.mkstemp(dir=os.path.dirname(path), prefix='.landing.', suffix='.tmp')
    with os.fdopen(fd, 'w') as f:
        json.dump(data, f, indent=2)
        f.write('\n')
    os.replace(tmp, path)

def valid_event(obj):  # SentinelBar EventParser rules; `at` a finite number, never a bool
    at = obj.get('at') if isinstance(obj, dict) else None
    return (isinstance(at, (int, float)) and not isinstance(at, bool) and (isinstance(at, int) or math.isfinite(at))
            and isinstance(obj.get('kind'), str) and isinstance(obj.get('text'), str)
            and obj.get('tone') in ('info', 'ok', 'warn', 'bad'))

def reject_constant(name):  # NaN / Infinity are not JSON; SentinelBar rejects them
    raise ValueError(name)

def parse(line):
    try:
        return json.loads(line, parse_constant=reject_constant)
    except ValueError:
        return None

def event_key(line, obj):
    event_id = obj.get('id') if isinstance(obj, dict) else None
    if isinstance(event_id, str) and event_id:
        return 'id:' + event_id
    return 'sha256:' + hashlib.sha256(line.encode()).hexdigest()

def transfer(src, dst, previous=''):
    # All or nothing: every source line must be a valid event ending in a newline, else nothing is
    # appended. The destination is locked (flock) across read, dedup and the single append.
    res = {'source': src, 'destination': dst, 'lines_read': 0, 'appended': 0,
           'duplicates_skipped': 0, 'status': 'none'}
    earlier = json.loads(previous).get('appended', 0) if previous else 0
    if os.path.exists(src):
        try:
            with open(src, encoding='utf-8', errors='replace') as f:
                raw = f.read()
            rows, bad = [], []
            for number, line in enumerate(raw.split('\n'), 1):
                if line.strip():
                    obj = parse(line)
                    ok = valid_event(obj) and (raw.endswith('\n') or number < raw.count('\n') + 1)
                    (rows.append((line, obj)) if ok else bad.append(number))
            res['lines_read'] = len(rows) + len(bad)
            if bad:
                res['status'], res['error'] = 'failed', 'invalid event lines %s; nothing transferred' % bad
            else:
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                fd = os.open(dst, os.O_RDWR | os.O_CREAT | os.O_APPEND, 0o644)
                try:
                    fcntl.flock(fd, fcntl.LOCK_EX)
                    with open(dst, encoding='utf-8', errors='replace') as f:
                        existing = f.read()
                    seen = {event_key(l, parse(l)) for l in existing.split('\n') if l.strip()}
                    new = []
                    for line, obj in rows:
                        key = event_key(line, obj)
                        if key not in seen:
                            seen.add(key)
                            new.append(line)
                    if new:
                        data = ('' if not existing or existing.endswith('\n') else '\n') + ''.join(l + '\n' for l in new)
                        data = data.encode()
                        while data:
                            data = data[os.write(fd, data):]
                finally:
                    os.close(fd)
                res['appended'] = earlier + len(new)
                res['duplicates_skipped'] = max(0, len(rows) - len(new) - earlier)
                res['status'] = 'ok'
        except OSError as exc:
            res['status'], res['error'] = 'failed', str(exc)
    print(json.dumps(res))
    return 1 if res['status'] == 'failed' else 0

def codex(branch, home):
    found, cutoff = [], time.time() - 30 * 86400
    for path in sorted(glob.glob(os.path.join(home, 'sessions', '**', 'rollout-*.jsonl'), recursive=True)):
        try:
            if os.path.getmtime(path) < cutoff:
                continue
            with open(path) as f:
                obj = json.loads(f.readline())
        except (OSError, ValueError):
            continue
        payload = obj.get('payload') if isinstance(obj, dict) and obj.get('type') == 'session_meta' else None
        if isinstance(payload, dict) and (payload.get('git') or {}).get('branch') == branch:
            found.append({'session_id': payload.get('id'), 'path': path,
                          'started_at': payload.get('timestamp') or obj.get('timestamp'),
                          'cwd': payload.get('cwd'), 'match': 'branch'})
    print(json.dumps(found))
    return 0

def verify(d):
    try:
        with open(os.path.join(d, 'landing.json')) as f:
            data = json.load(f)
    except (OSError, ValueError) as exc:
        print('landing.json unreadable: %s' % exc)
        return 1
    problems = ['log missing: ' + n for n in data.get('logs', []) if not os.path.isfile(os.path.join(d, n))]
    status = (data.get('events_transfer') or {}).get('status')
    if status not in ('ok', 'none'):
        problems.append('event transfer status: %s' % status)
    for p in problems:
        print(p)
    return 1 if problems else 0

def retain(root, current):
    # Only attempt dirs of this scheme (id pattern + landing.json); newest 300 kept, none older than 180 days.
    pattern, now, attempts = re.compile(r'^(\d{8}T\d{6}Z)-.+-\d+$'), datetime.now(timezone.utc), []
    for name in os.listdir(root):
        path, m = os.path.join(root, name), pattern.match(name)
        if name != current and m and not os.path.islink(path) and os.path.isfile(os.path.join(path, 'landing.json')):
            attempts.append((m.group(1), name))
    for index, (stamp, name) in enumerate(sorted(attempts, reverse=True)):
        age = now - datetime.strptime(stamp, '%Y%m%dT%H%M%SZ').replace(tzinfo=timezone.utc)
        if index >= 299 or age > timedelta(days=180):
            shutil.rmtree(os.path.join(root, name))
    return 0

sys.exit(globals()[sys.argv[1]](*sys.argv[2:]) or 0)
PY
)
# -I: no cwd, PYTHONPATH or user site on sys.path; cd /: the helper takes absolute paths only.
land_py() { (cd / && python3 -I -c "$LAND_PY" "$@"); }

add_problem() { echo "evidence: $1" >&2; problems+="$1"$'\n'; }

gate() {  # enter a land stage; the open one ended ok
    local now
    now=$(date +%s)
    [[ -z "$land_gate" ]] || gates+="$land_gate"$'\t'"$gate_started"$'\t'"$now"$'\tok\n'
    land_gate=$1 gate_started=$now
}

new_attempt() {  # atomically create a never-used attempt dir under $1; print its id
    local base id n=1
    base="$(date -u +%Y%m%dT%H%M%SZ)-$(tr -c 'A-Za-z0-9._\n-' '-' <<< "$2")-$$"
    id=$base
    until mkdir "$1/$id" 2>/dev/null; do
        n=$((n + 1))
        [[ $n -le 20 ]] || return 1
        id="$base-$n"
    done
    echo "$id"
}

write_record() {  # $1: result of the stage still open
    [[ -n "$evidence" ]] || return 1
    local open="$land_gate"$'\t'"$gate_started"$'\t'"$(date +%s)"$'\t'"$1"
    LAND_ATTEMPT_ID=$attempt_id LAND_BRANCH=$branch LAND_OUTCOME=$outcome LAND_EXIT_CODE=$final_rc \
        LAND_GATE=$land_gate LAND_GATES="$gates$open" LAND_STARTED=$land_started LAND_ENDED=$(date +%s) \
        LAND_MAIN_TIP=$main_tip LAND_BRANCH_TIP=$old_tip LAND_MERGE_BASE=$old_base LAND_REBASED_TIP=$new_tip \
        LAND_MERGE_SHA=$merge_sha LAND_TRAILERS=$trailers LAND_CTEST_SUMMARY=$ctest_summary LAND_LOGS=$logs \
        LAND_TRANSFER=$transfer LAND_CODEX=$codex LAND_PROBLEMS=$problems \
        land_py record "$evidence/landing.json"
}

transfer_events() {  # sandbox events into the primary journal; idempotent (dedup by id)
    [[ -n "$dir" ]] || return 0
    local out
    if out=$(land_py transfer "$dir/.sentinel-events.jsonl" "$primary_events" "$transfer"); then
        transfer=$out
    else
        transfer=${out:-'{"status": "failed"}'}
        add_problem "event transfer from $dir/.sentinel-events.jsonl failed"
    fi
}

scan_codex() {  # Codex sessions whose rollout names the branch (candidates only)
    [[ -z "$codex" ]] || return 0
    codex=$(land_py codex "$branch" "$codex_home") || { codex='[]'; add_problem "codex session scan failed"; }
}

send_event() {  # $1 kind $2 tone $3 text
    local slice event_args=(--branch "$branch")
    while IFS= read -r slice; do
        if [[ "$slice" == "Slice: "* ]]; then break; fi
    done <<< "$trailers"
    if [[ "$slice" == "Slice: "* ]]; then slice=${slice#Slice: }; else slice=""; fi
    [[ -z "$slice" ]] || event_args+=(--slice "$slice")
    if "$REPO/scripts/dev/sentinel-event.sh" "$1" "$2" "$3" "${event_args[@]}"; then
        events_sent+="$1 "
    else
        add_problem "outcome event $1 not recorded"
    fi
}

verify_record() {  # landing.json parses, its logs exist, the transfer succeeded
    local out p
    out=$(land_py verify "$evidence") ||
        while IFS= read -r p; do add_problem "$p"; done <<< "${out:-landing.json verify failed}"
}

land_finish() {  # EXIT trap of land: outcome event and final landing.json for every attempt
    local rc=$1 result=failed
    set +e
    if [[ $merged -eq 1 && $land_complete -eq 0 ]]; then
        outcome=landed_evidence_incomplete  # merged but removal not finished, whatever stopped it
    elif [[ -z "$outcome" || "$outcome" == running ]]; then
        outcome=land_failed
    fi
    final_rc=$rc
    if [[ $merged -eq 0 ]]; then
        [[ -n "$transfer" ]] || transfer_events
        scan_codex
    fi
    if [[ "$outcome" == landed ]]; then
        # Worktree already removed: the landed event was sent before; only the record remains.
        final_rc=0
        write_record ok || {
            echo "error: could not rewrite $evidence/landing.json after removing the worktree" >&2
            outcome=landed_evidence_incomplete
        }
    fi
    case "$outcome" in
        landed) ;;
        landed_evidence_incomplete)
            final_rc=4
            echo "warning: $branch is merged ($merge_short) but its land evidence is incomplete (see $evidence)." >&2
            echo "Do not land again. Read evidence_problems in landing.json, then finish with: scripts/dev/agent-worktree.sh remove $branch" >&2
            send_event landed_evidence_incomplete warn \
                "land $branch merged as $merge_short but evidence is incomplete; see $evidence"
            [[ -z "$evidence" ]] || write_record failed || echo "error: could not write $evidence/landing.json" >&2 ;;
        land_review)
            final_rc=3 result=stopped
            send_event land_review warn "land $branch changed $changed commit(s); review then rerun with --yes" ;;
        *)
            send_event land_failed bad "land $branch failed at $land_gate (exit $rc)" ;;
    esac
    if [[ "$outcome" == land_review || "$outcome" == land_failed ]] && [[ -n "$evidence" ]]; then
        write_record "$result" || echo "error: could not write $evidence/landing.json" >&2
    fi
    [[ -z "$evidence" ]] || echo "land evidence: $evidence ($outcome)"
    exit "$final_rc"
}

cmd=${1:-}
case "$cmd" in
    verify-gitdir)
        shift
        verify_gitdir "$@"
        ;;
    create)
        branch=${2:?usage: create <branch> [base-ref]}
        base=${3:-main}
        dir=$(dir_for "$branch")
        [[ -e "$dir" ]] && { echo "error: $dir already exists" >&2; exit 1; }
        git -C "$REPO" worktree add -q -b "$branch" "$dir" "$base"
        if [[ "$dir" == "$EXTERNAL_ROOT"/* ]]; then
            git -C "$REPO" worktree lock --reason "external drive worktree" "$dir"
        fi
        # _agent/ (invariants, failure modes, decisions) is gitignored; link the
        # shared copy so agents read current rules and record into one place.
        if [[ -d "$REPO/_agent" && ! -e "$dir/_agent" ]]; then
            ln -s "$REPO/_agent" "$dir/_agent"
        fi
        (cd "$dir" && cmake --preset mac-clang -DCMAKE_MAKE_PROGRAM="$NINJA" > "$dir/.configure.log" 2>&1) || {
            echo "error: configure failed, see $dir/.configure.log" >&2; exit 1; }
        echo "ready: $dir"
        echo "  branch: $branch (from $base)"
        echo "  build:  cmake --build --preset mac-clang"
        echo "  test:   ctest --test-dir build/mac-clang"
        ;;
    remove)
        branch=${2:?usage: remove <branch> [--force]}
        dir=$(path_of_branch "$branch")
        [[ -z "$dir" ]] && { echo "error: no worktree for branch $branch" >&2; exit 1; }
        git -C "$REPO" worktree unlock "$dir" 2>/dev/null || true
        if [[ "${3:-}" == "--force" ]]; then
            git -C "$REPO" worktree remove --force "$dir"
        else
            git -C "$REPO" worktree remove "$dir"
        fi
        if git -C "$REPO" branch -d "$branch" 2>/dev/null; then
            echo "removed $dir and merged branch $branch"
        else
            echo "removed $dir; branch $branch kept (not merged into HEAD)"
        fi
        ;;
    land)
        branch=${2:?usage: land <branch> [--yes] [-m <message>] [--trailer 'Key: value']...}
        shift 2
        land_started=$(date +%s)
        evidence_root=${SENTINEL_LAND_EVIDENCE_DIR:-$HOME/Library/Logs/Sentinel/land}
        primary_events=${SENTINEL_EVENTS_FILE:-"$HOME/Library/Application Support/Sentinel/events.jsonl"}
        codex_home=${CODEX_HOME:-$HOME/.codex}
        [[ "$primary_events" == /* ]] || primary_events="$PWD/$primary_events"
        [[ "$codex_home" == /* ]] || codex_home="$PWD/$codex_home"
        [[ "$evidence_root" == /* ]] || evidence_root="$PWD/$evidence_root"
        attempt_id="" evidence="" land_gate="" gate_started="" gates="" outcome="" final_rc="" problems=""
        merged=0 merge_sha="" merge_short="" main_tip="" old_tip="" old_base="" new_tip="" dir="" changed=0
        trailers="" ctest_summary="" logs="" transfer="" codex="" events_sent="" land_complete=0
        trap 'land_finish $?' EXIT
        gate arguments
        { mkdir -p "$evidence_root" && attempt_id=$(new_attempt "$evidence_root" "$branch"); } ||
            { echo "error: cannot create a land evidence directory under $evidence_root" >&2; exit 1; }
        evidence="$evidence_root/$attempt_id"
        echo "land attempt $attempt_id: evidence in $evidence"
        outcome=running
        write_record running || add_problem "initial landing.json not written"
        land_py retain "$evidence_root" "$attempt_id" || add_problem "retention pass over $evidence_root failed"
        confirmed=0
        message="merge: $branch"
        while [[ $# -gt 0 ]]; do
            case "$1" in
                --yes) confirmed=1; shift ;;
                -m) message=${2:?-m needs a message}; shift 2 ;;
                --trailer)
                    t=${2:?--trailer needs 'Key: value'}
                    [[ "$t" =~ ^[A-Za-z][A-Za-z0-9-]*:\ [^[:cntrl:]]+$ ]] ||
                        { echo "error: bad trailer '$t' (want 'Key: value' on one line)" >&2; exit 2; }
                    trailers+="$t"$'\n'; shift 2 ;;
                *) echo "error: unknown option $1" >&2; exit 2 ;;
            esac
        done
        missing=""
        for key in Writer Reviewer Risk; do
            grep -Eq "^$key: [^[:space:]].*$" <<< "$trailers" || missing+=" $key"
        done
        [[ -z "$missing" ]] || { echo "error: missing required land trailers:$missing" >&2; exit 2; }
        gate preflight
        [[ "$(git -C "$REPO" symbolic-ref --short HEAD)" == "main" ]] ||
            { echo "error: $REPO is not on main" >&2; exit 1; }
        git -C "$REPO" diff --quiet && git -C "$REPO" diff --cached --quiet ||
            { echo "error: main checkout has uncommitted changes to tracked files" >&2; exit 1; }
        dir=$(verify_gitdir "$branch")
        [[ -z "$(git -C "$dir" status --porcelain --untracked-files=no)" ]] ||
            { echo "error: $dir has uncommitted changes" >&2; exit 1; }
        main_tip=$(git -C "$REPO" rev-parse main)
        old_tip=$(git -C "$dir" rev-parse HEAD)
        old_base=$(git -C "$dir" merge-base HEAD main)
        if [[ "$old_base" != "$main_tip" ]]; then
            gate rebase
            echo "rebasing $branch onto main ($main_tip)..."
            if ! git -C "$dir" -c rerere.enabled=true -c rerere.autoUpdate=true rebase main; then
                git -C "$dir" rebase --abort || true
                echo "error: rebase conflicts. Resolve in $dir (git rebase main), commit, then rerun land." >&2
                exit 1
            fi
        fi
        new_tip=$(git -C "$dir" rev-parse HEAD)
        gate chart-health
        health_status=0
        (cd "$dir" && "$dir/scripts/dev/chart-health.sh") || health_status=$?
        if [[ "$health_status" -ne 0 ]]; then
            if [[ "$health_status" -eq 1 ]] && grep -q '^Chart-Health-Waiver: ' <<< "$trailers"; then
                echo "chart-health: waived by Chart-Health-Waiver trailer"
            else
                echo "error: chart-health failed (exit $health_status)" >&2
                exit "$health_status"
            fi
        fi
        gate doc-health
        health_status=0
        (cd "$dir" && "$dir/scripts/dev/doc-health.sh") || health_status=$?
        if [[ "$health_status" -ne 0 ]]; then
            if [[ "$health_status" -eq 1 ]] && grep -q '^Doc-Health-Waiver: ' <<< "$trailers"; then
                echo "doc-health: waived by Doc-Health-Waiver trailer"
            else
                echo "error: doc-health failed (exit $health_status)" >&2
                exit "$health_status"
            fi
        fi
        echo "building and testing $branch at $new_tip..."
        gate build
        logs+="build.log"$'\n'
        echo "build log: $evidence/build.log"
        (cd "$dir" && "$REPO/scripts/dev/build-queue.sh" --label "land $branch (build)" -- cmake --build --preset mac-clang -j 6 > "$evidence/build.log" 2>&1) ||
            { echo "error: build failed, see $evidence/build.log" >&2; exit 1; }
        gate ctest
        logs+="ctest.log"$'\n'
        echo "ctest log: $evidence/ctest.log"
        ctest_status=0
        (cd "$dir" && SENTINEL_REQUIRE_GPU=1 "$REPO/scripts/dev/build-queue.sh" --label "land $branch (ctest)" -- ctest --test-dir build/mac-clang --output-on-failure --timeout 600 -j 4 > "$evidence/ctest.log" 2>&1) || ctest_status=$?
        ctest_summary=$(grep -E "tests passed" "$evidence/ctest.log" | tail -1) || true
        [[ "$ctest_status" -eq 0 ]] ||
            { tail -20 "$evidence/ctest.log" >&2; echo "error: ctest failed, see $evidence/ctest.log" >&2; exit 1; }
        grep -E "tests passed" "$evidence/ctest.log" || true
        if [[ "$old_tip" != "$new_tip" ]]; then
            gate range-diff
            changed=$(git -C "$REPO" range-diff "$old_base..$old_tip" "$main_tip..$new_tip" | grep -c '^[0-9-]*: *[0-9a-f-]* ! ' || true)
            if [[ "$changed" -gt 0 && "$confirmed" -eq 0 ]]; then
                git -C "$REPO" range-diff "$old_base..$old_tip" "$main_tip..$new_tip"
                echo "stop: the rebase changed $changed commit(s) (above). Review, then rerun with --yes." >&2
                outcome=land_review
                exit 3
            fi
        fi
        gate main-tip
        [[ "$(git -C "$REPO" rev-parse main)" == "$main_tip" ]] ||
            { echo "error: main moved during land; rerun land" >&2; exit 1; }
        gate merge
        merge_status=0
        if [[ -n "$trailers" ]]; then
            git -C "$REPO" merge --no-ff -q "$branch" -m "$message" -m "${trailers%$'\n'}" || merge_status=$?
        else
            git -C "$REPO" merge --no-ff -q "$branch" -m "$message" || merge_status=$?
        fi
        [[ "$merge_status" -eq 0 ]] || exit "$merge_status"
        # Merged: from here on nothing reports this branch as unmerged (land_finish).
        merged=1
        merge_sha=$(git -C "$REPO" rev-parse HEAD)
        merge_short=$(git -C "$REPO" rev-parse --short HEAD)
        echo "landed $branch as $merge_short"
        # Everything that can be finished happens before the worktree goes; any evidence
        # problem (including earlier ones) keeps the worktree and exits 4.
        gate evidence
        scan_codex
        transfer_events
        outcome=landed final_rc=0
        gate remove
        [[ -n "$problems" ]] || write_record pending || add_problem "landing.json not written"
        [[ -n "$problems" ]] || send_event landed ok "$branch landed as $merge_short"
        [[ -n "$problems" ]] || verify_record
        # Last step before removal: drain events appended since the first transfer.
        transferred=$transfer
        [[ -n "$problems" ]] || transfer_events
        if [[ -z "$problems" && "$transfer" != "$transferred" ]]; then
            write_record pending || add_problem "landing.json not written"
            [[ -n "$problems" ]] || verify_record
        fi
        [[ -z "$problems" ]] || { outcome=landed_evidence_incomplete; exit 4; }
        "$0" remove "$branch" || { add_problem "worktree remove failed"; outcome=landed_evidence_incomplete; exit 4; }
        land_complete=1
        echo "queue rule: rebase and retest every other READY branch before it lands (run land on each)."
        ;;
    list)
        git -C "$REPO" worktree list
        ;;
    *)
        sed -n '2,27p' "$0" | sed 's/^# \{0,1\}//'
        exit 2
        ;;
esac
