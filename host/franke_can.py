#!/usr/bin/env python3
"""Host tool for the ESP32 CAN sniffer / payment-system emulator (Franke FM850).

Usage:
  py franke_can.py COM7 monitor [--id 481,4C6] [--exclude 181] [--changes] [--no-log]
  py franke_can.py socket://franke-can.local:3333 monitor
  py franke_can.py COM7 script scripts/test_kasse.py [--keep-active]
  py franke_can.py COM7 cmd "#status"
  py franke_can.py decode Messungen/can_2026-11-06_14-30-00.log [--id 481] [--stats-only]

While monitoring, type "#..." firmware commands or frames like "481#124655" and press Enter.
"quit" (or Ctrl+C) ends the monitor.
"""

from __future__ import annotations

import argparse
import collections
import datetime as dt
import queue
import re
import runpy
import sys
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Iterable, Optional, Sequence

from known_frames import decode, frame_type

try:
    import serial  # pyserial
except ImportError:  # pragma: no cover
    serial = None

try:
    from rich.console import Console
    from rich.markup import escape

    _console: Optional[Console] = Console(highlight=False)
except ImportError:  # pragma: no cover
    _console = None

    def escape(s: str) -> str:  # type: ignore[misc]
        return s


BAUD = 921600
LOG_DIR = Path(__file__).resolve().parent.parent / "Messungen"
ASYNC_TAGS = ("#stop", "#busoff", "#busrecovered", "#warn", "#txerr", "#error", "#info", "#boot", "#ota", "#hello",
              "#mark ", "#clock ")
CLOCK_RESYNC_S = 60


def utc_iso(epoch: float) -> str:
    """2026-11-06T15:12:03.123456Z - all wall-clock times are UTC."""
    return dt.datetime.fromtimestamp(epoch, dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%fZ")


def utc_stamp() -> str:
    """For file names: 2026-11-06_15-12-03Z"""
    return dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%d_%H-%M-%SZ")


def out(text: str, style: str = "") -> None:
    """Print a line (rich markup if available)."""
    if _console:
        _console.print(f"[{style}]{escape(text)}[/]" if style else escape(text))
    else:
        print(text, flush=True)


# --------------------------------------------------------------------------------------
# Frames
# --------------------------------------------------------------------------------------


@dataclass
class Frame:
    dev_us: int                 # device time, µs since boot
    wall: float                 # host wall clock (epoch seconds)
    can_id: int
    data: tuple[int, ...]
    ext: bool = False
    rtr: bool = False
    dlc: int = 0
    tx: bool = False            # sent by the ESP32 itself (echo)
    reason: str = ""            # "manual", rule name, ...
    host_time: float = field(default_factory=time.monotonic)

    @property
    def id_str(self) -> str:
        return f"{self.can_id:08X}" if self.ext else f"{self.can_id:03X}"

    @property
    def data_str(self) -> str:
        return "RTR" if self.rtr else " ".join(f"{b:02X}" for b in self.data)

    def candump(self, channel: str = "can0") -> str:
        payload = "R" if self.rtr else "".join(f"{b:02X}" for b in self.data)
        return f"({self.wall:.6f}) {channel} {self.id_str}#{payload}"

    @property
    def meaning(self) -> Optional[str]:
        return decode(self.can_id, self.data)


def parse_hex_bytes(text: str) -> list[int]:
    text = text.replace(" ", "").replace(".", "").replace(":", "")
    if len(text) % 2:
        raise ValueError(f"odd number of hex digits: {text!r}")
    return [int(text[i : i + 2], 16) for i in range(0, len(text), 2)]


def make_pattern(data: "str | Sequence[int] | None") -> Optional[list[tuple[int, int]]]:
    """'11 00 ??' / '1?' / [0x11, None] -> list of (value, mask) for prefix matching."""
    if data is None:
        return None
    if isinstance(data, str):
        toks = data.split() if " " in data.strip() else [data[i : i + 2] for i in range(0, len(data), 2)]
        pat = []
        for t in toks:
            if len(t) != 2:
                raise ValueError(f"bad byte {t!r}")
            v = m = 0
            for i, c in enumerate(t):
                shift = 4 if i == 0 else 0
                if c != "?":
                    v |= int(c, 16) << shift
                    m |= 0xF << shift
            pat.append((v, m))
        return pat
    return [(0, 0) if b is None else (int(b), 0xFF) for b in data]


def matches(f: Frame, can_id: Optional[int], pattern: Optional[list[tuple[int, int]]]) -> bool:
    if can_id is not None and f.can_id != can_id:
        return False
    if pattern:
        if len(f.data) < len(pattern):
            return False
        return all((f.data[i] & m) == (v & m) for i, (v, m) in enumerate(pattern))
    return True


# --------------------------------------------------------------------------------------
# Device connection
# --------------------------------------------------------------------------------------


class DeviceError(RuntimeError):
    pass


class ScriptStopped(RuntimeError):
    """The STOP latch is set (STOP button, #stop, BOOT key, rate limit). A script must end immediately."""


class Device:
    """Connection to the firmware over USB serial or TCP (pyserial URL socket://host:port)."""

    def __init__(self, url: str):
        if serial is None:
            raise SystemExit("pyserial missing: py -m pip install -r requirements.txt")
        self.url = url
        self.ser = self._open()
        self._wlock = threading.Lock()
        self._cmd_lock = threading.Lock()
        self._reply_q: "queue.Queue[str]" = queue.Queue()
        self._pending: Optional[str] = None
        self._listeners: list[Callable[[Frame], None]] = []
        self._notify_listeners: list[Callable[[str], None]] = []
        self.history: "collections.deque[Frame]" = collections.deque(maxlen=20000)
        self._hist_cv = threading.Condition()
        self._offset: Optional[float] = None       # fallback mapping (PC clock), seconds
        self._offset_n = 0
        self._last_us = 0
        self.clock_offset_us: Optional[int] = None  # device clock: epoch_us - boot_us (NTP or set by a client)
        self.clock_source = "none"
        self._marker_listeners: list[Callable[[float, int, str], None]] = []
        self._stop = threading.Event()
        self._restore: list[str] = []
        self._echo_wanted = False
        # replay after a reconnect: live frames are held back until the missed ones are in
        self._replay_lock = threading.Lock()
        self._replaying = False
        self._live_buf: list[str] = []
        self._replay_lines: list[str] = []
        self._replay_msgs: list[str] = []
        self._replay_gap: Optional[tuple[int, int]] = None
        self._seen_marks: set[tuple[int, str]] = set()
        self.connected = True
        # "ready" = connected and the replay of missed frames is done. Commands from other threads wait
        # for it (up to reconnect_timeout), so a short dropout does not abort a running script.
        self._ready = threading.Event()
        self._ready.set()
        self.reconnect_timeout = 120.0
        self._internal_thread: Optional[threading.Thread] = None   # the reconnect thread may always send
        self._down_total = 0.0          # seconds spent not ready (for dropout-aware timeouts)
        self._down_since: Optional[float] = None
        self._hist_total = 0            # frames ever appended to history (for incremental scans)
        self._ref_dev_us: Optional[int] = None   # device time reference for dev_now_us()
        self._ref_mono = 0.0
        self.abort_check: Optional[Callable[[], None]] = None   # called in wait loops (may raise)
        self._rx_thread = threading.Thread(target=self._reader, name="can-reader", daemon=True)
        self._rx_thread.start()

    # ---- low level ----
    def _open(self):
        s = serial.serial_for_url(self.url, baudrate=BAUD, timeout=0.05, do_not_open=True)
        # do not toggle DTR/RTS: on most ESP32 boards that resets the chip
        try:
            s.dtr = False
            s.rts = False
        except (AttributeError, ValueError, serial.SerialException):
            pass
        s.open()
        return s

    def write_line(self, text: str) -> None:
        with self._wlock:
            try:
                self.ser.write((text + "\r").encode("ascii", errors="replace"))
                self.ser.flush()
            except Exception as e:  # disconnected: the reader thread reconnects
                raise DeviceError(f"not connected ({e})") from None

    def _reconnect(self, reason: str) -> bool:
        """Reopen the port until it works (or close() is called). Returns False if stopped."""
        self.connected = False
        self._ready.clear()
        if self._down_since is None:
            self._down_since = time.monotonic()
        self._emit_notify(f"#error connection lost ({reason}) - reconnecting")
        try:
            self.ser.close()
        except Exception:
            pass
        while not self._stop.wait(2.0):
            try:
                self.ser = self._open()
            except Exception:
                continue
            self.connected = True
            self._emit_notify("#info reconnected - fetching missed frames")
            with self._replay_lock:   # hold back live frames until the replay is done
                self._replaying = True
                self._live_buf = []
            threading.Thread(target=self._after_reconnect, name="can-replay", daemon=True).start()
            return True
        return False

    def _after_reconnect(self) -> None:
        self._internal_thread = threading.current_thread()
        since = self._last_us
        old_off = self.clock_offset_us
        try:
            time.sleep(0.3)
            self.write_line("")
            st = self.status()
            if st.get("format") != "text":
                self.command("#format text")
            if self._echo_wanted and st.get("echo") != "on":
                self.command("#echo on")
            self.sync_clock()
            boot_now = self.dev_now_from_clock()
            rebooted = (boot_now is not None and boot_now < since) or (
                old_off is not None and self.clock_offset_us is not None and abs(old_off - self.clock_offset_us) > 5e6)
            if since and rebooted:
                frm = (since + old_off) / 1e6 if old_off is not None else None
                self._gap(0, frm or time.time(), f"DATA GAP: device restarted - no data from "
                          f"{utc_iso(frm) if frm else '?'} until {utc_iso(time.time())}")
                self._last_us = 0
            elif since:
                self._do_replay(since)
        except DeviceError as e:
            self._gap(since, self.wall_of(since), f"DATA GAP: missed data could not be fetched ({e})")
        finally:
            with self._replay_lock:
                live, self._live_buf = self._live_buf, []
                for line in live:
                    self._handle_frame_line(line)
                self._replaying = False
            if self._down_since is not None:
                self._down_total += time.monotonic() - self._down_since
                self._down_since = None
            self._internal_thread = None
            self._ready.set()

    def downtime(self) -> float:
        """Total seconds the connection was not ready (lost or replaying). Used to extend timeouts."""
        cur = time.monotonic() - self._down_since if self._down_since is not None else 0.0
        return self._down_total + cur

    def wait_ready(self, timeout: Optional[float] = None) -> bool:
        return self._ready.wait(self.reconnect_timeout if timeout is None else timeout)

    def _do_replay(self, since: int) -> None:
        self._replay_lines, self._replay_msgs, self._replay_gap = [], [], None
        lines = self.command(f"#replay {since}", timeout=20.0)
        if not lines[-1].startswith("#ok"):
            self._gap(since, self.wall_of(since), f"DATA GAP: missed data could not be fetched ({lines[-1]})")
            return
        with self._replay_lock:
            first_live = int(self._live_buf[0].split()[1]) if self._live_buf else None
        if self._replay_gap:
            a, b = self._replay_gap
            self._gap(a, self.wall_of(a), f"DATA GAP: no data from {utc_iso(self.wall_of(a))} to "
                      f"{utc_iso(self.wall_of(b))} (device buffer too small)")
        n = 0
        for line in self._replay_lines:
            us = int(line.split()[1])
            if us > since and (first_live is None or us < first_live):
                self._handle_frame_line("#" + line[2:])   # "#prx ..." -> "#rx ..."
                n += 1
        for line in self._replay_msgs:
            m = re.match(r"^#pmsg (\d+) (.*)$", line)
            if not m:
                continue
            if m.group(2).startswith("#mark "):
                self._dispatch(m.group(2))   # duplicates are ignored
            else:
                self._emit_notify("#info [while disconnected] " + m.group(2))
        self._emit_notify(f"#info recovered {n} frames from the device buffer")

    def _gap(self, us: int, wall: float, note: str) -> None:
        self._emit_notify("#warn " + note)
        for cb in list(self._marker_listeners):
            cb(wall, us, note)

    def dev_now_from_clock(self) -> Optional[int]:
        """Device time right now, from the last #time answer (set by sync_clock)."""
        return getattr(self, "_time_boot", None)

    def _reader(self) -> None:
        buf = bytearray()
        while not self._stop.is_set():
            try:
                chunk = self.ser.read(4096)
            except Exception as e:  # serial unplugged, TCP closed
                if self._stop.is_set() or not self._reconnect(str(e)):
                    return
                buf.clear()
                continue
            if not chunk:
                if self.url.startswith("socket://") and not self._socket_alive():
                    if not self._reconnect("socket closed"):
                        return
                    buf.clear()
                continue
            buf.extend(chunk)
            while True:
                idx = next((i for i, b in enumerate(buf) if b in (0x0A, 0x0D, 0x07)), -1)
                if idx < 0:
                    if len(buf) > 4096:
                        buf.clear()
                    break
                raw = bytes(buf[:idx])
                term = buf[idx]
                del buf[: idx + 1]
                if term == 0x07:
                    self._dispatch("\a")
                if raw:
                    self._dispatch(raw.decode("ascii", errors="replace"))

    def _socket_alive(self) -> bool:
        """pyserial's socket:// returns b'' on a closed connection instead of raising."""
        sock = getattr(self.ser, "_socket", None)
        if sock is None:
            return False
        try:
            import select
            r, _, _ = select.select([sock], [], [], 0)
            if r:
                return sock.recv(1, 0x02) != b""   # MSG_PEEK: readable but empty = closed
        except OSError:
            return False
        return True

    def _handle_frame_line(self, line: str) -> None:
        f = self._parse_frame(line)
        if f:
            with self._hist_cv:
                self.history.append(f)
                self._hist_total += 1
                if self._ref_dev_us is None or f.dev_us >= self._ref_dev_us:
                    self._ref_dev_us, self._ref_mono = f.dev_us, time.monotonic()
                self._hist_cv.notify_all()
            for cb in list(self._listeners):
                cb(f)

    def _dispatch(self, line: str) -> None:
        if line.startswith("#rx ") or line.startswith("#tx "):
            with self._replay_lock:
                if self._replaying:
                    self._live_buf.append(line)
                    return
            self._handle_frame_line(line)
            return
        if line.startswith("#prx ") or line.startswith("#ptx "):
            self._replay_lines.append(line)
            return
        if line.startswith("#pmsg "):
            self._replay_msgs.append(line)
            return
        if line.startswith("#replay gap "):
            t = line.split()
            self._replay_gap = (int(t[2]), int(t[3]))
            return
        if not line.startswith("#"):
            return  # SLCAN frames (if format is still slcan) / boot garbage
        if line.startswith("#mark "):
            m = re.match(r"^#mark (\d+) (\d+) ?(.*)$", line)
            if m:
                boot, epoch, note = int(m.group(1)), int(m.group(2)), m.group(3)
                if (boot, note) in self._seen_marks:
                    return   # already received live (marker came again from the replay buffer)
                self._seen_marks.add((boot, note))
                wall = epoch / 1e6 if epoch else self.wall_of(boot)
                for cb in list(self._marker_listeners):
                    cb(wall, boot, note)
        if line.startswith("#boot"):
            self.clock_offset_us = None   # device restarted: its clock is unknown until the next sync
        pending = self._pending
        is_async = line.startswith(ASYNC_TAGS) or (line.startswith("#wifi ") and not (pending or "").startswith("#wifi"))
        if pending and not is_async:
            self._reply_q.put(line)
        else:
            self._emit_notify(line)

    def _emit_notify(self, line: str) -> None:
        for cb in list(self._notify_listeners):
            cb(line)

    def _parse_frame(self, line: str) -> Optional[Frame]:
        t = line.split()
        try:
            tx = t[0] == "#tx"
            us = int(t[1])
            id_s = t[2]
            dlc = int(t[3])
            rtr = len(t) > 4 and t[4] == "R"
            data = () if rtr else tuple(int(x, 16) for x in t[4 : 4 + dlc])
            reason = " ".join(t[4 + (1 if rtr else dlc) :]) if tx else ""
        except (IndexError, ValueError):
            return None
        # fallback mapping (device clock not set): minimum offset over the first frames has the least latency
        now = time.time()
        if us < self._last_us:
            self._offset, self._offset_n = None, 0   # device rebooted
            self.clock_offset_us = None
        self._last_us = us
        off = now - us / 1e6
        if self._offset is None or (self._offset_n < 500 and off < self._offset):
            self._offset = off
        self._offset_n += 1
        return Frame(dev_us=us, wall=self.wall_of(us), can_id=int(id_s, 16), data=data, ext=len(id_s) > 3,
                     rtr=rtr, dlc=dlc, tx=tx, reason=reason)

    def wall_of(self, dev_us: int) -> float:
        """Device µs since boot -> epoch seconds (UTC). Uses the device clock if set, else the PC clock."""
        if self.clock_offset_us is not None:
            return (dev_us + self.clock_offset_us) / 1e6
        if self._offset is None:
            self._offset = time.time() - dev_us / 1e6
        return dev_us / 1e6 + self._offset

    def sync_clock(self) -> str:
        """Reads the device clock (#time). If it is not set (no NTP), sets it from this PC. Returns the source."""
        def read() -> None:
            for line in self.command("#time"):
                t = line.split()
                if t[0] == "#time" and len(t) >= 4:
                    boot, epoch = int(t[1]), int(t[2])
                    self._time_boot = boot
                    with self._hist_cv:
                        if self._ref_dev_us is None or boot >= self._ref_dev_us or boot < self._ref_dev_us - 60_000_000:
                            self._ref_dev_us, self._ref_mono = boot, time.monotonic()
                    self.clock_source = t[3]
                    self.clock_offset_us = epoch - boot if epoch else None
        read()
        if self.clock_source == "none":
            self.command(f"#time set {int(time.time() * 1000)}")
            read()
        return self.clock_source

    def _clock_loop(self) -> None:
        while not self._stop.wait(CLOCK_RESYNC_S):
            try:
                self.sync_clock()
            except DeviceError:
                pass

    def dev_now_us(self) -> Optional[int]:
        """Estimated current device time (µs since boot), from the newest frame or #time answer."""
        with self._hist_cv:
            if self._ref_dev_us is None:
                return None
            return int(self._ref_dev_us + (time.monotonic() - self._ref_mono) * 1e6)

    def on_marker(self, cb: Callable[[float, int, str], None]) -> None:
        """cb(wall_epoch_s, boot_us, note) for every marker (from any client)."""
        self._marker_listeners.append(cb)

    # ---- API ----
    def on_frame(self, cb: Callable[[Frame], None]) -> None:
        self._listeners.append(cb)

    def on_notify(self, cb: Callable[[str], None]) -> None:
        self._notify_listeners.append(cb)

    def command(self, text: str, timeout: float = 3.0, check: bool = False) -> list[str]:
        """Sends a "#..." command and returns all reply lines up to and including #ok/#err.
        While the connection is down, it first waits (up to reconnect_timeout) until it is back."""
        if threading.current_thread() is not self._internal_thread and not self._ready.is_set():
            if not self._ready.wait(self.reconnect_timeout):
                raise DeviceError(f"not connected (waited {self.reconnect_timeout:.0f} s) for {text!r}")
        with self._cmd_lock:
            while not self._reply_q.empty():
                self._reply_q.get_nowait()
            self._pending = text
            try:
                self.write_line(text)
                lines: list[str] = []
                end = time.monotonic() + timeout
                while True:
                    left = end - time.monotonic()
                    if left <= 0:
                        raise DeviceError(f"timeout waiting for reply to {text!r}")
                    try:
                        line = self._reply_q.get(timeout=left)
                    except queue.Empty:
                        continue
                    lines.append(line)
                    if line.startswith("#ok") or line.startswith("#err"):
                        break
            finally:
                self._pending = None
        if check and lines[-1].startswith("#err"):
            raise DeviceError(lines[-1])
        return lines

    def status(self) -> dict[str, str]:
        for line in self.command("#status"):
            if line.startswith("#status "):
                return dict(tok.split("=", 1) for tok in line.split()[1:] if "=" in tok)
        return {}

    def setup_text_stream(self, echo: bool = True) -> dict[str, str]:
        """Switch to the µs text format (+ echo); remembers what to restore on close()."""
        st = {}
        for _ in range(3):
            try:
                self.write_line("")
                st = self.status()
                break
            except DeviceError:
                time.sleep(0.5)   # device may just be booting
        if not st:
            raise DeviceError(f"no answer from the firmware on {self.url}")
        if st.get("format") != "text":
            self.command("#format text", check=True)
            self._restore.append(f"#format {st.get('format', 'slcan')}")
        self._echo_wanted = echo
        if echo and st.get("echo") != "on":
            self.command("#echo on", check=True)
            self._restore.append("#echo off")
        try:
            st["clock"] = self.sync_clock()
            threading.Thread(target=self._clock_loop, name="clock-sync", daemon=True).start()
        except DeviceError:
            st["clock"] = "pc (old firmware)"
        return st

    def frames_since(self, since: float) -> list[Frame]:
        with self._hist_cv:
            return [f for f in self.history if f.host_time >= since]

    def frames_between(self, start_us: int, end_us: Optional[int] = None, rx_only: bool = True) -> list[Frame]:
        """Frames by DEVICE time (µs since boot), incl. frames recovered by a replay."""
        with self._hist_cv:
            return [f for f in self.history if f.dev_us >= start_us and (end_us is None or f.dev_us < end_us)
                    and (not rx_only or not f.tx)]

    def sleep_connected(self, seconds: float) -> None:
        """Sleeps `seconds` of CONNECTED time: time without connection is added, and it returns only
        once the connection is ready again (missed frames replayed)."""
        d0, t0 = self.downtime(), time.monotonic()
        while True:
            if self.abort_check:
                self.abort_check()
            left = seconds - (time.monotonic() - t0 - (self.downtime() - d0))
            if left <= 0 and self._ready.is_set():
                return
            if self._stop.wait(min(max(left, 0.05), 0.2)):
                return

    def wait_for(self, can_id: Optional[int] = None, data=None, timeout: float = 1.0,
                 since_us: Optional[int] = None, rx_only: bool = True) -> Optional[Frame]:
        """First frame matching id/data with device time >= since_us (default: now). The timeout only
        counts connected time, and it never ends during a replay, so a dropout cannot fake a timeout."""
        pat = make_pattern(data)
        if since_us is None:
            since_us = self.dev_now_us() or 0
        d0, t0 = self.downtime(), time.monotonic()
        with self._hist_cv:
            pending = list(self.history)       # first pass: everything already received
            seen_total = self._hist_total
            while True:
                for f in pending:
                    if f.dev_us >= since_us and (not rx_only or not f.tx) and matches(f, can_id, pat):
                        return f
                if self.abort_check:
                    self.abort_check()
                left = timeout - (time.monotonic() - t0 - (self.downtime() - d0))
                if left <= 0 and self._ready.is_set():
                    return None
                self._hist_cv.wait(timeout=min(max(left, 0.05), 0.1))
                new = self._hist_total - seen_total
                seen_total = self._hist_total
                pending = list(self.history)[-new:] if new > 0 else []

    def close(self) -> None:
        for c in self._restore:
            try:
                self.command(c, timeout=1.0)
            except DeviceError:
                pass
        self._stop.set()
        self._rx_thread.join(timeout=1.0)
        self.ser.close()


def split_commands(text: str) -> list[str]:
    """'#mode active; #send 481#11' -> two commands. ';' and new lines separate, except inside "quotes"
    or written as '\\;'. Same rules as the web console."""
    out: list[str] = []
    cur, quoted, i = "", False, 0
    while i < len(text):
        c = text[i]
        if c == "\\" and text[i + 1 : i + 2] == ";":
            cur += ";"
            i += 2
            continue
        if c == "\\" and quoted and i + 1 < len(text):   # keep \" and \\ for the firmware
            cur += text[i : i + 2]
            i += 2
            continue
        if c == '"':
            quoted = not quoted
        if not quoted and c in ";\r\n":
            if cur.strip():
                out.append(cur.strip())
            cur = ""
        else:
            cur += c
        i += 1
    if cur.strip():
        out.append(cur.strip())
    return out


def frame_command(text: str) -> str:
    """'481#124655' -> '#send 481#124655' (other input is passed through)."""
    if re.fullmatch(r"[0-9A-Fa-f]{1,8}#(R\d?|[0-9A-Fa-f. ]*)", text.strip()):
        return "#send " + text.strip()
    return text


# --------------------------------------------------------------------------------------
# Logging (candump format + events side file)
# --------------------------------------------------------------------------------------


class Recorder:
    def __init__(self, enabled: bool = True, tag: str = ""):
        self.enabled = enabled
        self.count = 0
        self.path: Optional[Path] = None
        self._lock = threading.Lock()
        if not enabled:
            self._log = self._ev = None
            return
        LOG_DIR.mkdir(parents=True, exist_ok=True)
        self.path = LOG_DIR / f"can_{utc_stamp()}{tag}.log"          # UTC
        self.events_path = self.path.with_suffix(".events.txt")
        self.markers_path = self.path.with_suffix(".markers.txt")
        self.markers = 0
        self._log = open(self.path, "w", encoding="ascii", newline="\n")
        self._ev = open(self.events_path, "w", encoding="utf-8", newline="\n")
        self._mk = None   # created with the first marker

    def frame(self, f: Frame) -> None:
        if not self._log:
            return
        with self._lock:
            self._log.write(f.candump() + "\n")
            self.count += 1

    def event(self, text: str) -> None:
        if not self._ev:
            return
        with self._lock:
            self._ev.write(f"{utc_iso(time.time())}  {text}\n")
            self._ev.flush()

    def marker(self, wall: float, note: str) -> None:
        """Markers file: "(epoch) 2026-11-06T15:12:03.123456Z note" - same timestamps as the .log."""
        if not self._ev:
            return
        with self._lock:
            if self._mk is None:
                self._mk = open(self.markers_path, "w", encoding="utf-8", newline="\n")
            self._mk.write(f"({wall:.6f}) {utc_iso(wall)} {note}".rstrip() + "\n")
            self._mk.flush()
            self.markers += 1
        self.event(f"MARK {utc_iso(wall)} {note}")

    def close(self) -> None:
        with self._lock:
            for fh in (self._log, self._ev, self._mk):
                if fh:
                    fh.close()
            self._log = self._ev = self._mk = None


# --------------------------------------------------------------------------------------
# monitor
# --------------------------------------------------------------------------------------


def parse_id_list(text: Optional[str]) -> set[int]:
    return {int(x, 16) for x in re.split(r"[ ,;]+", text or "") if x}


def format_frame_line(f: Frame) -> tuple[str, str]:
    meaning = f.meaning
    direction = "TX" if f.tx else "RX"
    text = (f"{utc_iso(f.wall)[11:26]} {direction} {f.id_str:>8} [{f.dlc}] {f.data_str:<23}  "
            f"{meaning or ('' if f.tx else 'unknown')}")
    if f.reason:
        text += f"  [{f.reason}]"
    style = "magenta" if f.tx else ("" if meaning else "bold yellow")
    return text, style


def cmd_monitor(args) -> int:
    dev = Device(args.port)
    rec = Recorder(enabled=not args.no_log)
    include, exclude = parse_id_list(args.id), parse_id_list(args.exclude)
    display: "queue.Queue[Frame]" = queue.Queue(maxsize=5000)
    skipped = [0]
    last_data: dict[tuple[bool, int], tuple[int, ...]] = {}

    def on_frame(f: Frame) -> None:
        rec.frame(f)   # the log always gets everything
        if include and f.can_id not in include or f.can_id in exclude:
            return
        if args.changes:
            key = (f.tx, f.can_id)
            if last_data.get(key) == f.data:
                return
            last_data[key] = f.data
        try:
            display.put_nowait(f)
        except queue.Full:
            skipped[0] += 1

    def on_notify(line: str) -> None:
        rec.event(line)
        out(line, "bold red" if line.startswith(("#stop", "#busoff", "#error", "#txerr")) else "cyan")

    def on_marker(wall: float, boot: int, note: str) -> None:
        rec.marker(wall, note)
        out(f"{utc_iso(wall)[11:26]} ---- MARKER ---- {note}", "bold reverse blue")

    dev.on_notify(lambda l: None if l.startswith("#mark ") else on_notify(l))
    dev.on_marker(on_marker)
    try:
        st = dev.setup_text_stream(echo=True)
    except DeviceError as e:
        out(str(e), "bold red")
        dev.close()
        return 1
    dev.on_frame(on_frame)
    out(f"connected {args.port}: mode={st.get('mode')} bitrate={st.get('bitrate')} board={st.get('board')} "
        f"fw={st.get('fw')} clock={st.get('clock')} (times in UTC)", "green")
    if rec.path:
        out(f"logging to {rec.path}", "green")
        rec.event(f"monitor start {args.port} status: {' '.join(f'{k}={v}' for k, v in st.items())}")
    out("type #commands, frames (481#124655), 'm' or 'm <note>' for a marker, 'quit' to exit", "dim")

    stop = threading.Event()

    def printer() -> None:
        while not stop.is_set():
            try:
                f = display.get(timeout=0.2)
            except queue.Empty:
                continue
            if skipped[0]:
                out(f"... {skipped[0]} frames not shown (screen too slow), all are in the log", "dim")
                skipped[0] = 0
            text, style = format_frame_line(f)
            out(text, style)

    threading.Thread(target=printer, daemon=True).start()

    pending_mark: list[Optional[int]] = [None]   # device time of an "m" press, waiting for its note

    def send_mark(at_us: Optional[int], note: str) -> None:
        at = f"@{at_us} " if at_us else ""
        try:
            dev.command(f"#mark {at}{note}".rstrip(), timeout=5.0, check=True)
        except DeviceError as e:
            out(f"marker not saved: {e}", "red")

    def handle_input(line: str) -> bool:
        if pending_mark[0] is not None:   # this line is the note for the marker set before
            at, pending_mark[0] = pending_mark[0], None
            send_mark(at, line.strip())
            return True
        line = line.strip()
        if not line:
            return True
        if line.lower() in ("q", "quit", "exit"):
            return False
        if line.lower() == "m":
            pending_mark[0] = dev.dev_now_us() or 0
            out("marker time taken - type a note and Enter (or just Enter for none)", "blue")
            return True
        if line.lower().startswith("m "):
            send_mark(dev.dev_now_us(), line[2:].strip())
            return True
        cmds = [frame_command(c) for c in split_commands(line)]   # "#a; #b" runs both, in order
        for i, c in enumerate(cmds):
            if not c.startswith("#"):
                out(f"not a command: {c!r} - enter #commands (try #help) or frames like 481#124655", "yellow")
                return True
            rec.event(f"> {c}")
            try:
                replies = dev.command(c, timeout=5.0)
            except DeviceError as e:
                out(str(e), "red")
                replies = ["#err " + str(e)]
            for r in replies:
                rec.event(r)
                out(r, "red" if r.startswith("#err") else "green")
            if replies[-1].startswith("#err") and i < len(cmds) - 1:
                out(f"stopped after error, {len(cmds) - 1 - i} command(s) not run", "red")
                break
        return True

    try:
        try:
            from prompt_toolkit import PromptSession
            from prompt_toolkit.patch_stdout import patch_stdout

            session = PromptSession()
            with patch_stdout(raw=True):
                while handle_input(session.prompt("can> ")):
                    pass
        except ImportError:
            while handle_input(input()):
                pass
    except (KeyboardInterrupt, EOFError):
        pass
    finally:
        stop.set()
        rec.event("monitor end")
        rec.close()
        dev.close()
        if rec.path:
            out(f"{rec.count} frames written to {rec.path}", "green")
            if rec.markers:
                out(f"{rec.markers} markers written to {rec.markers_path}", "green")
    return 0


# --------------------------------------------------------------------------------------
# script
# --------------------------------------------------------------------------------------


def parse_script_args(items: Sequence[str]) -> dict:
    """['start=312', 'period=0x10', 'dry=true', 'name=abc'] -> {'start': 312, 'period': 16, ...}"""
    out_: dict = {}
    for it in items:
        if "=" not in it:
            raise SystemExit(f"script argument must be key=value, got {it!r}")
        k, v = it.split("=", 1)
        low = v.lower()
        if low in ("true", "yes", "on"):
            val: object = True
        elif low in ("false", "no", "off"):
            val = False
        else:
            try:
                val = int(v, 0)
            except ValueError:
                try:
                    val = float(v)
                except ValueError:
                    val = v
        out_[k] = val
    return out_


class PersistentState(dict):
    """dict that is saved to a JSON file on every change -> scripts can resume after a stop/crash.
    Stored in host/scripts/state/<script>.json."""

    def __init__(self, path: Path):
        super().__init__()
        self.path = path
        if path.exists():
            import json
            try:
                self.update(json.loads(path.read_text(encoding="utf-8")))
            except ValueError:
                pass

    def save(self) -> None:
        import json
        self.path.parent.mkdir(parents=True, exist_ok=True)
        tmp = self.path.with_suffix(".tmp")
        tmp.write_text(json.dumps(self, indent=1), encoding="utf-8")
        tmp.replace(self.path)

    def __setitem__(self, k, v) -> None:
        super().__setitem__(k, v)
        self.save()

    def clear(self) -> None:
        super().clear()
        self.save()


class ScriptApi:
    """Functions available inside test scripts (injected as globals). See host/SCRIPTING.md."""

    # commands that can safely be sent again after a dropout (state-setting, not "do something")
    _IDEMPOTENT = ("#mode", "#rule", "#status", "#stats", "#time", "#pins", "#wifi", "#echo", "#format", "#limit")

    def __init__(self, dev: Device, rec: Recorder, verbose: bool, script: Path, args: dict):
        self.dev = dev
        self.rec = rec
        self.verbose = verbose
        self.script = script
        self.args = args
        self.state = PersistentState(script.parent / "state" / f"{script.stem}.json")
        self.results: list[tuple[str, bool, str]] = []
        self.last_send_us: Optional[int] = None
        self._frames: "queue.Queue[Frame]" = queue.Queue()
        self._csv: dict[str, tuple[Path, list[str]]] = {}
        self._latched: Optional[str] = None
        dev.on_frame(lambda f: None if f.tx else self._frames.put(f))
        dev.on_notify(self._on_notify)
        dev.abort_check = self._check_latch

    # ---- STOP latch: any latched stop ends the script immediately ----
    def _on_notify(self, line: str) -> None:
        if line.startswith("#stop") and "LATCHED" in line:
            self._latched = line

    def _check_latch(self) -> None:
        if self._latched:
            raise ScriptStopped(f"STOP latched on the device ({self._latched}). The script ends without "
                                f"touching mode or rules; release it with #resume (web UI: Resume).")

    def log(self, msg: str) -> None:
        self.rec.event(f"LOG {msg}")
        out(f"[script] {msg}", "cyan")

    def status(self, text: str) -> None:
        """Progress line (console + events file)."""
        self.rec.event(f"STATUS {text}")
        out(f"[status] {text}", "bold cyan")

    def cmd(self, text: str, check: bool = True) -> list[str]:
        """Firmware command. Waits through short dropouts; state-setting commands are retried once.
        Raises ScriptStopped if the STOP latch is set."""
        self._check_latch()
        self.rec.event(f"> {text}")
        try:
            lines = self.dev.command(text, timeout=5.0)
        except DeviceError as e:
            if not text.startswith(self._IDEMPOTENT) or not self.dev.wait_ready():
                raise
            self.rec.event(f"retry after dropout ({e})")
            lines = self.dev.command(text, timeout=5.0)
        for l in lines:
            self.rec.event(l)
        if self.verbose:
            for l in lines:
                out(l, "dim")
        if "STOP latched" in lines[-1]:
            self._latched = self._latched or lines[-1]
            self._check_latch()
        if check and lines[-1].startswith("#err"):
            raise DeviceError(f"{text}: {lines[-1]}")
        return lines

    def mode(self, m: str) -> None:
        self.cmd(f"#mode {m}")
        self.log(f"mode {m}")

    def rule(self, text: str) -> list[str]:
        return self.cmd(f"#rule {text}")

    def send(self, frame: "str | int", data: "str | Sequence[int] | None" = None) -> None:
        """send('481#110027') / send(0x481, '11 00 27') / send(0x481, [0x11, 0, 0x27])"""
        if isinstance(frame, int):
            if isinstance(data, str):
                payload = data.replace(" ", "")
            else:
                payload = "".join(f"{b:02X}" for b in (data or []))
            frame = f"{frame:03X}#{payload}"
        self.last_send_us = self.dev.dev_now_us()
        self.cmd(f"#send {frame}")

    def wait_for(self, can_id: Optional[int] = None, data=None, timeout: float = 1.0,
                 since_us: Optional[int] = None) -> Optional[Frame]:
        """Waits for a received frame (ID, data prefix like '00 01 ??'). By default frames since the last
        send() count, so a fast answer is not missed. Dropouts do not count towards the timeout."""
        since = since_us if since_us is not None else self.last_send_us
        f = self.dev.wait_for(can_id, data, timeout, since_us=since)
        desc = f"{can_id:03X} {data or ''}".strip() if can_id is not None else str(data)
        if f:
            self.rec.event(f"wait_for {desc}: got {f.id_str}#{f.data_str} ({f.meaning or 'unknown'})")
        else:
            self.rec.event(f"wait_for {desc}: TIMEOUT after {timeout}s (connected time)")
        return f

    def collect(self, seconds: float, can_id: Optional[int] = None, data=None) -> list[Frame]:
        """Received frames during the next `seconds` of DEVICE time (dropouts are filled by the replay)."""
        start = self.dev.dev_now_us() or 0
        self.dev.sleep_connected(seconds)
        pat = make_pattern(data)
        return [f for f in self.dev.frames_between(start, start + int(seconds * 1e6)) if matches(f, can_id, pat)]

    def next_frame(self, timeout: float = 1.0) -> Optional[Frame]:
        """Next received frame in arrival order (frames recovered by a replay included), or None.
        Use drain() first to skip what arrived before. Dropouts do not count towards the timeout."""
        d0, t0 = self.dev.downtime(), time.monotonic()
        while True:
            self._check_latch()
            try:
                return self._frames.get(timeout=0.1)
            except queue.Empty:
                pass
            left = timeout - (time.monotonic() - t0 - (self.dev.downtime() - d0))
            if left <= 0 and self.dev._ready.is_set():
                return None

    def drain(self) -> int:
        """Discards frames queued for next_frame(). Returns how many."""
        n = 0
        while True:
            try:
                self._frames.get_nowait()
                n += 1
            except queue.Empty:
                return n

    def mark(self, note: str = "") -> None:
        """Marker at this moment, written to the .markers.txt (same as the web UI button)."""
        at = self.dev.dev_now_us()
        self.cmd(f"#mark {'@%d ' % at if at else ''}{note}".rstrip())

    def sleep(self, s: float) -> None:
        """Sleeps s seconds of connected time (a dropout extends it)."""
        self.dev.sleep_connected(s)

    def now(self) -> float:
        return time.monotonic()

    def now_us(self) -> Optional[int]:
        """Current device time (µs since boot), same time base as frame.dev_us."""
        return self.dev.dev_now_us()

    def csv(self, name: str, **fields) -> Path:
        """Appends one row to Messungen/<log name>_<name>.csv (header from the first call's field names)."""
        import csv as _csv
        if name not in self._csv:
            base = self.rec.path or (LOG_DIR / f"can_{utc_stamp()}")
            p = base.with_name(f"{base.stem}_{name}.csv")
            self._csv[name] = (p, list(fields.keys()))
            p.parent.mkdir(parents=True, exist_ok=True)
            with open(p, "w", newline="", encoding="utf-8") as fh:
                _csv.writer(fh).writerow(self._csv[name][1])
        p, cols = self._csv[name]
        with open(p, "a", newline="", encoding="utf-8") as fh:
            _csv.writer(fh).writerow([fields.get(c, "") for c in cols])
        return p

    def result(self, name: str, ok: bool, details: str = "") -> None:
        self.results.append((name, bool(ok), details))
        self.rec.event(f"RESULT {name}: {'OK' if ok else 'FAIL'} {details}")
        out(f"[result] {name}: {'OK' if ok else 'FAIL'} {details}", "bold green" if ok else "bold red")

    def ask(self, question: str) -> str:
        """Ask the operator (e.g. 'Are the yellow drinks active now? (y/n)')."""
        ans = input(f"[script] {question} ").strip()
        self.rec.event(f"ASK {question} -> {ans}")
        return ans

    def intervals(self, frames: Iterable[Frame]) -> list[float]:
        fs = list(frames)
        return [(b.dev_us - a.dev_us) / 1000 for a, b in zip(fs, fs[1:])]

    def globals(self) -> dict:
        g = {name: getattr(self, name) for name in
             ("log", "status", "cmd", "mode", "rule", "send", "wait_for", "collect", "next_frame", "drain",
              "sleep", "now", "now_us", "result", "ask", "intervals", "mark", "csv")}
        g.update(dev=self.dev, decode=decode, Frame=Frame, args=self.args, state=self.state,
                 make_pattern=make_pattern, matches=matches, utc_iso=utc_iso)
        return g


def cmd_script(args) -> int:
    path = Path(args.file)
    if not path.exists():
        out(f"script not found: {path}", "bold red")
        return 1
    dev = Device(args.port)
    rec = Recorder(tag=f"_{path.stem}")
    dev.on_frame(rec.frame)
    dev.on_notify(lambda l: None if l.startswith("#mark ") else
                  (rec.event(l), out(l, "bold red" if l.startswith(("#stop", "#busoff")) else "cyan")))
    dev.on_marker(lambda wall, boot, note: (rec.marker(wall, note),
                                            out(f"[marker] {utc_iso(wall)} {note}", "bold blue")))
    api = ScriptApi(dev, rec, args.verbose, path, parse_script_args(args.script_args))
    rc = 0
    try:
        st = dev.setup_text_stream(echo=True)
        out(f"connected {args.port}: mode={st.get('mode')} fw={st.get('fw')} clock={st.get('clock')}; "
            f"log {rec.path}", "green")
        rec.event(f"script {path} start, args {api.args}, status: {' '.join(f'{k}={v}' for k, v in st.items())}")
        if st.get("stop_latch") == "1":
            raise ScriptStopped(f"STOP is latched on the device (stops={st.get('stops')}). Not starting. "
                                f"Release it with #resume (web UI: Resume), then run the script again.")
        runpy.run_path(str(path), init_globals=api.globals(), run_name="__main__")
    except ScriptStopped as e:
        out(f"script stopped: {e}", "bold red")
        rec.event(f"STOPPED (latch): {e}")
        rc = 2
    except KeyboardInterrupt:
        out("script aborted", "bold red")
        rec.event("ABORTED by user")
        rc = 2
    except SystemExit as e:   # exit() / sys.exit() in the script = normal end
        rc = e.code if isinstance(e.code, int) else 0
    except Exception as e:  # report script errors (with the script line), but still clean up
        import traceback
        tb = [fr for fr in traceback.extract_tb(e.__traceback__) if Path(fr.filename).resolve() == path.resolve()]
        where = f" (line {tb[-1].lineno}: {tb[-1].line})" if tb else ""
        out(f"script error: {type(e).__name__}: {e}{where}", "bold red")
        rec.event(f"ERROR {type(e).__name__}: {e}{where}\n{traceback.format_exc()}")
        rc = 1
    finally:
        if not args.keep_active:
            try:
                # own cleanup: LISTEN + rules off, but without setting the STOP latch
                dev.command("#stop nolatch", timeout=2.0)
                rec.event("cleanup: #stop nolatch (LISTEN, rules disabled)")
            except DeviceError:
                pass
        ok = sum(1 for _, k, _ in api.results if k)
        out(f"results: {ok}/{len(api.results)} OK; {rec.count} frames in {rec.path}", "bold")
        for name, k, details in api.results:
            out(f"  {'OK  ' if k else 'FAIL'} {name} {details}", "green" if k else "red")
        rec.event(f"script end rc={rc}")
        rec.close()
        dev.close()
    if rc == 0 and any(not k for _, k, _ in api.results):
        rc = 3
    return rc


def cmd_cmd(args) -> int:
    dev = Device(args.port)
    try:
        for c in (x for arg in args.commands for x in split_commands(arg)):
            lines = dev.command(frame_command(c), timeout=5.0)
            for line in lines:
                out(line, "red" if line.startswith("#err") else "")
            if lines[-1].startswith("#err"):
                return 1   # stop at the first error
    except DeviceError as e:
        out(str(e), "bold red")
        return 1
    finally:
        dev.close()
    return 0


# --------------------------------------------------------------------------------------
# decode
# --------------------------------------------------------------------------------------

CANDUMP_RE = re.compile(r"^\s*\((\d+(?:\.\d+)?)\)\s+(\S+)\s+([0-9A-Fa-f]{1,8})#(R\d?|[0-9A-Fa-f]*)\s*$")


def read_log(path: Path) -> Iterable[Frame]:
    """candump log (own parser); other formats (.asc, .blf, .trc, ...) via python-can if installed."""
    if path.suffix.lower() in (".log", ".txt", ".candump"):
        with open(path, encoding="ascii", errors="replace") as fh:
            for line in fh:
                m = CANDUMP_RE.match(line)
                if not m:
                    continue
                ts, _, id_s, payload = m.groups()
                rtr = payload.startswith("R")
                data = () if rtr else tuple(int(payload[i : i + 2], 16) for i in range(0, len(payload), 2))
                wall = float(ts)
                yield Frame(dev_us=int(wall * 1e6), wall=wall, can_id=int(id_s, 16), data=data, ext=len(id_s) > 3,
                            rtr=rtr, dlc=len(data))
        return
    try:
        import can
    except ImportError:
        raise SystemExit("python-can is needed for this log format: py -m pip install python-can")
    for msg in can.LogReader(str(path)):
        data = tuple(msg.data)
        yield Frame(dev_us=int(msg.timestamp * 1e6), wall=msg.timestamp, can_id=msg.arbitration_id, data=data,
                    ext=msg.is_extended_id, rtr=msg.is_remote_frame, dlc=msg.dlc)


MARKER_RE = re.compile(r"^\s*\((\d+(?:\.\d+)?)\)\s+\S+\s?(.*)$")


def read_markers(log_path: Path) -> list[tuple[float, str]]:
    """Companion file <log>.markers.txt: "(epoch) ISO-UTC note" per line."""
    p = log_path.with_suffix(".markers.txt")
    if not p.exists():
        return []
    marks = []
    with open(p, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            m = MARKER_RE.match(line.rstrip("\n"))
            if m:
                marks.append((float(m.group(1)), m.group(2).strip()))
    return sorted(marks)


def cmd_decode(args) -> int:
    path = Path(args.file)
    include, exclude = parse_id_list(args.id), parse_id_list(args.exclude)
    per_id: dict[int, list[float]] = collections.defaultdict(list)
    per_type: dict[str, list[float]] = collections.defaultdict(list)
    marks = read_markers(path)
    mi = 0
    t0 = None
    n = 0

    def print_marks_until(t: float) -> None:
        nonlocal mi
        while mi < len(marks) and marks[mi][0] <= t:
            wall, note = marks[mi]
            out(f"{utc_iso(wall)[11:26]} {wall - (t0 or wall):10.6f} ---- MARKER ---- {note}", "bold reverse blue")
            mi += 1

    for f in read_log(path):
        if include and f.can_id not in include or f.can_id in exclude:
            continue
        n += 1
        if t0 is None:
            t0 = f.wall
        per_id[f.can_id].append(f.wall)
        per_type[frame_type(f.can_id, f.data)].append(f.wall)
        if not args.stats_only:
            print_marks_until(f.wall)
            meaning = f.meaning
            out(f"{utc_iso(f.wall)[11:26]} {f.wall - t0:10.6f} {f.id_str:>8} [{f.dlc}] {f.data_str:<23}  "
                f"{meaning or 'unknown'}", "" if meaning else "bold yellow")
    if not args.stats_only:
        print_marks_until(float("inf"))
    if n == 0:
        out("no frames", "yellow")
        return 0
    if marks:
        out(f"\n{len(marks)} markers (UTC)", "bold")
        for wall, note in marks:
            out(f"  {utc_iso(wall)}  +{wall - t0:10.3f} s  {note}")

    def table(title: str, groups: dict) -> None:
        out("")
        out(title, "bold")
        out(f"{'':<22} {'count':>8} {'avg ms':>10} {'min ms':>10} {'max ms':>10}", "bold")
        for key in sorted(groups, key=lambda k: (str(k))):
            ts = groups[key]
            iv = [(b - a) * 1000 for a, b in zip(ts, ts[1:])]
            label = f"{key:03X}" if isinstance(key, int) else key
            if iv:
                out(f"{label:<22} {len(ts):>8} {sum(iv) / len(iv):>10.2f} {min(iv):>10.2f} {max(iv):>10.2f}")
            else:
                out(f"{label:<22} {len(ts):>8} {'-':>10} {'-':>10} {'-':>10}")

    duration = per_id and max(max(v) for v in per_id.values()) - t0
    out(f"\n{n} frames, {duration:.3f} s, {len(per_id)} IDs  ({path})", "bold")
    table("per ID", per_id)
    table("per frame type", per_type)
    return 0


# --------------------------------------------------------------------------------------


def main(argv: Optional[list[str]] = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    if argv and argv[0] == "decode":
        p = argparse.ArgumentParser(prog="franke_can.py decode")
        p.add_argument("file")
        p.add_argument("--id", help="only these IDs (hex, comma separated)")
        p.add_argument("--exclude", help="skip these IDs")
        p.add_argument("--stats-only", action="store_true")
        return cmd_decode(p.parse_args(argv[1:]))

    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("port", help="COM7, /dev/ttyUSB0 or socket://franke-can.local:3333")
    sub = p.add_subparsers(dest="action", required=True)
    m = sub.add_parser("monitor", help="live view + log")
    m.add_argument("--id", help="show only these IDs (hex, comma separated); the log gets everything")
    m.add_argument("--exclude", help="hide these IDs, e.g. 181")
    m.add_argument("--changes", action="store_true", help="show a frame only if its data changed")
    m.add_argument("--no-log", action="store_true")
    s = sub.add_parser("script", help="run a test script")
    s.add_argument("file")
    s.add_argument("script_args", nargs="*", metavar="key=value", help="arguments for the script (args dict)")
    s.add_argument("--keep-active", action="store_true", help="do not send #stop when the script ends")
    s.add_argument("-v", "--verbose", action="store_true")
    c = sub.add_parser("cmd", help="send firmware command(s) and print the reply")
    c.add_argument("commands", nargs="+")
    args = p.parse_args(argv)
    try:
        return {"monitor": cmd_monitor, "script": cmd_script, "cmd": cmd_cmd}[args.action](args)
    except serial.SerialException as e:  # type: ignore[union-attr]
        out(f"cannot open {args.port}: {e}", "bold red")
        return 1


if __name__ == "__main__":
    sys.exit(main())
