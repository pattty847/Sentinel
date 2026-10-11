#!/usr/bin/env python3
"""Chart extraction ratchets. Run from any checkout; compare HEAD with its main merge base."""
import csv
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


def run(args, cwd=None, check=True, env=None):
    return subprocess.run(args, cwd=cwd, check=check, text=True, capture_output=True, env=env)


def git(*args):
    return run(["git", *args]).stdout.strip()


def missing_tools():
    absent = []
    if not shutil.which("lizard"):
        absent.append("lizard (install: python3 -m pip install --user lizard==1.24.1)")
    if not shutil.which("npx"):
        absent.append("npx (install: brew install node)")
    if absent:
        for item in absent:
            print(f"chart-health: missing {item}", file=sys.stderr)
        return True
    return False


def self_test(script):
    # An isolated repository avoids changing this checkout's index or worktree list.
    source = Path(git("rev-parse", "--show-toplevel"))
    with tempfile.TemporaryDirectory(prefix="chart-health-self-") as scratch:
        root = Path(scratch)
        (root / "libs/gui").mkdir(parents=True)
        for name in ("UnifiedGridRenderer.cpp", "UnifiedGridRenderer.h"):
            shutil.copy2(source / "libs/gui" / name, root / "libs/gui" / name)
        run(["git", "init", "-q", "-b", "main"], cwd=root)
        env = dict(os.environ, GIT_AUTHOR_NAME="Chart Health", GIT_AUTHOR_EMAIL="chart-health@local",
                   GIT_COMMITTER_NAME="Chart Health", GIT_COMMITTER_EMAIL="chart-health@local")
        run(["git", "add", "libs/gui"], cwd=root)
        run(["git", "commit", "-qm", "baseline"], cwd=root, env=env)
        run(["git", "switch", "-qc", "probe"], cwd=root)
        run(["git", "commit", "-q", "--allow-empty", "-m", "no-op"], cwd=root, env=env)
        noop = run([str(script)], cwd=root, check=False)
        if noop.returncode or "chart-health: PASS" not in noop.stdout:
            print(noop.stdout + noop.stderr, end="", file=sys.stderr)
            print("chart-health: self-test no-op branch failed", file=sys.stderr)
            return 1
        path = root / "libs/gui/UnifiedGridRenderer.cpp"
        code = path.read_text()
        needle = "UnifiedGridRenderer::wheelEvent(QWheelEvent *event)"
        start = code.find(needle)
        if start < 0:
            print("chart-health: self-test function missing", file=sys.stderr)
            return 1
        brace = code.find("{", start)
        path.write_text(code[:brace + 1] + "\n" + "  if (event) {}\n" * 16 + code[brace + 1:])
        run(["git", "add", "libs/gui/UnifiedGridRenderer.cpp"], cwd=root)
        run(["git", "commit", "-qm", "exceed the branch limit"], cwd=root, env=env)
        changed = run([str(script)], cwd=root, check=False)
        if changed.returncode != 1 or "FAIL complexity" not in changed.stdout or "wheelEvent" not in changed.stdout:
            print(changed.stdout + changed.stderr, end="", file=sys.stderr)
            print("chart-health: self-test complexity branch was not rejected", file=sys.stderr)
            return 1
        run(["git", "switch", "-qc", "moved", "main"], cwd=root)
        code = path.read_text()
        start = code.index("void UnifiedGridRenderer::wheelEvent(")
        brace = code.index("{", start)
        depth = 0
        for end in range(brace, len(code)):
            depth += (code[end] == "{") - (code[end] == "}")
            if depth == 0:
                break
        else:
            print("chart-health: self-test could not find function end", file=sys.stderr)
            return 1
        moved_file = root / "libs/gui/chart/shell/WheelEvent.cpp"
        moved_file.parent.mkdir(parents=True)
        moved_file.write_text(code[start:end + 1] + "\n")
        path.write_text(code[:start] + code[end + 1:])
        run(["git", "add", "libs/gui"], cwd=root)
        run(["git", "commit", "-qm", "move function verbatim"], cwd=root, env=env)
        moved = run([str(script)], cwd=root, check=False)
        if moved.returncode or "chart-health: PASS" not in moved.stdout:
            print(moved.stdout + moved.stderr, end="", file=sys.stderr)
            print("chart-health: self-test verbatim move was rejected", file=sys.stderr)
            return 1
        run(["git", "switch", "-qc", "outside", "main"], cwd=root)
        unrelated = root / "libs/core/Unrelated.cpp"
        unrelated.parent.mkdir(parents=True)
        unrelated.write_text("int unrelated(int x) {\n" + "".join(f"  if (x == {i}) return {i};\n" for i in range(20)) + "  return -1;\n}\n")
        run(["git", "add", "libs/core/Unrelated.cpp"], cwd=root)
        run(["git", "commit", "-qm", "unrelated C++"], cwd=root, env=env)
        outside = run([str(script)], cwd=root, check=False)
        if outside.returncode or "chart-health: PASS" not in outside.stdout:
            print(outside.stdout + outside.stderr, end="", file=sys.stderr)
            print("chart-health: self-test unrelated C++ was rejected", file=sys.stderr)
            return 1
        include = root / "libs/gui/Outside.cpp"
        include.write_text('#include "UnifiedGridRenderer.h"\n')
        run(["git", "add", "libs/gui/Outside.cpp"], cwd=root)
        run(["git", "commit", "-qm", "illegal UGR include"], cwd=root, env=env)
        boundary = run([str(script)], cwd=root, check=False)
        if boundary.returncode != 1 or "FAIL UGR-include" not in boundary.stdout:
            print(boundary.stdout + boundary.stderr, end="", file=sys.stderr)
            print("chart-health: self-test UGR include was not rejected", file=sys.stderr)
            return 1
    print("chart-health: self-test PASS (no-op and verbatim move passed; +if and outside include failed; unrelated C++ passed)")
    return 0


def lizard_functions(path):
    result = run(["lizard", "-l", "cpp", "--csv", str(path)])
    functions = {}
    for row in csv.reader(io.StringIO(result.stdout)):
        if len(row) < 8 or not row[0].isdigit():
            continue
        # csv: NLOC,CCN,tokens,params,length,location,file,name,signature,...
        if len(row) < 9:
            continue
        functions[row[8]] = (int(row[1]), int(row[0]), int(row[3]), row[7].rsplit("::", 1)[-1])
    return functions


def source_at(base, name):
    result = run(["git", "show", f"{base}:{name}"], check=False)
    return result.stdout if result.returncode == 0 else None


def members(source):
    return sum(bool(re.search(r"^\s+[^/(]*\bm_[a-zA-Z]", line)) for line in source.splitlines())


def chart_file(name):
    return name.startswith("libs/gui/chart/") or name.startswith("libs/gui/UnifiedGridRenderer")


def baseline_ccn(base):
    """Qualified signatures identify local functions; unqualified names find verbatim moves."""
    paths = [p for p in git("ls-tree", "-r", "--name-only", base, "libs/gui").splitlines()
             if chart_file(p) and p.endswith((".cpp", ".hpp", ".h"))]
    by_name = {}
    with tempfile.TemporaryDirectory(prefix="chart-health-baseline-") as scratch:
        for index, name in enumerate(paths):
            source = source_at(base, name)
            if source is None:
                continue
            path = Path(scratch) / f"{index}{Path(name).suffix}"
            path.write_text(source)
            for ccn, _, _, unqualified in lizard_functions(path).values():
                by_name[unqualified] = max(ccn, by_name.get(unqualified, 0))
    return by_name


def scan_clones(root, paths, out):
    if len(paths) < 2:
        return set()
    cmd = ["npx", "--yes", "jscpd@4.0.5", "--min-lines", "8", "--min-tokens", "60",
           "--format", "cpp", "--reporters", "json", "-o", str(out), *paths]
    env = dict(os.environ)
    env.setdefault("npm_config_cache", str(Path(tempfile.gettempdir()) / "chart-health-npm"))
    result = run(cmd, cwd=root, check=False, env=env)
    report = out / "jscpd-report.json"
    if result.returncode not in (0, 1) or not report.exists():
        raise RuntimeError(f"jscpd failed: {result.stderr.strip() or result.stdout.strip()}")
    data = json.loads(report.read_text())
    clones = set()
    for clone in data.get("duplicates", []):
        names = tuple(sorted(Path(clone[k]["name"]).name if "UnifiedGridRenderer" in clone[k]["name"] else
                             str(Path(clone[k]["name"]).relative_to(root)) if Path(clone[k]["name"]).is_absolute() else
                             clone[k]["name"] for k in ("firstFile", "secondFile")))
        fingerprint = hashlib.sha256(clone["fragment"].encode()).hexdigest()
        clones.add((names, fingerprint))
    return clones


def check():
    root = Path(git("rev-parse", "--show-toplevel"))
    os.chdir(root)
    base = git("merge-base", "HEAD", "main")
    changed = [p for p in git("diff", "--name-only", "--diff-filter=ACMR", base, "HEAD").splitlines() if p]
    breaches = []
    old_functions_by_name = None
    def fail(kind, name, old, new):
        location = name if ":" in name else f"{name}:file"
        breaches.append(f"chart-health: FAIL {kind} {location} {old} -> {new}")
    def files_at(ref):
        return [p for p in git("ls-tree", "-r", "--name-only", ref, "libs/gui").splitlines()
                if (p.startswith("libs/gui/chart/") or
                    (Path(p).name.startswith("UnifiedGridRenderer") and p.endswith(".cpp")))]
    for name in changed:
        path = root / name
        if not path.is_file():
            continue
        current = path.read_text(errors="replace")
        old = source_at(base, name)
        # The UGR include boundary is repo-wide within libs/gui. All other
        # budgets apply only to chart-core and UGR source files.
        if name.startswith("libs/gui/") and "UnifiedGridRenderer.h" in current and "UnifiedGridRenderer.h" not in (old or ""):
            allowed = name.startswith(("libs/gui/UnifiedGridRenderer", "libs/gui/MainWindowGpu", "libs/gui/mainwindow/"))
            if not allowed: fail("UGR-include", name, 0, 1)
        if not chart_file(name):
            continue
        if name.startswith("libs/gui/UnifiedGridRenderer"):
            old_lines = len(old.splitlines()) if old is not None else 0
            new_lines = len(current.splitlines())
            if new_lines > old_lines:
                fail("UGR-lines", name, old_lines, new_lines)
            if os.getenv("CHART_HEALTH_FINAL") == "1":
                cap = 350 if name.endswith(".h") else 450 if name.endswith(".cpp") else None
                if cap is not None and new_lines > cap:
                    fail("UGR-final-lines", name, cap, new_lines)
            if name == "libs/gui/UnifiedGridRenderer.h":
                before = members(old or "")
                after = members(current)
                if after > before: fail("UGR-members", name, before, after)
                if os.getenv("CHART_HEALTH_FINAL") == "1" and after > 15:
                    fail("UGR-final-members", name, 15, after)
        if name.startswith("libs/gui/chart/"):
            lines = len(current.splitlines())
            if lines > 500: fail("chart-lines", name, 500, lines)
            elif lines > 400: print(f"chart-health: WARN chart-lines {name} 400 -> {lines}")
            if name.startswith("libs/gui/chart/core/"):
                forbidden = re.findall(r'^\s*#include\s*[<"](?:QQuick|QSG|QtGui|QtQuick|QQml)', current, re.M)
                before = re.findall(r'^\s*#include\s*[<"](?:QQuick|QSG|QtGui|QtQuick|QQml)', old or "", re.M)
                if len(forbidden) > len(before): fail("core-include", name, len(before), len(forbidden))
        if name.endswith((".cpp", ".hpp", ".h")):
            with tempfile.TemporaryDirectory(prefix="chart-health-lizard-") as scratch:
                old_path = Path(scratch) / ("old" + path.suffix)
                if old is not None: old_path.write_text(old)
                before = lizard_functions(old_path) if old is not None else {}
                after = lizard_functions(path)
            for func, (ccn, nloc, params, unqualified) in after.items():
                prior = before.get(func)
                if prior is None:
                    if old_functions_by_name is None:
                        old_functions_by_name = baseline_ccn(base)
                    moved_ccn = old_functions_by_name.get(unqualified)
                    if moved_ccn is not None:
                        if ccn > max(15, moved_ccn):
                            fail("complexity", f"{name}:{func}", moved_ccn, ccn)
                    else:
                        limit = 10 if "ChartViewController" in name else 15
                        if ccn > limit: fail("complexity", f"{name}:{func}", limit, ccn)
                        if nloc > 60: fail("NLOC", f"{name}:{func}", 60, nloc)
                        if params > 6: fail("params", f"{name}:{func}", 6, params)
                elif ccn > max(15, prior[0]):
                    fail("complexity", f"{name}:{func}", prior[0], ccn)
                if name == "libs/gui/UnifiedGridRenderer.Qml.cpp" and nloc > 3:
                    fail("forwarder-NLOC", f"{name}:{func}", 3, nloc)
    changed_scoped = set(changed) & set(files_at("HEAD"))
    if changed_scoped:
        with tempfile.TemporaryDirectory(prefix="chart-health-clones-") as scratch:
            temp = Path(scratch)
            baseline = temp / "baseline"
            for name in files_at(base):
                content = source_at(base, name)
                if content is None: continue
                target = baseline / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(content)
            current_paths = [p for p in files_at("HEAD") if (root / p).exists()]
            old_paths = [p for p in files_at(base) if (baseline / p).exists()]
            old_clones = scan_clones(baseline, old_paths, temp / "old-report")
            new_clones = scan_clones(root, current_paths, temp / "new-report")
            for names, fragment in sorted(new_clones - old_clones):
                if any(any(Path(p).name == name or p == name for p in changed_scoped) for name in names):
                    fail("clone", ",".join(names), "absent", fragment[:12])
    for breach in breaches: print(breach)
    print("chart-health: PASS" if not breaches else f"chart-health: FAIL ({len(breaches)})")
    return 1 if breaches else 0


if __name__ == "__main__":
    if missing_tools(): sys.exit(2)
    try:
        if sys.argv[1:] == ["--self-test"]:
            sys.exit(self_test(Path(__file__).resolve()))
        if len(sys.argv) != 1:
            print("usage: chart-health.sh [--self-test]", file=sys.stderr)
            sys.exit(2)
        sys.exit(check())
    except (subprocess.CalledProcessError, RuntimeError) as error:
        print(f"chart-health: error: {error}", file=sys.stderr)
        sys.exit(2)
