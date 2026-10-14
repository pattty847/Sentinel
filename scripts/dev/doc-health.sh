#!/usr/bin/env bash
# Documentation caps and ratchets. Run from any checkout; --report never blocks curation.
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
DOC_HEALTH_PENDING_PRUNE = "INVARIANTS.md DECISIONS.md"

ENTRY = re.compile(r"^- (?:FM-|INV-|\d{4}-\d{2}-\d{2} \||AREA:)")
ID = re.compile(r"^- ((?:FM|INV)-\d+)(?![\w-])")
POINTER = re.compile(
    r"(?:[\w.-]+/)+[\w.-]+\.[\w]+(?::\d+)?"
    r"|[\w.-]+\.[\w]+:\d+"
    r"|(?<![\w.])[A-Z]\w*\.[A-Za-z_]\w*(?![\w.])"
    r"|\b[A-Za-z_]\w*::\w+(?:\(\))?"
    r"|\b\w+\(\)"
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


def entry_blocks(text):
    """Blank lines end entries; other non-entry lines continue the current one."""
    block = []
    start = 0
    for number, line in enumerate(text.splitlines(), 1):
        if ENTRY.match(line) or not line.strip():
            if block:
                yield start, tuple(block)
            block = [line] if ENTRY.match(line) else []
            start = number
        elif block:
            block.append(line)
    if block:
        yield start, tuple(block)


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
        exempt = is_memory and path.name in DOC_HEALTH_PENDING_PRUNE.split()
        breach = over and (not exempt if is_memory else grew)
        state = ("FAIL" if breach else "over cap (pending prune)" if over and exempt
                 else "over cap, ratchet holds" if over else "OK")
        old_size = "missing" if old is None else str(len(old))
        details = f"cap={cap}" + ("" if is_memory else f", baseline={old_size}")
        print(f"doc-health: size {name}: {len(data)} bytes ({details}) {state}")
        if breach:
            failures.append(f"size {name}: {len(data)} > {cap} bytes"
                            + ("" if is_memory else f" and baseline {old_size}"))
        if not is_memory:
            continue
        text = data.decode("utf-8", errors="surrogateescape")
        old_entries = Counter(block for _, block in entry_blocks(
            (old or b"").decode("utf-8", errors="surrogateescape")))
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
        for number, block in entry_blocks(text):
            unchanged = old_entries[block] > 0
            if unchanged:
                old_entries[block] -= 1
            line = block[0]
            label = entry_label(line, name, number)
            length = sum(len(part) for part in block)
            if not unchanged and length > ENTRY_CAP:
                failures.append(f"entry {name}:{number} {label}: {length} > {ENTRY_CAP} characters")
            match = ID.match(line)
            if match:
                ids[match[1]] += 1
                if match[1] in archived_ids:
                    failures.append(f"archive {name}:{number}: {match[1]} also appears in archive/agent/{path.name}")
            content = "\n".join(block)
            tokens = set(re.findall(r"`([^`\n]+)`", content)) | set(POINTER.findall(content))
            # Lowercase dotted identifiers are config keys, not gtest names.
            # Preserve a quoted filename when it actually exists in the checkout.
            tokens = {token for token in tokens
                      if not re.fullmatch(r"[a-z_]\w*(?:\.\w+)+", token) or (root / token).exists()}
            entries.append((name, label, tokens))
        for ident, count in sorted(ids.items()):
            if count > 1:
                failures.append(f"duplicate {name}: {ident} appears {count} times")

    # Paths resolve only within this checkout. Other tokens share one fixed-string
    # rg scan over source directories, avoiding a process/tree walk per entry.
    raw_tokens = set().union(*(item[2] for item in entries)) if entries else set()
    normalized = {token: token.removesuffix("()") for token in raw_tokens}
    tokens = set(normalized.values())
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
                      if re.fullmatch(r"[A-Z]\w*\.[A-Za-z_]\w*", token)}
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
    no_pointer = [item for item in entries if not item[2]]
    live = [item for item in entries if any(normalized[token] in resolved for token in item[2])]
    dead = [f"{name}:{label} {sorted(pointers)[0]}" for name, label, pointers in entries
            if pointers and not any(normalized[token] in resolved for token in pointers)]
    print(f"doc-health: no pointer: {len(no_pointer)}")
    print(f"doc-health: resolved pointers: {len(live)}")
    print(f"doc-health: warning: dead pointers: {len(dead)}"
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
