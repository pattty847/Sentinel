#!/usr/bin/env python3
"""Tests for scripts/dev/gui-host.py trust rules (no GUI, no network):  python3 scripts/dev/test_gui_host.py

Each test is one way a sandboxed agent could try to make the host run code the owner never reviewed."""
import importlib.util
import json
import os
import signal
import stat
import subprocess
import sys
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("gui_host", os.path.join(HERE, "gui-host.py"))
gh = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gh)

FLAGGED = b"#!/bin/sh\n# --agent-host\nexit 0\n"
UNFLAGGED = b"#!/bin/sh\nexit 0\n"


class HostTrust(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        t = os.path.realpath(self.tmp.name)
        self.repo = os.path.join(t, "repo")
        self.wtroot = os.path.join(t, "worktrees")
        for d in (self.repo, self.wtroot):
            os.makedirs(d)
        self.saved = {k: getattr(gh, k) for k in
                      ("REPO", "WORKTREE_ROOTS", "RUNTIME_DIR", "BLESSED_DIR", "REGISTRY", "SESSIONS_DIR", "PIDFILE")}
        gh.REPO, gh.WORKTREE_ROOTS = self.repo, [self.wtroot]
        gh.RUNTIME_DIR = os.path.join(t, "runtime")
        gh.BLESSED_DIR = os.path.join(gh.RUNTIME_DIR, "blessed")
        gh.REGISTRY = os.path.join(gh.RUNTIME_DIR, "registry.json")
        gh.SESSIONS_DIR = os.path.join(t, "sessions")
        gh.PIDFILE = os.path.join(gh.SESSIONS_DIR, "session.json")
        os.makedirs(gh.SESSIONS_DIR)

    def tearDown(self):
        for k, v in self.saved.items():
            setattr(gh, k, v)
        for root, dirs, files in os.walk(self.tmp.name):  # blessed dirs are 0555
            os.chmod(root, 0o755)
        self.tmp.cleanup()

    def binary(self, root, content=FLAGGED, mode=0o755):
        path = os.path.join(root, gh.GUI_REL)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(content)
        os.chmod(path, mode)
        return path

    def worktree(self, name="lt-x"):
        wt = os.path.join(self.wtroot, name)
        os.makedirs(wt)
        return wt, self.binary(wt)

    def assertRefused(self, code, fn, *a):
        with self.assertRaises(gh.HostError) as cm:
            fn(*a)
        self.assertEqual(cm.exception.code, code)

    # ---- which binary may run
    def test_main_is_refused_when_it_lacks_the_flag(self):
        self.binary(self.repo, UNFLAGGED)
        self.assertRefused("no_agent_host_flag", gh.resolve_binary, "main")

    def test_main_with_the_flag_is_accepted(self):
        path = self.binary(self.repo)
        self.assertEqual(gh.resolve_binary("main"), (os.path.realpath(path), "main"))

    def test_main_that_is_group_writable_is_refused(self):
        self.binary(self.repo, mode=0o775)
        self.assertRefused("writable_binary", gh.resolve_binary, "main")

    def test_paths_and_traversal_are_not_binary_names(self):
        wt, path = self.worktree()
        for bad in (path, wt, "../x", "main/../..", "/bin/sh", "ABCDEF123456", "abc", "0123456789abcdeg"):
            self.assertRefused("bad_binary", gh.resolve_binary, bad)

    def test_unblessed_id_is_refused(self):
        self.assertRefused("unknown_binary", gh.resolve_binary, "0123456789ab")

    def test_launch_refuses_a_worktree_parameter(self):
        wt, _ = self.worktree()
        self.assertRefused("no_worktree_launch", gh.launch, {"worktree": wt})

    # ---- blessing
    def test_bless_copies_read_only_and_resolves(self):
        wt, src = self.worktree()
        bid, entry = gh.bless(wt)
        path, label = gh.resolve_binary(bid)
        self.assertEqual(label, bid)
        self.assertEqual(entry["sha256"], gh.sha256_file(src))
        self.assertNotEqual(path, os.path.realpath(src), "must run the copy, never the agent-writable build")
        self.assertTrue(path.startswith(os.path.realpath(gh.BLESSED_DIR) + os.sep))
        self.assertFalse(os.stat(path).st_mode & (stat.S_IWUSR | stat.S_IWGRP | stat.S_IWOTH))
        self.assertFalse(os.stat(os.path.dirname(path)).st_mode & stat.S_IWUSR)

    def test_editing_the_source_after_bless_does_not_change_what_runs(self):
        wt, src = self.worktree()
        bid, _ = gh.bless(wt)
        with open(src, "wb") as f:  # the agent rewrites its build output after review
            f.write(FLAGGED + b"# evil\n")
        path, _ = gh.resolve_binary(bid)
        self.assertNotIn(b"evil", open(path, "rb").read())

    def test_tampered_blessed_copy_is_refused_by_hash(self):
        wt, _ = self.worktree()
        bid, _ = gh.bless(wt)
        d = os.path.join(gh.BLESSED_DIR, bid)
        os.chmod(d, 0o755)
        p = os.path.join(d, "sentinel-gui")
        os.chmod(p, 0o755)
        with open(p, "ab") as f:
            f.write(b"# tampered\n")
        os.chmod(p, 0o555)
        self.assertRefused("hash_mismatch", gh.resolve_binary, bid)

    def test_writable_blessed_copy_is_refused(self):
        wt, _ = self.worktree()
        bid, _ = gh.bless(wt)
        os.chmod(os.path.join(gh.BLESSED_DIR, bid, "sentinel-gui"), 0o755)
        self.assertRefused("writable_binary", gh.resolve_binary, bid)

    def test_symlinked_blessed_entry_is_refused(self):
        wt, src = self.worktree()
        bid, _ = gh.bless(wt)
        d = os.path.join(gh.BLESSED_DIR, bid)
        os.chmod(d, 0o755)
        os.remove(os.path.join(d, "sentinel-gui"))
        os.symlink(src, os.path.join(d, "sentinel-gui"))  # points at the agent-writable build
        self.assertRefused("bad_blessed_path", gh.resolve_binary, bid)

    def test_bless_refuses_a_binary_without_the_flag(self):
        wt = os.path.join(self.wtroot, "lt-old")
        os.makedirs(wt)
        self.binary(wt, UNFLAGGED)
        self.assertRefused("no_agent_host_flag", gh.bless, wt)

    def test_bless_refuses_paths_outside_the_roots(self):
        outside = os.path.join(os.path.realpath(self.tmp.name), "elsewhere")
        os.makedirs(outside)
        self.binary(outside)
        self.assertRefused("bad_worktree", gh.bless, outside)
        link = os.path.join(self.wtroot, "link")
        os.symlink(outside, link)  # a child of the root that really lives outside it
        self.assertRefused("bad_worktree", gh.bless, link)

    # ---- screenshots
    def test_shot_refuses_screen_grabs_and_bad_names(self):
        for target in ("main", "screen", "heatmap/../x", "settings:", "MAIN"):
            self.assertRefused("bad_target", gh.shot, {"name": "a", "target": target})
        for name in ("", "../x", "a/b", "x" * 65, "a b"):
            self.assertRefused("bad_name", gh.shot, {"name": name})

    # ---- stale GUI after a host SIGKILL
    def test_cleanup_ends_a_stale_agent_host_gui_but_not_a_recycled_pid(self):
        stale = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)", "sentinel-gui", "--agent-host", "x"],
                                 start_new_session=True)
        other = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"], start_new_session=True)
        try:
            with open(gh.PIDFILE, "w") as f:
                json.dump({"pid": stale.pid}, f)
            gh.cleanup_stale()
            self.assertIsNotNone(stale.wait(10), "the stale agent-host GUI must be ended")
            with open(gh.PIDFILE, "w") as f:
                json.dump({"pid": other.pid}, f)  # a recycled pid: some unrelated process
            gh.cleanup_stale()
            time.sleep(0.3)
            self.assertIsNone(other.poll(), "an unrelated process must not be killed")
        finally:
            for p in (stale, other):
                if p.poll() is None:
                    os.killpg(p.pid, signal.SIGKILL)
                p.wait()


if __name__ == "__main__":
    unittest.main(verbosity=2)
