#!/usr/bin/env python3
"""gui-host.py: run a sentinel-gui for sandboxed agents and hand back screenshots.

A sandboxed Codex run cannot start the GUI (no window server, no Metal) but it can reach
localhost and read files. This host runs OUTSIDE any sandbox, in a login session with the screen
unlocked; agents call it through scripts/dev/gui-shot.sh.

TRUST MODEL. Everything an agent can write (its worktree, its build) is untrusted, and the host
executes code with the owner's privileges. So the host never runs a path the agent names:
  - `main`    the main checkout's build (agents cannot write it), or
  - `<id>`    a copy the ORCHESTRATOR blessed after a cross-vendor review of that branch:
              `gui-host.py bless <worktree>` copies the binary into a read-only directory under
              ~/Sentinel-runtime/gui-host/blessed/ (outside every agent's writable roots) and
              records its sha256; launch re-checks the hash and the permissions each time.
Blessed means reviewed, not trusted. The host never builds (CMake runs code). The GUI is started
with --agent-host (AgentHostMode.hpp), so it refuses screen-pixel screenshots, keeps screenshots
and settings in its session directory, and loads QML only from its embedded resources. A binary
that lacks that flag is refused.

    scripts/dev/gui-host.py                 serve on 127.0.0.1:17190 (GUI_HOST_PORT)
    scripts/dev/gui-host.py bless <worktree>    orchestrator only, after review
    scripts/dev/gui-host.py blessed | unbless <id>

Start it detached: `nohup scripts/dev/gui-host.py >/dev/null 2>&1 & disown`. nohup keeps it alive
after the session that started it ends, so it must be stopped on purpose (`pkill -TERM -f
gui-host.py`; SIGTERM ends the GUI too). If it is SIGKILLed, the GUI it started stays up until the
next host start, which ends it (pidfile). The GUI also has an idle timeout in the host
(GUI_HOST_TTL_S, default 1800).

API (JSON; every POST needs the header `X-Gui-Host: 1`, which a browser page cannot send
cross-origin without a preflight this server never answers; the Host header must be loopback):
    GET  /status      the live session or null
    GET  /binaries    what launch accepts: main + blessed ids
    POST /launch {binary?:"main"|<id>, renderer?:"gpu"|"legacy", replace?:bool}
    POST /shot   {name, afterOperation?, target?:"heatmap"|"lab"|"telemetry"|"toolbar"|"settings[:Tab]", settle?:bool}
    POST /stop

Other rules (AGENTS.md 4a/4b): never starts a server (refuses when the recorder :8080 is down),
one session at a time (16 GB Mac), SIGTERM stop (no closeEvent, so no _last_session layout write),
and the owner's real QSettings plist is compared before and after as a check.
"""
import glob
import hashlib
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
WORKTREE_ROOTS = [os.path.realpath(p) for p in (
    os.environ.get("SENTINEL_WORKTREE_ROOT", "/Volumes/T7/sentinel-worktrees"),
    os.path.join(REPO, ".claude", "worktrees"))]
RUNTIME_DIR = os.path.expanduser(os.environ.get("GUI_HOST_RUNTIME", "~/Sentinel-runtime/gui-host"))
BLESSED_DIR = os.path.join(RUNTIME_DIR, "blessed")
REGISTRY = os.path.join(RUNTIME_DIR, "registry.json")
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
ID_RE = re.compile(r"^[0-9a-f]{12}$")
SHOT_TARGETS = re.compile(r"^(heatmap|lab|telemetry|toolbar|settings(:[A-Za-z]+)?)$")
NAME_RE = re.compile(r"^[A-Za-z0-9_.-]{1,64}$")
SCRUB_ENV = ("SENTINEL_QML_PATH", "SENTINEL_GUI_SCREENSHOT_DIR", "QML_IMPORT_PATH", "QML2_IMPORT_PATH")

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


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def has_flag(path):
    with open(path, "rb") as f:
        return FLAG in f.read()


def check_file_safe(path):
    """Executable, ours, and writable by nobody but (possibly) the owner of this account."""
    st = os.stat(path)
    if not stat.S_ISREG(st.st_mode) or not os.access(path, os.X_OK):
        raise HostError(412, "not_executable", f"{path} is not an executable file")
    if st.st_uid != os.getuid():
        raise HostError(412, "bad_owner", f"{path} is not owned by this user")
    if st.st_mode & (stat.S_IWGRP | stat.S_IWOTH):
        raise HostError(412, "writable_binary", f"{path} is group/world writable")


def worktree_binary(raw):
    wt = os.path.realpath(raw)
    if wt != REPO and os.path.dirname(wt) not in WORKTREE_ROOTS:
        raise HostError(400, "bad_worktree", f"{wt} is not the main checkout or a child of {WORKTREE_ROOTS}")
    gui = os.path.realpath(os.path.join(wt, GUI_REL))
    if not gui.startswith(wt + os.sep) or not os.path.isfile(gui) or not os.access(gui, os.X_OK):
        raise HostError(400, "no_binary", f"no executable {GUI_REL} in {wt} (build it: cmake --build --preset mac-clang)")
    return wt, gui


def load_registry():
    try:
        with open(REGISTRY) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def save_registry(reg):
    os.makedirs(RUNTIME_DIR, exist_ok=True)
    tmp = REGISTRY + ".tmp"
    with open(tmp, "w") as f:
        json.dump(reg, f, indent=1, sort_keys=True)
    os.replace(tmp, REGISTRY)


# ------------------------------------------------------------------ trust: which binary may run
def resolve_binary(which):
    """The ONLY way a launch picks an executable. `which` is "main" or a blessed id, never a path."""
    which = str(which or "main")
    if which == "main":
        path = os.path.realpath(os.path.join(REPO, GUI_REL))
        if not os.path.isfile(path):
            raise HostError(412, "no_main_binary", f"no main-checkout build at {GUI_REL}")
        check_file_safe(path)
        if not has_flag(path):
            raise HostError(412, "no_agent_host_flag",
                            "the main-checkout build predates --agent-host (rebuild main after the gui-host branch lands)")
        return path, "main"
    if not ID_RE.match(which):
        raise HostError(400, "bad_binary", 'binary must be "main" or a blessed id (see GET /binaries)')
    entry = load_registry().get(which)
    if not entry:
        raise HostError(404, "unknown_binary", f"{which} is not blessed (the orchestrator runs `gui-host.py bless <worktree>`)")
    path = os.path.join(BLESSED_DIR, which, "sentinel-gui")
    if os.path.realpath(path) != path or not os.path.isfile(path):
        raise HostError(412, "bad_blessed_path", f"blessed binary {which} is missing or a link")
    check_file_safe(path)
    if os.stat(path).st_mode & stat.S_IWUSR:
        raise HostError(412, "writable_binary", f"blessed binary {which} is writable (must be read-only)")
    if sha256_file(path) != entry["sha256"]:
        raise HostError(412, "hash_mismatch", f"blessed binary {which} no longer matches its recorded sha256")
    if not has_flag(path):
        raise HostError(412, "no_agent_host_flag", f"blessed binary {which} lacks --agent-host")
    return path, which


def bless(worktree):
    wt, gui = worktree_binary(worktree)
    if not has_flag(gui):
        raise HostError(412, "no_agent_host_flag", f"{gui} lacks --agent-host (rebase onto the gui-host branch and rebuild)")
    digest = sha256_file(gui)
    bid = digest[:12]
    dest_dir = os.path.join(BLESSED_DIR, bid)
    dest = os.path.join(dest_dir, "sentinel-gui")
    os.makedirs(BLESSED_DIR, exist_ok=True)
    if os.path.isdir(dest_dir):
        os.chmod(dest_dir, 0o755)
        if os.path.exists(dest):
            os.chmod(dest, 0o755)
    os.makedirs(dest_dir, exist_ok=True)
    shutil.copyfile(gui, dest)
    os.chmod(dest, 0o555)          # read-only file
    os.chmod(dest_dir, 0o555)      # and a read-only directory: no rename/replace
    if sha256_file(dest) != digest:
        raise HostError(500, "copy_mismatch", "blessed copy differs from the source binary")

    def git(*a):
        return subprocess.run(["git", "-C", wt, *a], capture_output=True, text=True).stdout.strip()
    reg = load_registry()
    reg[bid] = dict(sha256=digest, worktree=wt, branch=git("rev-parse", "--abbrev-ref", "HEAD"),
                    commit=git("rev-parse", "--short", "HEAD"), dirty=bool(git("status", "--porcelain")),
                    blessedAt=int(time.time()))
    save_registry(reg)
    return bid, reg[bid]


def unbless(bid):
    if not ID_RE.match(bid):
        raise HostError(400, "bad_binary", "not a blessed id")
    reg = load_registry()
    d = os.path.join(BLESSED_DIR, bid)
    if os.path.isdir(d):
        os.chmod(d, 0o755)
        shutil.rmtree(d)
    reg.pop(bid, None)
    save_registry(reg)


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
    return {k: s[k] for k in ("id", "pid", "port", "binary", "renderer", "shotDir", "log", "runLog", "startedAt")}


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
    if "worktree" in body:
        raise HostError(400, "no_worktree_launch", "launch takes binary=main|<blessed id>, never a worktree or path")
    binary, label = resolve_binary(body.get("binary", "main"))
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
        env = {k: v for k, v in os.environ.items() if k not in SCRUB_ENV}
        before = settings_dump()
        with open(log, "wb") as out:
            proc = subprocess.Popen(
                [binary, "--agent-host", sdir, "--heatmap-renderer", renderer, "--api-port", str(port), "--no-screener"],
                cwd=REPO, env=env, stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, start_new_session=True)
        session = dict(id=sid, pid=proc.pid, port=port, binary=label, renderer=renderer, proc=proc, log=log,
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
    if s["runLog"] and b"QML failed to load" in open(s["runLog"], "rb").read():
        with lock:  # --agent-host loads only embedded QML; a chart that cannot load is a failed launch
            if session is s:
                stop_session("qml failed")
        raise HostError(500, "qml_failed", "the GUI could not load its embedded QML (see runLog)", runLog=s["runLog"])
    out = public(s)
    out.update(ready=True, settled=settled, screenLocked=screen_locked(), coldStartMs=int((time.time() - t0) * 1000))
    print(f"[gui-host] launched {s['id']} pid {s['pid']} api :{port} {renderer} binary={label}", flush=True)
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


def binaries():
    out = []
    for bid, e in sorted(load_registry().items()):
        out.append(dict(id=bid, branch=e.get("branch"), commit=e.get("commit"), dirty=e.get("dirty"),
                        blessedAt=e.get("blessedAt")))
    try:
        main_ok, main_why = bool(resolve_binary("main")), None
    except HostError as e:
        main_ok, main_why = False, str(e)
    return {"main": {"usable": main_ok, "why": main_why}, "blessed": out}


# ------------------------------------------------------------------ HTTP
class Handler(BaseHTTPRequestHandler):
    server_version = "gui-host/2"

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
            if self.command == "GET" and self.path == "/binaries":
                return self.reply(200, {"ok": True, **binaries()})
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
    print(f"[gui-host] http://127.0.0.1:{HOST_PORT} repo={REPO} roots={WORKTREE_ROOTS} blessed={RUNTIME_DIR}", flush=True)
    ThreadingHTTPServer(("127.0.0.1", HOST_PORT), Handler).serve_forever()


def main(argv):
    cmd = argv[1] if len(argv) > 1 else "serve"
    try:
        if cmd == "serve":
            serve()
        elif cmd == "bless" and len(argv) == 3:
            bid, entry = bless(argv[2])
            print(f"blessed {bid}  {entry['branch']}@{entry['commit']}{' (DIRTY worktree)' if entry['dirty'] else ''}")
            print("Blessed means reviewed. Bless only after the branch's cross-vendor review of the exact commit.")
        elif cmd == "blessed":
            print(json.dumps(binaries(), indent=1))
        elif cmd == "unbless" and len(argv) == 3:
            unbless(argv[2])
            print(f"unblessed {argv[2]}")
        else:
            print(__doc__.split("API (JSON")[0])
            return 2
    except HostError as e:
        print(f"gui-host: {e.code}: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
