#!/usr/bin/env bash
# Documentation ratchets. Run from any checkout; --report never blocks curation.
set -euo pipefail
exec python3 - "$@" <<'PY'
from collections import Counter
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

# Byte caps and the character cap live together so curation can ratchet them down.
STATUS_CAP = 8 * 1024
TODO_CAP = 12 * 1024
FAILURE_MODES_CAP = 25 * 1024
INVARIANTS_CAP = 25 * 1024
DECISIONS_CAP = 25 * 1024
REPO_MAP_CAP = 5 * 1024
ENTRY_CAP = 350

ENTRY = re.compile(r"^- (?:FM-|INV-|\d{4}-\d{2}-\d{2} \||AREA:)")
ID = re.compile(r"^- ((?:FM|INV)-\d+)(?=\s|\||$)")
POINTER = re.compile(
    r"(?:[\w.-]+/)+[\w.-]+\.[\w]+(?::\d+)?"
    r"|[\w.-]+\.[\w]+:\d+"
    r"|\b[A-Za-z_]\w*\.[A-Za-z_]\w*\b"
)
GTEST = re.compile(r"\b(?:TEST(?:_F|_P)?|TYPED_TEST(?:_P)?)\s*\(\s*(\w+)\s*,\s*(\w+)\s*\)")
REPORT = sys.argv[1:] == ["--report"]


def git(repo, *args, check=True):
    return subprocess.run(["git", "-C", str(repo), *args], check=check,
                          capture_output=True, text=True)


def baseline(repo, revision, name):
    if revision is None:
        return None
    # Listing the path distinguishes an absent baseline from a failed Git read.
    if not git(repo, "ls-tree", "--name-only", revision, "--", name).stdout.strip():
        return None
    return subprocess.run(["git", "-C", str(repo), "show", f"{revision}:{name}"],
                          check=True, capture_output=True).stdout


def entry_label(line, name, number):
    match = ID.match(line)
    if match:
        return match[1]
    return line.split("|", 1)[0].removeprefix("- ").strip() or f"{name}:{number}"


def check():
    root = Path(git(Path.cwd(), "rev-parse", "--show-toplevel").stdout.strip())
    base_result = git(root, "merge-base", "HEAD", "main", check=False)
    # A missing main/ref or common ancestor means there is no Sentinel baseline.
    if base_result.returncode not in (0, 1, 128):
        raise RuntimeError(base_result.stderr.strip())
    base = base_result.stdout.strip() or None
    failures = []
    entries = []
    files = [
        ("docs/STATUS.md", root / "docs/STATUS.md", STATUS_CAP, root, base, False),
        ("docs/TODO.md", root / "docs/TODO.md", TODO_CAP, root, base, False),
    ]
    memory = root / "_agent"
    private = None
    if not memory.exists():
        if memory.is_symlink():
            raise RuntimeError("_agent symlink is broken")
        print("doc-health: notice: _agent missing; memory checks skipped")
    else:
        memory = Path(os.path.realpath(memory))
        private = memory.parent
        private_root = Path(git(private, "rev-parse", "--show-toplevel").stdout.strip())
        if private_root.resolve() != private:
            raise RuntimeError("_agent parent is not the private repository root")
        head = git(private, "rev-parse", "--verify", "HEAD", check=False)
        private_base = head.stdout.strip() if head.returncode == 0 else None
        for name, cap in (("INVARIANTS.md", INVARIANTS_CAP),
                          ("FAILURE_MODES.md", FAILURE_MODES_CAP),
                          ("DECISIONS.md", DECISIONS_CAP), ("REPO_MAP.md", REPO_MAP_CAP)):
            files.append((f"_agent/{name}", memory / name, cap, private, private_base, True))

    for name, path, cap, repo, revision, is_memory in files:
        old = baseline(repo, revision, name)
        if not path.exists():
            print(f"doc-health: size {name}: missing (cap={cap} bytes)")
            continue
        data = path.read_bytes()
        grew = old is None or len(data) > len(old)
        over = len(data) > cap
        state = "FAIL" if over and grew else "over cap, ratchet holds" if over else "OK"
        old_size = "missing" if old is None else str(len(old))
        print(f"doc-health: size {name}: {len(data)} bytes (cap={cap}, baseline={old_size}) {state}")
        if over and grew:
            failures.append(f"size {name}: {len(data)} > {cap} bytes and baseline {old_size}")
        if not is_memory:
            continue
        text = data.decode("utf-8", errors="surrogateescape")
        old_lines = Counter((old or b"").decode("utf-8", errors="surrogateescape").splitlines())
        ids = Counter()
        archive = private / "archive/agent" / path.name
        archived_ids = set()
        if archive.exists():
            for line in archive.read_bytes().decode("utf-8", errors="surrogateescape").splitlines():
                match = ID.match(line)
                if match:
                    archived_ids.add(match[1])
        for number, line in enumerate(text.splitlines(), 1):
            if not line.isascii():
                failures.append(f"ASCII {name}:{number}: non-ASCII content")
            unchanged = old_lines[line] > 0
            if unchanged:
                old_lines[line] -= 1
            if not ENTRY.match(line):
                continue
            label = entry_label(line, name, number)
            if not unchanged and len(line) > ENTRY_CAP:
                failures.append(f"entry {name}:{number} {label}: {len(line)} > {ENTRY_CAP} characters")
            match = ID.match(line)
            if match:
                ids[match[1]] += 1
                if match[1] in archived_ids:
                    failures.append(f"archive {name}:{number}: {match[1]} also appears in archive/agent/{path.name}")
            tokens = set(re.findall(r"`([^`\n]+)`", line)) | set(POINTER.findall(line))
            entries.append((name, label, tokens))
        for ident, count in sorted(ids.items()):
            if count > 1:
                failures.append(f"duplicate {name}: {ident} appears {count} times")

    # Paths resolve only within this checkout. Other tokens share one fixed-string
    # rg scan over source directories, avoiding a process/tree walk per entry.
    tokens = set().union(*(item[2] for item in entries)) if entries else set()
    resolved = set()
    for token in tokens:
        path = (root / re.sub(r":\d+(?::\d+)?$", "", token)).resolve()
        if path.is_relative_to(root) and path.exists():
            resolved.add(token)
    pending = tokens - resolved
    dirs = [str(root / name) for name in ("libs", "apps", "tests", "scripts", "config", "tools")
            if (root / name).is_dir()]
    if pending and dirs:
        with tempfile.TemporaryDirectory(prefix="doc-health-") as scratch:
            patterns = Path(scratch) / "pointers"
            suites = {token.split(".", 1)[0] for token in pending
                      if re.fullmatch(r"[A-Za-z_]\w*\.[A-Za-z_]\w*", token)}
            patterns.write_text("\n".join(sorted(pending | suites)) + "\n", errors="surrogateescape")
            result = subprocess.run(["rg", "--no-heading", "--no-filename", "--no-line-number",
                                     "--fixed-strings", "--file", str(patterns), "--", *dirs],
                                    capture_output=True, text=True, errors="surrogateescape")
            if result.returncode not in (0, 1):
                raise RuntimeError(f"pointer scan failed: {result.stderr.strip()}")
            # Lookahead preserves overlapping matches; a longer token can also
            # resolve its shorter substrings, just as a separate rg -q would.
            matcher = re.compile("(?=(" + "|".join(re.escape(token) for token in
                                                  sorted(pending, key=lambda t: (-len(t), t))) + "))")
            contained = {token: {other for other in pending if other in token} for token in pending}
            for match in matcher.finditer(result.stdout):
                resolved.update(contained[match[1]])
            for suite, case in GTEST.findall(result.stdout):
                resolved.add(f"{suite}.{case}")
    dead = [f"{name}:{label}" for name, label, pointers in entries if not pointers & resolved]
    print(f"doc-health: warning: unresolved pointers in {len(dead)} entries"
          + (" (up to 20): " + ", ".join(dead[:20]) if dead else ""))
    for failure in failures:
        print(f"doc-health: FAIL {failure}")
    print("doc-health: PASS" if not failures else f"doc-health: FAIL ({len(failures)})")
    return 1 if failures else 0


if sys.argv[1:] not in ([], ["--report"]):
    print("usage: doc-health.sh [--report]", file=sys.stderr)
    sys.exit(2)
try:
    status = check()
except (OSError, subprocess.CalledProcessError, RuntimeError) as error:
    print(f"doc-health: error: {error}", file=sys.stderr)
    status = 2
sys.exit(0 if REPORT else status)
PY
