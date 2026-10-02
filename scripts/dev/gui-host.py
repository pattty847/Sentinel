#!/usr/bin/env python3
"""gui-host.py: launch a sentinel-gui for sandboxed agents and hand back screenshots.

A sandboxed Codex run cannot start the GUI (no window server, no Metal) but it can reach
localhost and read files. Run this once OUTSIDE any agent sandbox, in a login session with
the screen unlocked; agents then call it through scripts/dev/gui-shot.sh.

    nohup scripts/dev/gui-host.py >/dev/null 2>&1 & disown      # port 17190 (GUI_HOST_PORT)

It dies with its session like any background task (sentinel-server-lifecycle); agents get a
clear "host not running" error and the orchestrator restarts it.

API (JSON; every POST needs the header `X-Gui-Host: 1`, which a browser page cannot send
cross-origin without a preflight this server never answers; the Host header must be loopback):
    GET  /status                      the live session or null
    POST /launch {worktree, renderer?:"gpu"|"legacy", replace?:bool}
    POST /shot   {name, afterOperation?, target?:"heatmap", settle?:bool}
    POST /stop

Rules it keeps (AGENTS.md sections 4a/4b):
  - It never starts a server and refuses when the recorder (:8080) is down.
  - It refuses while the owner's GUI is up (:17100 / :17200): both would write the same
    QSettings domain, so the before/after settings check would prove nothing.
  - It runs only <worktree>/build/mac-clang/apps/sentinel-gui/sentinel-gui for a worktree
    that is the main checkout or a direct child of a known worktree root (real paths), with
    --no-screener and a fixed argument list. One session at a time (16 GB Mac).
  - Screenshots are widget/chart grabs only: target=main is refused (FM-120).
  - The session is ended by SIGTERM (no closeEvent, so no _last_session layout write) and by
    an idle timeout (GUI_HOST_TTL_S, default 1800). The owner's QSettings plist is compared
    before and after and the result is reported.
"""
import glob
import json
import os
import re
import secrets
import signal
import socket
import subprocess
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
# The main checkout (not the worktree this copy lives in), as scripts/dev/agent-worktree.sh finds it.
REPO = os.path.dirname(os.path.realpath(subprocess.run(
    ["git", "-C", HERE, "rev-parse", "--path-format=absolute", "--git-common-dir"],
    capture_output=True, text=True, check=True).stdout.strip()))
WORKTREE_ROOTS = [os.path.realpath(p) for p in (
    os.environ.get("SENTINEL_WORKTREE_ROOT", "/Volumes/T7/sentinel-worktrees"),
    os.path.join(REPO, ".claude", "worktrees"))]
HOST_PORT = int(os.environ.get("GUI_HOST_PORT", "17190"))
TTL_S = int(os.environ.get("GUI_HOST_TTL_S", "1800"))
SERVER_PORT = int(os.environ.get("GUI_HOST_SERVER_PORT", "8080"))
API_PORTS = range(17130, 17170)
OWNER_PORTS = (17100, 17200)
PLIST_DOMAIN = "com.sentinel.SentinelTerminal"
GUI_REL = os.path.join("build", "mac-clang", "apps", "sentinel-gui", "sentinel-gui")
LOG_DIR = os.path.expanduser("~/Library/Logs/Sentinel/gui-host")
# target=main grabs screen pixels and can capture other apps' windows (FM-120): not offered.
SHOT_TARGETS = re.compile(r"^(heatmap|telemetry|toolbar|settings(:[A-Za-z]+)?)$")
NAME_RE = re.compile(r"^[A-Za-z0-9_.-]{1,64}$")

lock = threading.Lock()
session = None  # dict while a GUI is live


class HostError(Exception):
    def __init__(self, status, code, message, **extra):
        super().__init__(message)
        self.status, self.code, self.extra = status, code, extra


def listening(port):
    with socket.socket() as s:
        s.settimeout(0.5)
        return s.connect_ex(("127.0.0.1", port)) == 0


def screen_locked():
    out = subprocess.run(["ioreg", "-n", "Root", "-d1", "-a"], capture_output=True, text=True).stdout
    m = re.search(r"ScreenIsLocked</key>\s*<(true|false)/>", out)
    return bool(m and m.group(1) == "true")


def settings_dump():
    r = subprocess.run(["defaults", "export", PLIST_DOMAIN, "-"], capture_output=True)
    return r.stdout


def resolve_worktree(raw):
    wt = os.path.realpath(raw)
    if wt != REPO and os.path.dirname(wt) not in WORKTREE_ROOTS:
        raise HostError(400, "bad_worktree", f"{wt} is not the main checkout or a child of {WORKTREE_ROOTS}")
    gui = os.path.realpath(os.path.join(wt, GUI_REL))
    if not gui.startswith(wt + os.sep) or not os.access(gui, os.X_OK):
        raise HostError(400, "no_binary", f"no executable {GUI_REL} in {wt} (build it: cmake --build --preset mac-clang)")
    return wt, gui


def gui_get(port, path, timeout=10):
    try:
        with urllib.request.urlopen(f"http://127.0.0.1:{port}{path}", timeout=timeout) as r:
            return r.status, json.loads(r.read() or b"{}")
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read() or b"{}")
        except ValueError:
            return e.code, {}
    except (urllib.error.URLError, OSError, ValueError):
        return 0, {}


def public(s):
    if not s:
        return None
    return {k: s[k] for k in ("id", "pid", "port", "worktree", "renderer", "shotDir", "log", "runLog", "startedAt")}


def stop_session(reason):
    """Caller holds the lock."""
    global session
    s, session = session, None
    if not s:
        return None
    proc = s["proc"]
    if proc.poll() is None:
        try:
            os.killpg(proc.pid, signal.SIGTERM)  # the GUI is its own group leader (start_new_session)
        except ProcessLookupError:
            pass
        try:
            proc.wait(3)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            proc.wait()
    unchanged = settings_dump() == s["settingsBefore"]
    print(f"[gui-host] stopped {s['id']} pid {proc.pid} ({reason}); owner settings "
          f"{'UNCHANGED' if unchanged else 'CHANGED'}", flush=True)
    return {"id": s["id"], "reason": reason, "ownerSettingsUnchanged": unchanged}


def launch(body):
    global session
    renderer = body.get("renderer", "gpu")
    if renderer not in ("gpu", "legacy"):
        raise HostError(400, "bad_renderer", "renderer must be gpu or legacy")
    wt, gui = resolve_worktree(str(body.get("worktree", "")))
    with lock:
        if session and session["proc"].poll() is not None:
            stop_session("exited")
        if session:
            if not body.get("replace"):
                raise HostError(409, "busy", "a GUI session is already running (send replace:true or POST /stop)",
                                session=public(session))
            stop_session("replaced")
        if not listening(SERVER_PORT):
            raise HostError(412, "no_recorder", f"no server on :{SERVER_PORT}; the recorder must be up (never started here)")
        for p in OWNER_PORTS:
            if listening(p):
                raise HostError(412, "owner_gui_up", f"port {p} is in use: the owner's GUI is running (shared settings)")
        port = next((p for p in API_PORTS if not listening(p)), None)
        if port is None:
            raise HostError(503, "no_port", "no free API port in 17130-17169")
        # Worktrees lack the gitignored certs the client trusts; link the main checkout's.
        for f in ("sentinel-server.crt", "sentinel-server.key"):
            src, dst = os.path.join(REPO, "certs", f), os.path.join(wt, "certs", f)
            if os.path.exists(src) and not os.path.exists(dst):
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                os.symlink(src, dst)
        os.makedirs(LOG_DIR, exist_ok=True)
        sid = secrets.token_hex(4)
        log = os.path.join(LOG_DIR, f"{sid}.out")
        before = settings_dump()
        with open(log, "wb") as out:
            proc = subprocess.Popen(
                [gui, "--heatmap-renderer", renderer, "--api-port", str(port), "--no-screener"],
                cwd=wt, stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, start_new_session=True)
        session = dict(id=sid, pid=proc.pid, port=port, worktree=wt, renderer=renderer, proc=proc, log=log,
                       shotDir=os.path.join(wt, "screenshots"), runLog=None, settingsBefore=before,
                       startedAt=int(time.time()), lastUsed=time.time(), lastShot=0.0)
        s = session
    t0 = time.time()
    while time.time() - t0 < 60:  # API up
        if s["proc"].poll() is not None:
            with lock:
                if session is s:
                    stop_session("exited early")
            tail = open(log, "rb").read()[-1500:].decode("utf-8", "replace")
            raise HostError(500, "gui_exited", "sentinel-gui exited during startup", log=log, tail=tail)
        if gui_get(port, "/api/v1/state", 2)[0] == 200:
            break
        time.sleep(0.2)
    else:
        with lock:
            if session is s:
                stop_session("api timeout")
        raise HostError(504, "api_timeout", "the GUI API did not come up in 60 s", log=log)
    settled = None
    if renderer == "gpu":  # first heatmap picture drawn
        settled = wait_settled(port, 30)
    found = sorted(glob.glob(os.path.expanduser(f"~/Library/Logs/Sentinel/sentinel-gui-*-{s['pid']}.log")))
    s["runLog"] = found[-1] if found else None
    out = public(s)
    out.update(ready=True, settled=settled, screenLocked=screen_locked(),
               coldStartMs=int((time.time() - t0) * 1000))
    print(f"[gui-host] launched {s['id']} pid {s['pid']} api :{port} {renderer} from {wt}", flush=True)
    return out


def wait_settled(port, timeout):
    t0 = time.time()
    while time.time() - t0 < timeout:
        _, r = gui_get(port, "/api/v1/heatmap/state", 3)
        if r.get("data", {}).get("settled") is True:
            return True
        time.sleep(0.2)
    return False


def shot(body):
    name = str(body.get("name", ""))
    target = str(body.get("target", "heatmap"))
    if not NAME_RE.match(name):
        raise HostError(400, "bad_name", "name must match [A-Za-z0-9_.-]{1,64}")
    if not SHOT_TARGETS.match(target):
        raise HostError(400, "bad_target", "target must be heatmap, telemetry, toolbar or settings[:Tab] (target=main is refused)")
    with lock:
        s = session
        if not s or s["proc"].poll() is not None:
            raise HostError(409, "no_session", "no running GUI session (POST /launch first)")
        s["lastUsed"] = time.time()
        wait = 1.15 - (time.time() - s["lastShot"])  # the API allows one screenshot per second
        port, wt, renderer = s["port"], s["worktree"], s["renderer"]
        if wait > 0:
            time.sleep(wait)
        s["lastShot"] = time.time()
    if body.get("settle") and renderer == "gpu":
        wait_settled(port, 30)
    q = {"name": name, "target": target}
    if body.get("afterOperation"):
        q.update(afterOperation=str(body["afterOperation"]), waitMs="5000")
    status, r = gui_get(port, "/api/v1/screenshot?" + urllib.parse.urlencode(q), 15)
    if status == 429:  # raced another caller's shot
        time.sleep(1.2)
        status, r = gui_get(port, "/api/v1/screenshot?" + urllib.parse.urlencode(q), 15)
    if status != 200 or not r.get("ok"):
        raise HostError(502, "shot_failed", f"screenshot failed (HTTP {status})", gui=r,
                        screenLocked=screen_locked())
    path = os.path.join(wt, r["path"].lstrip("./")) if r.get("path") else None
    if not path or not os.path.exists(path):
        raise HostError(502, "shot_missing", "the GUI reported success but wrote no file", gui=r)
    r["path"] = path
    return r


class Handler(BaseHTTPRequestHandler):
    server_version = "gui-host/1"

    def log_message(self, fmt, *args):
        pass

    def reply(self, status, obj):
        data = json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def guard(self):
        host = (self.headers.get("Host") or "").split(":")[0]
        if host not in ("127.0.0.1", "localhost", "[::1]"):  # DNS rebinding
            raise HostError(403, "bad_host", "loopback Host header required")
        if self.command == "POST" and self.headers.get("X-Gui-Host") != "1":
            raise HostError(403, "no_header", "POST needs the header X-Gui-Host: 1")

    def handle_any(self):
        try:
            self.guard()
            n = int(self.headers.get("Content-Length") or 0)
            body = json.loads(self.rfile.read(n) or b"{}") if n else {}
            if self.command == "GET" and self.path == "/status":
                with lock:
                    live = session and session["proc"].poll() is None
                    return self.reply(200, {"ok": True, "session": public(session) if live else None,
                                            "idleTtlS": TTL_S})
            if self.command == "POST" and self.path == "/launch":
                return self.reply(200, {"ok": True, "session": launch(body)})
            if self.command == "POST" and self.path == "/shot":
                return self.reply(200, shot(body))
            if self.command == "POST" and self.path == "/stop":
                with lock:
                    return self.reply(200, {"ok": True, "stopped": stop_session("requested")})
            raise HostError(404, "not_found", "unknown route")
        except HostError as e:
            self.reply(e.status, {"ok": False, "error": {"code": e.code, "message": str(e), **e.extra}})
        except (ValueError, TypeError) as e:
            self.reply(400, {"ok": False, "error": {"code": "bad_request", "message": str(e)}})
        except Exception as e:  # never kill the host over one request
            self.reply(500, {"ok": False, "error": {"code": "internal", "message": repr(e)}})

    do_GET = do_POST = handle_any


def reaper():
    while True:
        time.sleep(10)
        with lock:
            if session and (session["proc"].poll() is not None):
                stop_session("exited")
            elif session and time.time() - session["lastUsed"] > TTL_S:
                stop_session("idle timeout")


def main():
    def bye(*_):
        with lock:
            stop_session("host exit")
        os._exit(0)
    signal.signal(signal.SIGTERM, bye)
    signal.signal(signal.SIGINT, bye)
    threading.Thread(target=reaper, daemon=True).start()
    print(f"[gui-host] http://127.0.0.1:{HOST_PORT} repo={REPO} roots={WORKTREE_ROOTS}", flush=True)
    ThreadingHTTPServer(("127.0.0.1", HOST_PORT), Handler).serve_forever()


if __name__ == "__main__":
    main()
