#!/usr/bin/env python3
"""gui-host.py: run the main checkout's sentinel-gui for sandboxed agents and hand back screenshots.

A sandboxed Codex run cannot start the GUI (no window server, no Metal) but it can reach
localhost and read files. This host runs OUTSIDE any sandbox, in a login session with the screen
unlocked; agents call it through scripts/dev/gui-shot.sh.

TRUST MODEL. The host executes code with the owner's privileges, and an agent can write its own
worktree and build output. So the host runs exactly one thing: the main checkout's build
(build/mac-clang/apps/sentinel-gui/sentinel-gui), which agents cannot write and the orchestrator
builds from landed main. It never runs a path an agent names, never runs a worktree build, and never
builds (CMake runs code). A branch's own change is therefore not visible through this host until it
has landed and main is rebuilt; Claude subagents can run their own GUI for branch visuals.
Second-review note (2026-10-02): a per-branch "bless the worktree build" design was tried and
dropped, because a blessed copy is not bound to the reviewed source. The safe form is the
orchestrator building the reviewed commit in a checkout agents cannot write; not built.

The GUI is started with --agent-host (AgentHostMode.hpp): it refuses screen-pixel screenshots,
keeps screenshots per session and settings in a persistent host profile, sends no trade commands, and switches only
to allowlisted symbols. A build without that flag is refused (a stale main must not run uncontained).
The child gets a minimal environment (no DYLD_*, QT_*, QML_* from this process's env).

    nohup scripts/dev/gui-host.py >/dev/null 2>&1 & disown     # 127.0.0.1:17190 (GUI_HOST_PORT)

nohup keeps it alive after the session that started it ends, so stop it on purpose (`pkill -TERM
-f gui-host.py`; SIGTERM ends the GUI too). If it is SIGKILLed, its GUI stays up until the next host
start, which ends it (pidfile). Idle timeout GUI_HOST_TTL_S (default 1800) is enforced BY the host, so
it does not bound an orphan: the GUI has no parent-liveness lease, and a host that is SIGKILLed and
never restarted leaves one GUI running until someone ends it (`pkill -f 'sentinel-gui.*--agent-host'`).

API (JSON; every POST needs the header `X-Gui-Host: 1`, which a browser page cannot send
cross-origin without a preflight this server never answers; the Host header must be loopback):
    GET  /status      the live session or null, and whether main is launchable
    POST /launch {renderer?:"gpu"|"legacy", replace?:bool, freshProfile?:bool}
    POST /shot   {name, afterOperation?, target?:"heatmap"|"lab"|"telemetry"|"toolbar"|"settings[:Tab]", settle?:bool}
    POST /stop
    POST /profile-reset    clears the persistent agent settings while no GUI is running

Other rules (AGENTS.md 4a/4b): never starts a server (refuses when the recorder :8080 is down), one
session at a time (16 GB Mac), SIGTERM stop (no closeEvent, so no _last_session layout write), and
the owner's real QSettings plist is compared before and after as a check.
"""
import glob
import json
import os
import re
import secrets
import shutil
import signal
import socket
import stat
import subprocess
import sys
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
SESSIONS_DIR = os.path.expanduser(os.environ.get("GUI_HOST_SESSIONS", "~/Library/Logs/Sentinel/gui-host"))
PIDFILE = os.path.join(SESSIONS_DIR, "session.json")
HOST_PORT = int(os.environ.get("GUI_HOST_PORT", "17190"))
TTL_S = int(os.environ.get("GUI_HOST_TTL_S", "1800"))
SERVER_PORT = int(os.environ.get("GUI_HOST_SERVER_PORT", "8080"))
API_PORTS = range(17130, 17170)
OWNER_PORTS = (17100, 17200)
PLIST_DOMAIN = "com.sentinel.SentinelTerminal"
GUI_REL = os.path.join("build", "mac-clang", "apps", "sentinel-gui", "sentinel-gui")
FLAG = b"--agent-host"
# The products the recorder already captures; a GUI symbol change subscribes the recorder upstream.
SYMBOLS = os.environ.get("GUI_HOST_SYMBOLS", "BTC-USD,ETH-USD,SOL-USD,FARTCOIN-USD,PEPE-USD,DOGE-USD,AVAX-USD")
SHOT_TARGETS = re.compile(r"^(heatmap|lab|telemetry|toolbar|settings(:[A-Za-z]+)?)$")
NAME_RE = re.compile(r"^[A-Za-z0-9_.-]{1,64}$")
# Environment the GUI child may inherit. Nothing else (no DYLD_*, QT_*, QSG_*, QML*, SENTINEL_*).
ENV_ALLOW = ("HOME", "USER", "LOGNAME", "TMPDIR", "LANG", "LC_ALL", "__CF_USER_TEXT_ENCODING")
SAFE_PATH = "/usr/bin:/bin:/usr/sbin:/sbin"

lock = threading.Lock()
session = None  # dict while a GUI is live


class HostError(Exception):
    def __init__(self, status, code, message, **extra):
        super().__init__(message)
        self.status, self.code, self.extra = status, code, extra


# ------------------------------------------------------------------ small helpers
def listening(port):
    with socket.socket() as s:
        s.settimeout(0.5)
        return s.connect_ex(("127.0.0.1", port)) == 0


def screen_locked():
    out = subprocess.run(["ioreg", "-n", "Root", "-d1", "-a"], capture_output=True, text=True).stdout
    m = re.search(r"ScreenIsLocked</key>\s*<(true|false)/>", out)
    return bool(m and m.group(1) == "true")


def settings_dump():
    return subprocess.run(["defaults", "export", PLIST_DOMAIN, "-"], capture_output=True).stdout


def has_flag(path):
    with open(path, "rb") as f:
        return FLAG in f.read()


def child_env(environ=None):
    environ = os.environ if environ is None else environ
    env = {k: environ[k] for k in ENV_ALLOW if k in environ}
    env["PATH"] = SAFE_PATH
    return env


def gui_argv(binary, session_dir, renderer, port, profile_settings=None):
    """The only command line the host ever builds: fixed flags, no caller-supplied arguments."""
    return [binary, "--agent-host", session_dir, "--agent-host-profile", profile_settings or profile_dir(),
            "--agent-host-symbols", SYMBOLS,
            "--heatmap-renderer", renderer, "--api-port", str(port), "--no-screener"]


def profile_dir():
    """Fixed path only; reject symlinks and forbidden roots before any write or deletion."""
    base = os.path.realpath(SESSIONS_DIR)
    candidate = os.path.join(SESSIONS_DIR, "profile", "settings")
    resolved = os.path.realpath(candidate)
    forbidden = (os.path.realpath(REPO), os.path.realpath(os.path.join(REPO, "build")), "/Volumes")
    if any(os.path.commonpath((base, root)) == root or os.path.commonpath((resolved, root)) == root
           for root in forbidden):
        raise HostError(412, "unsafe_profile", "agent profile resolves inside a forbidden root")
    if os.path.commonpath((resolved, base)) != base or resolved == base:
        raise HostError(412, "unsafe_profile", "agent profile resolves outside the sessions directory")
    if any(os.path.islink(p) for p in (SESSIONS_DIR, os.path.join(SESSIONS_DIR, "profile"), candidate)):
        raise HostError(412, "unsafe_profile", "agent profile path must not use symlinks")
    return candidate


def reset_profile():
    with lock:
        if session and session["proc"].poll() is None:
            raise HostError(409, "busy", "stop the GUI before resetting its profile")
        settings = profile_dir()
        if os.path.lexists(settings):
            shutil.rmtree(settings)
        os.makedirs(settings, exist_ok=True)
        return {"ok": True, "profileReset": True}


# ------------------------------------------------------------------ trust: which binary may run
def resolve_binary(which="main"):
    """The ONLY executable a launch can pick: the main checkout's build. `which` exists so a request
    that names anything else gets a clear refusal instead of being ignored."""
    if which not in (None, "main"):
        raise HostError(400, "main_only", 'this host runs only the main checkout build (binary must be "main" or omitted)')
    path = os.path.realpath(os.path.join(REPO, GUI_REL))
    if not os.path.isfile(path) or not os.access(path, os.X_OK):
        raise HostError(412, "no_main_binary", f"no main-checkout build at {GUI_REL} (the orchestrator builds landed main)")
    if not path.startswith(os.path.realpath(REPO) + os.sep):  # a symlink must not lead out of the main checkout
        raise HostError(412, "outside_main", f"{path} is not inside the main checkout {REPO}")
    st = os.stat(path)
    if st.st_uid != os.getuid():
        raise HostError(412, "bad_owner", f"{path} is not owned by this user")
    if st.st_mode & (stat.S_IWGRP | stat.S_IWOTH):
        raise HostError(412, "writable_binary", f"{path} is group/world writable")
    if not has_flag(path):
        raise HostError(412, "no_agent_host_flag",
                        "the main-checkout build predates --agent-host (rebuild main after the gui-host branch lands)")
    return path


# ------------------------------------------------------------------ GUI sessions
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
    return {k: s[k] for k in ("id", "pid", "port", "renderer", "shotDir", "log", "runLog", "startedAt")}


def kill_group(pid, wait_s=3.0):
    try:
        os.killpg(pid, signal.SIGTERM)  # the GUI is its own group leader (start_new_session)
    except ProcessLookupError:
        return
    end = time.time() + wait_s
    while time.time() < end:
        try:
            os.killpg(pid, 0)
        except (ProcessLookupError, PermissionError):  # gone, or a zombie not yet reaped (EPERM on macOS)
            return
        time.sleep(0.1)
    try:
        os.killpg(pid, signal.SIGKILL)
    except ProcessLookupError:
        pass


def cleanup_stale():
    """A SIGKILLed host leaves its GUI running; the pidfile finds it at the next host start."""
    try:
        with open(PIDFILE) as f:
            rec = json.load(f)
        os.remove(PIDFILE)
    except (OSError, ValueError):
        return
    pid = int(rec.get("pid", 0))
    if pid <= 1:
        return
    cmd = subprocess.run(["ps", "-o", "command=", "-p", str(pid)], capture_output=True, text=True).stdout
    if "--agent-host" in cmd and "sentinel-gui" in cmd:  # not a recycled pid
        print(f"[gui-host] ending stale GUI pid {pid} from a previous host", flush=True)
        kill_group(pid)


def stop_session(reason):
    """Caller holds the lock."""
    global session
    s, session = session, None
    if not s:
        return None
    proc = s["proc"]
    if proc.poll() is None:
        kill_group(proc.pid)
        try:
            proc.wait(3)
        except subprocess.TimeoutExpired:
            pass
    try:
        os.remove(PIDFILE)
    except OSError:
        pass
    unchanged = settings_dump() == s["settingsBefore"]
    print(f"[gui-host] stopped {s['id']} pid {proc.pid} ({reason}); owner settings "
          f"{'UNCHANGED' if unchanged else 'CHANGED'}", flush=True)
    return {"id": s["id"], "reason": reason, "ownerSettingsUnchanged": unchanged}


def launch(body):
    global session
    renderer = body.get("renderer", "gpu")
    if renderer not in ("gpu", "legacy"):
        raise HostError(400, "bad_renderer", "renderer must be gpu or legacy")
    if "freshProfile" in body and not isinstance(body["freshProfile"], bool):
        raise HostError(400, "bad_profile", "freshProfile must be boolean")
    for forbidden in ("worktree", "path"):
        if forbidden in body:
            raise HostError(400, "main_only", "launch takes no worktree or path: the host runs only the main checkout build")
    binary = resolve_binary(body.get("binary", "main"))
    with lock:
        if session and session["proc"].poll() is not None:
            stop_session("exited")
        if session:
            if not body.get("replace"):
                raise HostError(409, "busy", "a GUI session is already running (send replace:true or POST /stop)",
                                session=public(session))
            stop_session("replaced")
        profile_settings = profile_dir()
        if body.get("freshProfile"):
            if os.path.lexists(profile_settings):
                shutil.rmtree(profile_settings)
        os.makedirs(profile_settings, exist_ok=True)
        if not listening(SERVER_PORT):
            raise HostError(412, "no_recorder", f"no server on :{SERVER_PORT}; the recorder must be up (never started here)")
        for p in OWNER_PORTS:  # courtesy: settings are isolated, but two GUIs on one screen are confusing
            if listening(p):
                raise HostError(412, "owner_gui_up", f"port {p} is in use: the owner's GUI is running")
        port = next((p for p in API_PORTS if not listening(p)), None)
        if port is None:
            raise HostError(503, "no_port", "no free API port in 17130-17169")
        sid = secrets.token_hex(4)
        sdir = os.path.join(SESSIONS_DIR, sid)
        os.makedirs(sdir)
        log = os.path.join(sdir, "gui.out")
        before = settings_dump()
        with open(log, "wb") as out:
            proc = subprocess.Popen(gui_argv(binary, sdir, renderer, port, profile_settings), cwd=REPO, env=child_env(),
                                    stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                    start_new_session=True)
        session = dict(id=sid, pid=proc.pid, port=port, renderer=renderer, proc=proc, log=log,
                       shotDir=os.path.join(sdir, "screenshots"), runLog=None, settingsBefore=before,
                       startedAt=int(time.time()), lastUsed=time.time(), lastShot=0.0)
        with open(PIDFILE, "w") as f:
            json.dump({"pid": proc.pid, "host": os.getpid(), "id": sid}, f)
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
    settled = wait_settled(port, 30) if renderer == "gpu" else None
    found = sorted(glob.glob(os.path.expanduser(f"~/Library/Logs/Sentinel/sentinel-gui-*-{s['pid']}.log")))
    s["runLog"] = found[-1] if found else None
    out = public(s)
    out.update(ready=True, settled=settled, screenLocked=screen_locked(), coldStartMs=int((time.time() - t0) * 1000))
    print(f"[gui-host] launched {s['id']} pid {s['pid']} api :{port} {renderer}", flush=True)
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
        raise HostError(400, "bad_target", "target must be heatmap, lab, telemetry, toolbar or settings[:Tab] (screen grabs are refused)")
    with lock:
        s = session
        if not s or s["proc"].poll() is not None:
            raise HostError(409, "no_session", "no running GUI session (POST /launch first)")
        s["lastUsed"] = time.time()
        wait = 1.15 - (time.time() - s["lastShot"])  # the API allows one screenshot per second
        port, shot_dir, renderer = s["port"], s["shotDir"], s["renderer"]
        if wait > 0:
            time.sleep(wait)
        s["lastShot"] = time.time()
    if body.get("settle") and renderer == "gpu":
        wait_settled(port, 30)
    q = {"name": name, "target": target}
    if body.get("afterOperation"):
        q.update(afterOperation=str(body["afterOperation"]), waitMs="5000")
    url = "/api/v1/screenshot?" + urllib.parse.urlencode(q)
    status, r = gui_get(port, url, 15)
    if status == 429:  # raced another caller's shot
        time.sleep(1.2)
        status, r = gui_get(port, url, 15)
    if status != 200 or not r.get("ok"):
        raise HostError(502, "shot_failed", f"screenshot failed (HTTP {status})", gui=r, screenLocked=screen_locked())
    path = os.path.join(shot_dir, name if name.lower().endswith(".png") else name + ".png")
    if not os.path.exists(path):  # the GUI writes only to the fixed directory; its reply path is not trusted
        raise HostError(502, "shot_missing", "the GUI reported success but wrote no file", gui=r)
    r["path"] = path
    return r


def main_status():
    try:
        resolve_binary("main")
        return {"usable": True, "why": None}
    except HostError as e:
        return {"usable": False, "why": str(e)}


# ------------------------------------------------------------------ HTTP
class Handler(BaseHTTPRequestHandler):
    server_version = "gui-host/3"

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
                    sess = public(session) if live else None
                return self.reply(200, {"ok": True, "session": sess, "idleTtlS": TTL_S, "main": main_status()})
            if self.command == "POST" and self.path == "/launch":
                return self.reply(200, {"ok": True, "session": launch(body)})
            if self.command == "POST" and self.path == "/shot":
                return self.reply(200, shot(body))
            if self.command == "POST" and self.path == "/stop":
                with lock:
                    return self.reply(200, {"ok": True, "stopped": stop_session("requested")})
            if self.command == "POST" and self.path == "/profile-reset":
                return self.reply(200, reset_profile())
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


def serve():
    def bye(*_):
        with lock:
            stop_session("host exit")
        os._exit(0)
    signal.signal(signal.SIGTERM, bye)
    signal.signal(signal.SIGINT, bye)
    os.makedirs(SESSIONS_DIR, exist_ok=True)
    cleanup_stale()
    threading.Thread(target=reaper, daemon=True).start()
    print(f"[gui-host] http://127.0.0.1:{HOST_PORT} repo={REPO} main={main_status()}", flush=True)
    ThreadingHTTPServer(("127.0.0.1", HOST_PORT), Handler).serve_forever()


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] != "serve":
        print(__doc__.split("API (JSON")[0])
        sys.exit(2)
    serve()
