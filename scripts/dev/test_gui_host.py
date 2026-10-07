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
from unittest.mock import patch

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("gui_host", os.path.join(HERE, "gui-host.py"))
gh = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gh)

FLAGGED = b"\xcf\xfa\xed\xfe" + b"\0" * 20 + b"--agent-host\n"
UNFLAGGED = b"\xcf\xfa\xed\xfe" + b"\0" * 20


class HostTrust(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        t = os.path.realpath(self.tmp.name)
        self.repo = os.path.join(t, "repo")
        os.makedirs(self.repo)
        self.saved = {k: getattr(gh, k) for k in ("REPO", "SESSIONS_DIR", "PIDFILE", "EXTERNAL_WORKTREES")}
        gh.REPO = self.repo
        gh.EXTERNAL_WORKTREES = os.path.join(t, "sentinel-worktrees")
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

    # ---- which binary may run: main by default, or the fixed binary in an approved worktree
    def worktree_binary(self, root=None, name="lt-sol-dock-infra", content=FLAGGED, mode=0o755):
        root = root or gh.EXTERNAL_WORKTREES
        worktree = os.path.join(root, name)
        path = os.path.join(worktree, gh.GUI_REL)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(content)
        os.chmod(path, mode)
        return worktree, path

    def test_main_without_the_flag_is_refused(self):
        self.binary(UNFLAGGED)  # a stale main must not run uncontained
        self.assertRefused("no_agent_host_flag", gh.resolve_binary, "main")

    def test_main_with_the_flag_is_accepted(self):
        path = self.binary()
        self.assertEqual(gh.resolve_binary("main"), os.path.realpath(path))
        self.assertEqual(gh.resolve_binary(None), os.path.realpath(path))
        self.assertEqual(gh.resolve_binary(), os.path.realpath(path))

    def test_worktree_and_non_t7_fallback_fixed_builds_are_accepted(self):
        for root in (gh.EXTERNAL_WORKTREES, os.path.join(self.repo, ".claude", "worktrees")):
            worktree, path = self.worktree_binary(root)
            self.assertEqual(gh.resolve_binary(build=worktree), os.path.realpath(path))

    def test_outside_root_and_nested_worktree_are_refused(self):
        outside, _ = self.worktree_binary(os.path.join(self.tmp.name, "unapproved"))
        self.assertRefused("outside_worktrees", gh.resolve_binary, "main", outside)
        nested, _ = self.worktree_binary(os.path.join(gh.EXTERNAL_WORKTREES, "nested"))
        self.assertRefused("outside_worktrees", gh.resolve_binary, "main", nested)

    def test_worktree_symlink_escape_is_refused(self):
        outside, path = self.worktree_binary(os.path.join(self.tmp.name, "unapproved"))
        os.makedirs(gh.EXTERNAL_WORKTREES)
        alias = os.path.join(gh.EXTERNAL_WORKTREES, "lt-escape")
        os.symlink(outside, alias)
        self.assertRefused("outside_worktrees", gh.resolve_binary, "main", alias)
        inside, binary = self.worktree_binary(name="lt-binary-link")
        os.remove(binary)
        os.symlink(path, binary)
        self.assertRefused("outside_build", gh.resolve_binary, "main", inside)

    def test_non_binary_worktree_build_is_refused(self):
        worktree, path = self.worktree_binary(mode=0o644)
        self.assertRefused("no_worktree_binary", gh.resolve_binary, "main", worktree)
        os.chmod(path, 0o755)
        with open(path, "wb") as f:
            f.write(UNFLAGGED)
        self.assertRefused("no_agent_host_flag", gh.resolve_binary, "main", worktree)
        with open(path, "wb") as f:
            f.write(b"#!/bin/sh\n# --agent-host\n")
        self.assertRefused("not_gui_binary", gh.resolve_binary, "main", worktree)

    def test_missing_main_is_refused(self):
        self.assertRefused("no_main_binary", gh.resolve_binary, "main")

    def test_group_or_world_writable_main_is_refused(self):
        for mode in (0o775, 0o757):
            self.binary(mode=mode)
            self.assertRefused("writable_binary", gh.resolve_binary, "main")

    def test_main_symlink_out_of_the_checkout_is_refused_even_with_the_flag(self):
        elsewhere = os.path.join(os.path.realpath(self.tmp.name), "agent-build")
        with open(elsewhere, "wb") as f:
            f.write(FLAGGED)  # carries the flag, owned by us, not writable by others: only containment can refuse it
        os.chmod(elsewhere, 0o755)
        link = os.path.join(self.repo, gh.GUI_REL)
        os.makedirs(os.path.dirname(link))
        os.symlink(elsewhere, link)
        self.assertRefused("outside_main", gh.resolve_binary, "main")

    def test_any_other_binary_name_or_path_is_refused(self):
        self.binary()
        for bad in ("/bin/sh", self.repo, "5016ccf3909a", "../x", "MAIN", "", "main/../..", "worktree"):
            self.assertRefused("main_only", gh.resolve_binary, bad)

    def test_launch_refuses_worktree_and_path_parameters(self):
        self.binary()
        for key in ("worktree", "path"):
            self.assertRefused("bad_build", gh.launch, {key: "/Volumes/T7/sentinel-worktrees/lt-x"})
        self.assertRefused("main_only", gh.launch, {"binary": "/Volumes/T7/sentinel-worktrees/lt-x/build/gui"})

    def test_launch_refuses_the_removed_legacy_renderer(self):
        self.binary()
        for renderer in ("legacy", "", "GPU", None, 1):
            self.assertRefused("bad_renderer", gh.launch, {"renderer": renderer})

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
        argv = gh.gui_argv("/bin/gui", "/sess", 17130)
        self.assertEqual(argv[0], "/bin/gui")
        self.assertEqual(argv[1:3], ["--agent-host", "/sess"])
        self.assertEqual(argv[3:5], ["--agent-host-profile", gh.profile_dir()])
        self.assertEqual(argv[5:7], ["--agent-host-symbols", gh.SYMBOLS])
        self.assertEqual(argv[7:], ["--api-port", "17130", "--no-screener"])
        self.assertIn("BTC-USD", gh.SYMBOLS.split(","))
        self.assertTrue(all(re.fullmatch(r"[A-Z0-9]{2,20}-[A-Z0-9]{2,20}", x) for x in gh.SYMBOLS.split(",")))

    def test_only_dock_file_persists_across_sessions_and_reset_keeps_session_data(self):
        first = gh.gui_argv("/bin/gui", os.path.join(gh.SESSIONS_DIR, "one"), 17130)
        second = gh.gui_argv("/bin/gui", os.path.join(gh.SESSIONS_DIR, "two"), 17131)
        self.assertNotEqual(first[2], second[2])
        self.assertEqual(first[4], second[4])
        self.assertEqual(os.path.basename(first[4]), "docks.ini")
        os.makedirs(os.path.dirname(first[4]))
        with open(first[4], "w") as f:
            f.write("dock=heatmap")
        self.assertEqual(gh.prepare_profile(), first[4])
        self.assertTrue(os.path.isfile(first[4]))
        first_settings = os.path.join(first[2], "settings")
        second_settings = os.path.join(second[2], "settings")
        self.assertNotEqual(first_settings, second_settings)
        os.makedirs(first_settings)
        with open(os.path.join(first_settings, "Sentinel.ini"), "w") as f:
            f.write("heatmap=changed")
        self.assertFalse(os.path.exists(second_settings))
        self.assertEqual(gh.prepare_profile(fresh=True), first[4])  # launch --fresh-profile
        self.assertFalse(os.path.exists(first[4]))
        with open(first[4], "w") as f:
            f.write("dock=watchlist")
        shot = os.path.join(first[2], "screenshots")
        os.makedirs(shot)
        self.assertEqual(gh.reset_profile(), {"ok": True, "profileReset": True})
        self.assertFalse(os.path.exists(first[4]))
        self.assertTrue(os.path.exists(os.path.join(first_settings, "Sentinel.ini")))
        self.assertTrue(os.path.isdir(shot))

    def test_profile_refuses_forbidden_roots_and_symlink_escape(self):
        original = gh.SESSIONS_DIR
        try:
            gh.SESSIONS_DIR = os.path.join(self.repo, "sessions")
            self.assertRefused("unsafe_profile", gh.profile_dir)
            gh.SESSIONS_DIR = "/Volumes/T7/agent-profile"
            self.assertRefused("unsafe_profile", gh.profile_dir)
            gh.SESSIONS_DIR = original
            os.symlink(self.repo, os.path.join(original, "profile"))
            self.assertRefused("unsafe_profile", gh.profile_dir)
        finally:
            gh.SESSIONS_DIR = original

    # ---- screenshots
    def test_shot_refuses_screen_grabs_and_bad_names(self):
        for target in ("main", "lab", "aiCommentary", "screen", "heatmap/../x", "settings:", "settings:Nope", "heatmap\n", "MAIN"):
            self.assertRefused("bad_target", gh.shot, {"name": "a", "target": target})
        for target in ("window", "heatmap", "orderBook", "watchlist", "screener", "stockChart", "paperTrading",
                       "sec", "copenet", "telemetry", "statusBar", "toolbar", "chartmenu",
                       "settings", "settings:TPO"):
            self.assertIsNotNone(gh.SHOT_TARGETS.fullmatch(target), target)
        for name in ("", "../x", "a/b", "x" * 65, "a b"):
            self.assertRefused("bad_name", gh.shot, {"name": name})

    def test_hidden_dock_shot_explains_how_to_show_it(self):
        class Running:
            def poll(self):
                return None
        active = {"proc": Running(), "lastUsed": 0, "lastShot": time.time() - 5,
                  "port": 17130, "shotDir": self.tmp.name, "renderer": "gpu"}
        with patch.object(gh, "session", active), patch.object(gh, "gui_get", return_value=(
                500, {"ok": False, "error": {"message": "orderBook_not_visible"}})):
            with self.assertRaises(gh.HostError) as cm:
                gh.shot({"name": "dock", "target": "orderBook"})
            self.assertEqual(cm.exception.code, "dock_not_visible")
            self.assertIn("focus", str(cm.exception))

    # ---- stale GUI after a host SIGKILL
    def test_cleanup_ends_a_stale_agent_host_gui_but_not_a_recycled_pid(self):
        stale = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)", "sentinel-gui", "--agent-host", "x"],
                                 start_new_session=True)
        other = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"], start_new_session=True)
        try:
            actual_run = gh.subprocess.run
            def fake_ps(argv, **kwargs):
                if argv[:3] == ["ps", "-o", "command="]:
                    pid = int(argv[-1])
                    return subprocess.CompletedProcess(argv, 0,
                        "sentinel-gui --agent-host x" if pid == stale.pid else "unrelated app")
                return actual_run(argv, **kwargs)
            with patch.object(gh.subprocess, "run", side_effect=fake_ps):
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
