#!/usr/bin/env python3
"""can-lab: runs Franke CAN test scripts on a Raspberry Pi and exposes them as a remote MCP server.

Everything that touches the bus is a "job": one python script run through
`franke_can.py socket://<ip>:3333 script <file> key=value ...`. Only one job runs at a time,
further jobs wait in a queue. Jobs survive client disconnects; their output is kept on disk.

Endpoints (all below the secret prefix /<SECRET>/):
  mcp                 MCP server (streamable HTTP)  -> add https://<host>/<SECRET>/mcp as connector
  ""  (index page)    status, jobs, logs, emergency stop button
  logs/<file>         download a log / csv / markers file
  announce?ip=...     the ESP32 can call this after connecting to Wi-Fi (POST or GET)
  stop                POST: emergency stop

Lives in the CAN-tool repo (lab/). Runtime data is kept OUTSIDE the repo in CANLAB_HOME
(default ~/can-lab-data): config.json (secret!), scripts/, data/ (job history), .venv/.
Logs are written by franke_can.py to <repo>/Messungen (git-ignored).
Config: environment variables CANLAB_<KEY> or CANLAB_HOME/config.json (see README.md).
"""
from __future__ import annotations

import collections
import contextlib
import datetime as dt
import html
import json
import os
import re
import secrets
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import Any, Optional

from mcp.server.fastmcp import FastMCP
from starlette.requests import Request
from starlette.responses import FileResponse, HTMLResponse, JSONResponse, PlainTextResponse, RedirectResponse

BASE = Path(__file__).resolve().parent                       # <repo>/lab
HOME_DIR = Path(os.environ.get("CANLAB_HOME", str(Path.home() / "can-lab-data")))
HOME_DIR.mkdir(parents=True, exist_ok=True)


# ---------------------------------------------------------------- config
def load_config() -> dict:
    cfg: dict[str, Any] = {
        "franke_can": str(BASE.parent / "host" / "franke_can.py"),
        "scripts_dir": str(HOME_DIR / "scripts"),
        "data_dir": str(HOME_DIR / "data"),
        "device_ip": "",
        "device_port": 3333,
        "listen_host": "127.0.0.1",
        "listen_port": 8765,
        "secret": "",
        "max_job_seconds": 4 * 3600,
        "python": sys.executable,
    }
    p = HOME_DIR / "config.json"
    if p.exists():
        cfg.update(json.loads(p.read_text()))
    for k in list(cfg):
        env = os.environ.get("CANLAB_" + k.upper())
        if env:
            cfg[k] = type(cfg[k])(env) if not isinstance(cfg[k], str) else env
    if not cfg["secret"]:
        cfg["secret"] = secrets.token_urlsafe(24)
        stored = json.loads(p.read_text()) if p.exists() else {}
        stored["secret"] = cfg["secret"]
        p.write_text(json.dumps(stored, indent=2))
        print(f"[can-lab] generated secret, stored in {p}")
    return cfg


CFG = load_config()
FRANKE = Path(CFG["franke_can"])
LOG_DIR = FRANKE.resolve().parent.parent / "Messungen"   # where franke_can.py writes its logs
SCRIPTS = Path(CFG["scripts_dir"]); SCRIPTS.mkdir(parents=True, exist_ok=True)
INLINE = SCRIPTS / "_inline"; INLINE.mkdir(exist_ok=True)
DATA = Path(CFG["data_dir"]); DATA.mkdir(parents=True, exist_ok=True)
JOBS_DIR = DATA / "jobs"; JOBS_DIR.mkdir(exist_ok=True)
STATE_FILE = DATA / "state.json"
SECRET = CFG["secret"]
NAME_RE = re.compile(r"^[A-Za-z0-9_\-]{1,64}\.py$")


def now_iso() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds")


# ---------------------------------------------------------------- state
_lock = threading.RLock()


def load_state() -> dict:
    st = {"device_ip": CFG["device_ip"], "device_seen": None, "next_id": 1}
    if STATE_FILE.exists():
        with contextlib.suppress(Exception):
            st.update(json.loads(STATE_FILE.read_text()))
    return st


STATE = load_state()


def save_state() -> None:
    with _lock:
        STATE_FILE.write_text(json.dumps(STATE, indent=2))


def device_url() -> str:
    ip = STATE.get("device_ip") or CFG["device_ip"]
    if not ip:
        raise RuntimeError("device IP unknown - call set_device_ip or let the ESP32 announce itself")
    return f"socket://{ip}:{CFG['device_port']}"


# ---------------------------------------------------------------- jobs
class Job:
    def __init__(self, jid: int, script: str, args: dict, kind: str = "script"):
        self.id = jid
        self.script = script
        self.args = args
        self.kind = kind
        self.status = "queued"
        self.created = now_iso()
        self.started: Optional[str] = None
        self.ended: Optional[str] = None
        self.rc: Optional[int] = None
        self.logs: list[str] = []
        self.proc: Optional[subprocess.Popen] = None
        self.stop_requested = False

    @property
    def dir(self) -> Path:
        return JOBS_DIR / f"{self.id:05d}"

    @property
    def out_file(self) -> Path:
        return self.dir / "output.txt"

    def meta(self) -> dict:
        return {k: getattr(self, k) for k in ("id", "script", "args", "kind", "status", "created", "started", "ended", "rc", "logs")}

    def save(self) -> None:
        self.dir.mkdir(parents=True, exist_ok=True)
        (self.dir / "job.json").write_text(json.dumps(self.meta(), indent=2))


JOBS: "collections.OrderedDict[int, Job]" = collections.OrderedDict()
QUEUE: "collections.deque[int]" = collections.deque()
_cv = threading.Condition(_lock)


def load_jobs() -> None:
    for d in sorted(JOBS_DIR.glob("*/job.json")):
        with contextlib.suppress(Exception):
            m = json.loads(d.read_text())
            j = Job(m["id"], m["script"], m.get("args", {}), m.get("kind", "script"))
            for k in ("status", "created", "started", "ended", "rc", "logs"):
                setattr(j, k, m.get(k))
            j.logs = j.logs or []
            if j.status in ("queued", "running"):
                j.status = "lost (service restarted)"
                j.save()
            JOBS[j.id] = j


def new_job(script: str, args: dict, kind: str = "script") -> Job:
    with _lock:
        jid = STATE["next_id"]; STATE["next_id"] = jid + 1; save_state()
        j = Job(jid, script, args, kind)
        j.save()
        JOBS[jid] = j
        QUEUE.append(jid)
        _cv.notify_all()
        return j


LOGPATH_RE = re.compile(r"(can_[0-9TZ_\-]+[A-Za-z0-9_\-]*\.log)")


def _run(j: Job) -> None:
    script_path = (INLINE if j.kind == "inline" else SCRIPTS) / j.script
    cmd = [CFG["python"], "-u", str(FRANKE), device_url(), "script", str(script_path)]
    for k, v in (j.args or {}).items():
        cmd.append(f"{k}={v}")
    env = dict(os.environ, PYTHONUNBUFFERED="1", NO_COLOR="1", TERM="dumb", COLUMNS="200")
    j.dir.mkdir(parents=True, exist_ok=True)
    with open(j.out_file, "a", encoding="utf-8") as out:
        out.write(f"$ {' '.join(cmd)}\n"); out.flush()
        j.proc = subprocess.Popen(cmd, cwd=str(FRANKE.parent), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                  stdin=subprocess.DEVNULL, text=True, encoding="utf-8", errors="replace",
                                  env=env, start_new_session=True)
        deadline = time.time() + float(CFG["max_job_seconds"])
        assert j.proc.stdout is not None
        for line in j.proc.stdout:
            out.write(line); out.flush()
            for m in LOGPATH_RE.findall(line):
                if m not in j.logs:
                    j.logs.append(m); j.save()
            if time.time() > deadline and not j.stop_requested:
                out.write("[can-lab] max_job_seconds reached - stopping\n")
                stop_job_proc(j)
        j.rc = j.proc.wait()


def worker() -> None:
    while True:
        with _cv:
            while not QUEUE:
                _cv.wait()
            jid = QUEUE.popleft()
            j = JOBS[jid]
            if j.status != "queued":
                continue
            j.status = "running"; j.started = now_iso(); j.save()
        try:
            _run(j)
            j.status = "stopped" if j.stop_requested else ("done" if j.rc == 0 else f"failed (rc={j.rc})")
        except Exception as e:  # noqa: BLE001
            with open(j.out_file, "a") as out:
                out.write(f"[can-lab] error: {e!r}\n")
            j.status = f"error: {e}"
        j.ended = now_iso(); j.proc = None; j.save()


def stop_job_proc(j: Job) -> None:
    """Ctrl+C semantics: franke_can.py runs its cleanup (#stop nolatch -> LISTEN, rules off)."""
    j.stop_requested = True
    if j.proc and j.proc.poll() is None:
        with contextlib.suppress(Exception):
            os.killpg(j.proc.pid, signal.SIGINT)
        def _kill_later(p=j.proc):
            time.sleep(15)
            if p.poll() is None:
                with contextlib.suppress(Exception):
                    os.killpg(p.pid, signal.SIGKILL)
        threading.Thread(target=_kill_later, daemon=True).start()


def running_job() -> Optional[Job]:
    for j in JOBS.values():
        if j.status == "running":
            return j
    return None


def device_cmd(command: str, timeout: float = 15) -> str:
    """Runs `franke_can.py <dev> cmd <command>` (only when no job holds the device)."""
    r = subprocess.run([CFG["python"], str(FRANKE), device_url(), "cmd", command], cwd=str(FRANKE.parent),
                       capture_output=True, text=True, timeout=timeout,
                       env=dict(os.environ, NO_COLOR="1", TERM="dumb", COLUMNS="200"))
    return (r.stdout + r.stderr).strip()


def emergency_stop() -> str:
    msgs = []
    with _lock:
        QUEUE.clear()
        for j in JOBS.values():
            if j.status == "queued":
                j.status = "cancelled"; j.save()
    j = running_job()
    if j:
        stop_job_proc(j); msgs.append(f"job {j.id}: SIGINT sent (script cleanup -> LISTEN)")
        for _ in range(30):
            if j.proc is None or j.proc.poll() is not None:
                break
            time.sleep(0.5)
    try:
        msgs.append(device_cmd("#stop"))
    except Exception as e:  # noqa: BLE001
        msgs.append(f"#stop failed: {e}")
    return "\n".join(msgs)


# ---------------------------------------------------------------- log helpers
def safe_log(name: str) -> Path:
    p = (LOG_DIR / Path(name).name).resolve()
    if p.parent != LOG_DIR.resolve() or not p.exists():
        raise FileNotFoundError(f"no such log file: {name}")
    return p


FRAME_RE = re.compile(r"^\((\d+\.\d+)\)\s+\S+\s+([0-9A-Fa-f]+)#(\S*)")


def parse_ids(s: Optional[str]) -> Optional[set[int]]:
    if not s:
        return None
    return {int(x, 16) for x in re.split(r"[,\s]+", s.strip()) if x}


def iter_frames(p: Path):
    with open(p, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = FRAME_RE.match(line)
            if m:
                yield float(m.group(1)), int(m.group(2), 16), m.group(3).upper(), line.rstrip("\n")


# ---------------------------------------------------------------- MCP
try:
    from mcp.server.transport_security import TransportSecuritySettings
    _ts = TransportSecuritySettings(enable_dns_rebinding_protection=False)
except Exception:  # noqa: BLE001
    _ts = None

INSTRUCTIONS = """Lab bench: ESP32 CAN tool on the CAN bus of a Franke FM850 coffee machine (125 kbit/s).
Scripts use the franke_can script API (mode, send, rule, collect, mark, csv, result, log, args ...),
see get_scripting_doc(). One job runs at a time; jobs keep running when you disconnect.
Safety: never send 480#00 01 03 01 ... (starts an endless broadcast flood) unless the user asked.
Keep test traffic low (firmware limit 50 frames/s). emergency_stop() stops everything (latched)."""

mcp = FastMCP("can-lab", instructions=INSTRUCTIONS, host=CFG["listen_host"], port=int(CFG["listen_port"]),
              streamable_http_path=f"/{SECRET}/mcp", stateless_http=True, json_response=True,
              **({"transport_security": _ts} if _ts else {}))


@mcp.tool()
def device_status() -> dict:
    """Device IP, last announce, running job and (if the bus is free) the firmware #status line."""
    j = running_job()
    out: dict[str, Any] = {"device_ip": STATE.get("device_ip"), "last_announce": STATE.get("device_seen"),
                           "running_job": j.meta() if j else None, "queued": list(QUEUE)}
    if not j:
        try:
            out["status"] = device_cmd("#status")
        except Exception as e:  # noqa: BLE001
            out["status_error"] = str(e)
    return out


@mcp.tool()
def set_device_ip(ip: str) -> str:
    """Set the ESP32 IP address (used for all following jobs)."""
    if not re.fullmatch(r"\d{1,3}(\.\d{1,3}){3}", ip):
        raise ValueError("not an IPv4 address")
    STATE["device_ip"] = ip; save_state()
    return f"device ip = {ip}"


@mcp.tool()
def get_scripting_doc() -> str:
    """The SCRIPTING.md of the host tool (script API, rules syntax, safety rules)."""
    p = FRANKE.parent / "SCRIPTING.md"
    return p.read_text(encoding="utf-8") if p.exists() else "SCRIPTING.md not found"


@mcp.tool()
def list_scripts() -> list[dict]:
    """Scripts available to run_script (name, size, modified)."""
    return [{"name": p.name, "bytes": p.stat().st_size,
             "modified": dt.datetime.fromtimestamp(p.stat().st_mtime, dt.timezone.utc).isoformat(timespec="seconds")}
            for p in sorted(SCRIPTS.glob("*.py"))]


@mcp.tool()
def get_script(name: str) -> str:
    """Source of a script."""
    if not NAME_RE.match(name):
        raise ValueError("invalid script name")
    return (SCRIPTS / name).read_text(encoding="utf-8")


@mcp.tool()
def put_script(name: str, content: str) -> str:
    """Create or overwrite a script (name like 'reply_detect.py'). The old version is kept as .bak."""
    if not NAME_RE.match(name):
        raise ValueError("invalid script name (letters, digits, _ and -, ending in .py)")
    compile(content, name, "exec")   # syntax check
    p = SCRIPTS / name
    bak = SCRIPTS / (name + ".bak")
    for f in (p, bak):   # files copied from read-only sources may lack the write bit
        if f.exists() and not os.access(f, os.W_OK):
            os.chmod(f, 0o644)
    if p.exists():
        bak.write_text(p.read_text(encoding="utf-8"), encoding="utf-8")
    p.write_text(content, encoding="utf-8")
    return f"saved {name} ({len(content)} bytes)"


@mcp.tool()
def run_script(name: str, args: Optional[dict] = None) -> dict:
    """Queue a script from the scripts dir with key=value args. Returns the job (id, status)."""
    if not NAME_RE.match(name) or not (SCRIPTS / name).exists():
        raise ValueError(f"no such script: {name}")
    return new_job(name, args or {}).meta()


@mcp.tool()
def run_inline(code: str, args: Optional[dict] = None) -> dict:
    """Queue a short ad-hoc script (same API as scripts). Kept on disk as _inline/job_<id>.py."""
    compile(code, "inline", "exec")
    with _lock:
        jid = STATE["next_id"]
    name = f"job_{jid:05d}.py"
    (INLINE / name).write_text(code, encoding="utf-8")
    return new_job(name, args or {}, kind="inline").meta()


@mcp.tool()
def job_status(job_id: Optional[int] = None, last: int = 10) -> Any:
    """One job (by id) or the last N jobs."""
    if job_id is not None:
        return JOBS[job_id].meta()
    return [j.meta() for j in list(JOBS.values())[-last:]]


@mcp.tool()
def job_output(job_id: int, from_line: int = 0, max_lines: int = 200, wait_seconds: float = 0) -> dict:
    """Console output of a job from line `from_line`. wait_seconds (<=120) waits for the job to end first."""
    j = JOBS[job_id]
    end = time.time() + min(max(wait_seconds, 0), 120)
    while time.time() < end and j.status in ("queued", "running"):
        time.sleep(1)
    lines = j.out_file.read_text(encoding="utf-8", errors="replace").splitlines() if j.out_file.exists() else []
    chunk = lines[from_line:from_line + max(1, min(max_lines, 1000))]
    return {"status": j.status, "total_lines": len(lines), "from_line": from_line,
            "next_line": from_line + len(chunk), "lines": chunk, "logs": j.logs}


@mcp.tool()
def stop_job(job_id: int) -> str:
    """Stop a job like Ctrl+C (its cleanup sets the device to LISTEN, rules off). Removes it if still queued."""
    j = JOBS[job_id]
    with _lock:
        if j.status == "queued":
            with contextlib.suppress(ValueError):
                QUEUE.remove(job_id)
            j.status = "cancelled"; j.save()
            return "cancelled"
    stop_job_proc(j)
    return "SIGINT sent"


@mcp.tool(name="emergency_stop")
def emergency_stop_tool() -> str:
    """Cancel queue, stop the running job, send latched #stop to the device (release in the web UI)."""
    return emergency_stop()


@mcp.tool()
def list_logs(limit: int = 20, contains: str = "") -> list[dict]:
    """Newest files in Messungen (logs, markers, events, csv)."""
    files = sorted((p for p in LOG_DIR.glob("*") if p.is_file() and contains in p.name),
                   key=lambda p: p.stat().st_mtime, reverse=True)[:max(1, min(limit, 200))]
    return [{"name": p.name, "bytes": p.stat().st_size,
             "modified": dt.datetime.fromtimestamp(p.stat().st_mtime, dt.timezone.utc).isoformat(timespec="seconds")}
            for p in files]


@mcp.tool()
def read_text_file(name: str, from_line: int = 0, max_lines: int = 300) -> dict:
    """Read a markers/events/csv (or any) file from Messungen in chunks."""
    lines = safe_log(name).read_text(encoding="utf-8", errors="replace").splitlines()
    chunk = lines[from_line:from_line + max(1, min(max_lines, 2000))]
    return {"total_lines": len(lines), "next_line": from_line + len(chunk), "lines": chunk}


@mcp.tool()
def read_log(name: str, ids: str = "", exclude_ids: str = "", t_from: float = 0, t_to: float = 0,
             data_contains: str = "", max_lines: int = 400) -> dict:
    """Filtered candump log. ids/exclude_ids: hex list '481,4C6'. t_from/t_to: epoch seconds (0 = open).
    data_contains: hex substring of the payload, e.g. '110021'."""
    inc, exc = parse_ids(ids), parse_ids(exclude_ids)
    out, n = [], 0
    for t, cid, data, line in iter_frames(safe_log(name)):
        if inc is not None and cid not in inc: continue
        if exc is not None and cid in exc: continue
        if t_from and t < t_from: continue
        if t_to and t > t_to: continue
        if data_contains and data_contains.upper().replace(" ", "") not in data: continue
        n += 1
        if len(out) < max(1, min(max_lines, 3000)):
            out.append(line)
    return {"matched": n, "returned": len(out), "lines": out}


@mcp.tool()
def log_summary(name: str, top_payloads: int = 8) -> dict:
    """Per CAN ID: count, first/last time, number of distinct payloads and the most frequent ones."""
    stats: dict[int, dict] = {}
    t0 = t1 = None
    for t, cid, data, _ in iter_frames(safe_log(name)):
        t0 = t if t0 is None else t0; t1 = t
        s = stats.setdefault(cid, {"count": 0, "first": t, "last": t, "payloads": collections.Counter()})
        s["count"] += 1; s["last"] = t; s["payloads"][data] += 1
    return {"start": t0, "end": t1, "ids": {
        f"{cid:03X}": {"count": s["count"], "first": s["first"], "last": s["last"], "distinct": len(s["payloads"]),
                       "top": s["payloads"].most_common(top_payloads)} for cid, s in sorted(stats.items())}}


# ---------------------------------------------------------------- web routes
P = f"/{SECRET}"


@mcp.custom_route(P + "/announce", methods=["GET", "POST"])
async def announce(request: Request):
    ip = request.query_params.get("ip") or request.client.host
    if not re.fullmatch(r"\d{1,3}(\.\d{1,3}){3}", ip or ""):
        return PlainTextResponse("bad ip", status_code=400)
    STATE["device_ip"] = ip; STATE["device_seen"] = now_iso()
    STATE["device_info"] = dict(request.query_params); save_state()
    return PlainTextResponse("ok")


@mcp.custom_route(P + "/stop", methods=["POST"])
async def web_stop(request: Request):
    msg = await __import__("anyio").to_thread.run_sync(emergency_stop)
    return PlainTextResponse("EMERGENCY STOP\n" + msg)


@mcp.custom_route(P + "/logs/{fname}", methods=["GET"])
async def web_log(request: Request):
    try:
        return FileResponse(safe_log(request.path_params["fname"]))
    except FileNotFoundError:
        return PlainTextResponse("not found", status_code=404)


@mcp.custom_route(P + "/job/{jid}", methods=["GET"])
async def web_job(request: Request):
    j = JOBS.get(int(request.path_params["jid"]))
    if not j or not j.out_file.exists():
        return PlainTextResponse("no output yet")
    return PlainTextResponse(j.out_file.read_text(encoding="utf-8", errors="replace")[-200000:])


@mcp.custom_route(P + "/", methods=["GET"])
async def web_index(request: Request):
    j = running_job()
    rows = "".join(
        f"<tr><td><a href='{P}/job/{x.id}'>{x.id}</a></td><td>{html.escape(x.script)}</td><td>{html.escape(json.dumps(x.args))}</td>"
        f"<td>{html.escape(x.status)}</td><td>{x.started or ''}</td><td>{x.ended or ''}</td></tr>"
        for x in reversed(list(JOBS.values())[-30:]))
    logs = "".join(f"<li><a href='{P}/logs/{html.escape(l['name'])}'>{html.escape(l['name'])}</a> ({l['bytes']} B)</li>"
                   for l in list_logs(30))
    body = f"""<!doctype html><meta charset=utf-8><meta http-equiv=refresh content=10>
<title>can-lab</title><style>body{{font-family:system-ui;margin:2em;background:#111;color:#ddd}}a{{color:#8cf}}
td,th{{padding:2px 8px;border-bottom:1px solid #333}}button{{font-size:1.4em;background:#d33;color:#fff;border:0;padding:.5em 1em}}</style>
<h1>can-lab</h1><p>Device: <b>{html.escape(str(STATE.get('device_ip')))}</b> (last announce {STATE.get('device_seen')})<br>
Running: {html.escape(j.script) + ' (job ' + str(j.id) + ')' if j else '-'} · queued: {list(QUEUE)}</p>
<form method=post action='{P}/stop' onsubmit="return confirm('Emergency stop?')"><button>EMERGENCY STOP</button></form>
<h2>Jobs</h2><table><tr><th>id</th><th>script</th><th>args</th><th>status</th><th>start</th><th>end</th></tr>{rows}</table>
<h2>Logs</h2><ul>{logs}</ul>"""
    return HTMLResponse(body)


@mcp.custom_route("/", methods=["GET"])
async def root(request: Request):
    return PlainTextResponse("can-lab")


def main() -> None:
    load_jobs()
    threading.Thread(target=worker, daemon=True).start()
    print(f"[can-lab] franke_can: {FRANKE}\n[can-lab] logs: {LOG_DIR}\n[can-lab] scripts: {SCRIPTS}")
    print(f"[can-lab] MCP:  http://{CFG['listen_host']}:{CFG['listen_port']}{P}/mcp")
    print(f"[can-lab] Web:  http://{CFG['listen_host']}:{CFG['listen_port']}{P}/")
    mcp.run(transport="streamable-http")


if __name__ == "__main__":
    main()
