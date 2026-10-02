#!/usr/bin/env python3
"""Tests for scripts/dev/gui-host.py trust rules (no GUI, no network):  python3 scripts/dev/test_gui_host.py

Each test is one way a sandboxed agent could try to make the host run, or hand the GUI, something the
owner never reviewed."""
import importlib.util
import json
import os
import re
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
        os.makedirs(self.repo)
        self.saved = {k: getattr(gh, k) for k in ("REPO", "SESSIONS_DIR", "PIDFILE")}
        gh.REPO = self.repo
        gh.SESSIONS_DIR = os.path.join(t, "sessions")
        gh.PIDFILE = os.path.join(gh.SESSIONS_DIR, "session.json")
        os.makedirs(gh.SESSIONS_DIR)

    def tearDown(self):
        for k, v in self.saved.items():
            setattr(gh, k, v)
        self.tmp.cleanup()

    def binary(self, content=FLAGGED, mode=0o755):
        path = os.path.join(self.repo, gh.GUI_REL)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(content)
        os.chmod(path, mode)
        return path

    def assertRefused(self, code, fn, *a):
        with self.assertRaises(gh.HostError) as cm:
            fn(*a)
        self.assertEqual(cm.exception.code, code)

    # ---- which binary may run: only the main checkout's build
    def test_main_without_the_flag_is_refused(self):
        self.binary(UNFLAGGED)  # a stale main must not run uncontained
        self.assertRefused("no_agent_host_flag", gh.resolve_binary, "main")

    def test_main_with_the_flag_is_accepted(self):
        path = self.binary()
        self.assertEqual(gh.resolve_binary("main"), os.path.realpath(path))
        self.assertEqual(gh.resolve_binary(None), os.path.realpath(path))

    def test_missing_main_is_refused(self):
        self.assertRefused("no_main_binary", gh.resolve_binary, "main")

    def test_group_or_world_writable_main_is_refused(self):
        for mode in (0o775, 0o757):
            self.binary(mode=mode)
            self.assertRefused("writable_binary", gh.resolve_binary, "main")

    def test_main_symlink_is_resolved_not_trusted_blindly(self):
        elsewhere = os.path.join(os.path.realpath(self.tmp.name), "agent-build")
        with open(elsewhere, "wb") as f:
            f.write(UNFLAGGED)
        os.chmod(elsewhere, 0o755)
        link = os.path.join(self.repo, gh.GUI_REL)
        os.makedirs(os.path.dirname(link))
        os.symlink(elsewhere, link)  # the flag check runs on the real file, so it still refuses
        self.assertRefused("no_agent_host_flag", gh.resolve_binary, "main")

    def test_any_other_binary_name_or_path_is_refused(self):
        self.binary()
        for bad in ("/bin/sh", self.repo, "5016ccf3909a", "../x", "MAIN", "", "main/../..", "worktree"):
            self.assertRefused("main_only", gh.resolve_binary, bad)

    def test_launch_refuses_worktree_and_path_parameters(self):
        self.binary()
        for key in ("worktree", "path"):
            self.assertRefused("main_only", gh.launch, {key: "/Volumes/T7/sentinel-worktrees/lt-x"})
        self.assertRefused("main_only", gh.launch, {"binary": "/Volumes/T7/sentinel-worktrees/lt-x/build/gui"})

    # ---- the scripts agents run must be executable (a rewrite with a tool that creates a fresh file drops the bit)
    def test_scripts_are_executable(self):
        for name in ("gui-host.py", "gui-shot.sh"):
            self.assertTrue(os.access(os.path.join(HERE, name), os.X_OK), name)

    # ---- what the child gets
    def test_child_env_keeps_only_the_allowlist_and_a_fixed_path(self):
        env = gh.child_env({"HOME": "/h", "USER": "u", "PATH": "/agent/bin:/usr/bin", "DYLD_INSERT_LIBRARIES": "/x.dylib",
                            "DYLD_LIBRARY_PATH": "/x", "QT_PLUGIN_PATH": "/x", "QML_IMPORT_PATH": "/x", "QML2_IMPORT_PATH": "/x",
                            "QSG_RHI_BACKEND": "x", "SENTINEL_QML_PATH": "/x", "SENTINEL_GUI_SCREENSHOT_DIR": "/x",
                            "QT_QPA_PLATFORM_PLUGIN_PATH": "/x"})
        self.assertEqual(env, {"HOME": "/h", "USER": "u", "PATH": gh.SAFE_PATH})

    def test_gui_argv_is_fixed_flags_with_no_caller_supplied_arguments(self):
        argv = gh.gui_argv("/bin/gui", "/sess", "gpu", 17130)
        self.assertEqual(argv[0], "/bin/gui")
        self.assertEqual(argv[1:3], ["--agent-host", "/sess"])
        self.assertEqual(argv[3:5], ["--agent-host-symbols", gh.SYMBOLS])
        self.assertEqual(argv[5:], ["--heatmap-renderer", "gpu", "--api-port", "17130", "--no-screener"])
        self.assertIn("BTC-USD", gh.SYMBOLS.split(","))
        self.assertTrue(all(re.fullmatch(r"[A-Z0-9]{2,20}-[A-Z0-9]{2,20}", x) for x in gh.SYMBOLS.split(",")))

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
