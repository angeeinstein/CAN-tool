# Writing and running CAN test scripts (Franke FM850)

Instructions for an agent that writes and runs automated tests against the coffee machine's CAN bus.
Read this file completely before writing or running a script.

## 1. Setup in one paragraph

An ESP32 with a CAN transceiver sits on the bus of a Franke FM850 coffee machine (125 kbit/s, 11-bit IDs).
Its firmware streams every frame to clients and can send frames. The firmware never sends anything on its own.
Test scripts are **plain Python files that run on the PC** (`host/scripts/*.py`). They are started by
`host/franke_can.py`, which connects to the ESP32, records everything, and injects the script API as globals.
**Time-critical replies (≤ a few ms) are done by firmware rules**: a script only *configures* rules; it must
never try to answer frames itself with `send()` and expect exact timing (Wi-Fi latency is 5–100 ms).

## 2. Running a script

From the `host/` folder (Windows, Python 3.11):

```
py franke_can.py socket://192.168.29.195:3333 script scripts/<name>.py [key=value ...] [--keep-active] [-v]
py franke_can.py COM17 script scripts/<name>.py ...          # if the ESP32 is on USB
```

- `key=value` arguments arrive in the dict `args` (values converted: `12` → int, `0x10` → int, `1.5` → float,
  `true/false` → bool, everything else str).
- At the end (normal, error or Ctrl+C) the tool sends **`#stop nolatch`**: bus to LISTEN, all rules disabled
  (without setting the STOP latch, so the next script can run).
  `--keep-active` skips that (e.g. to leave a found reply rule running for the human).
- `-v` prints every firmware reply.
- Only **one TCP client** is allowed at a time. While a script runs, the human can still watch in the web UI
  (http://192.168.29.195), but not with a second `franke_can.py`.
- Exit codes: 0 ok, 1 script error, 2 stopped (STOP latch, see rule 6) or aborted (Ctrl+C),
  3 at least one `result(..., False)`.
- Unit tests for host-side logic: `py -m unittest discover tests` (from `host/`).

Output, all in `Messungen/` at the project root, named `can_<UTC time>Z_<script>.*`:

| file | content |
|---|---|
| `.log` | every frame, candump format `(epoch.usec) can0 ID#DATA` (own frames included) |
| `.markers.txt` | markers: `(epoch.usec) ISO-UTC text`, same timestamps as the `.log` |
| `.events.txt` | commands, replies, `log()`, `status()`, `result()`, notifications, errors with traceback |
| `_<name>.csv` | rows written by `csv(name, ...)` |

Analyse a log: `py franke_can.py decode ../Messungen/<file>.log [--id 481] [--stats-only]`
(decodes known frames, merges the markers, statistics per ID / frame type with intervals).

## 3. Safety rules (must follow)

1. After every boot the bus is in **LISTEN**: no ACK, no transmit. Sending and rules need `mode("active")`.
   Switch to ACTIVE only when the test needs it, and only after the human agreed to the test.
2. **TX rate limit 50 frames/s.** Exceeding it triggers an emergency stop (LISTEN, rules off) and the script's
   next `send`/`rule` fails. Design tests far below that (the machine polls every few seconds).
3. **Do not answer `481 11 00 21`** (availability check to the payment node, see section 4) with experimental
   data unless the human explicitly asks for it: a wrong answer could approve a purchase or change billing
   counters. Experiments on the poll `481 11 00 27` are fine.
4. Never change the CAN pins, bitrate, Wi-Fi settings or flash firmware from a script.
5. Do not send frames with IDs of real nodes (0x401, 0x446, 0x481, 0x4C6, 0x181 …) unless the test is about
   exactly that and the human agreed. The emulated payment system is node 0x00: it sends on **0x480** and
   acknowledges on **0x400**.
6. **STOP latch.** STOP in the web UI, `#stop`, the BOOT key and the TX rate limit switch the bus to LISTEN,
   disable all rules and set a latched flag on the device (kept across reboots, counter `stops`). While it is
   set, the firmware refuses `#mode active`, `#send`, `#rule add/enable/load` and SLCAN `O`/`t`, and a running
   script ends **immediately** (also in the middle of `wait_for`/`collect`/`sleep`/`next_frame`) with
   `ScriptStopped`, exit code 2 and a clear message; a script does not start at all while it is set
   (`#status` shows `stop_latch=1 stops=N`). Only the human releases it: `#resume` (any link, also the one a
   script used) or the Resume button in the web UI header. **Never send `#resume` from a script.**
   A script's own cleanup uses `#stop nolatch` (LISTEN + rules off without latching); never send a plain
   `#stop` from a script unless you intend to block further tests until the human resumes.

## 4. The bus (what is known)

Node 0x01 = coffee machine, 0x46 = milk controller, 0x00 = payment system (emulated by us).
Node n sends on 0x480+n and acknowledges on 0x400+n. Reply code = request code + 1.

| ID | data | meaning |
|---|---|---|
| 481 | `12 46 55` | status poll → milk controller, every 2.05 s (0.51 s when active) |
| 4C6 | `00 01 56 SS PP 02 00 TT` | status reply; SS PP `01 00` pump off, `02 01` running; TT temperature °C |
| 481 | `12 46 57 FF EE WW` | command: function FF, EE 01 on / 00 off, WW value |
| 4C6 | `00 01 58 TT 02 00 00 00` | command acknowledged |
| 481 | `12 46 51 06 08 00 00 00` / 4C6 `00 01 52 00 14 00 01 00` | query 0x51 and reply (meaning unknown) |
| 481 | `12 46 00 00 00 00 00 00` | init/reset milk controller (assumed) |
| 446 | `00 01 09 00 00 00 00 00` | receipt ack from the milk controller |
| 401 | `00 46 09` / `00 00 09` | receipt ack from the coffee machine (to node 46 / node 00) |
| 481 | `11 00 27` | **poll → payment system**, every 5.12 s (only if billing = CAN); the 5.12 s timer restarts after a `11 00 21` |
| 481 | `11 00 21` | **once-per-minute availability check** (state 181, 21 to payment node, 51 query to milk controller, outputs reset); also sent when the screen changes |
| 181 | `00 00 03` / `00 00 01` | machine state (03 → 01 when made ready) |

`known_frames.decode(id, data)` returns the meaning (or None); `frame.meaning` uses it.

## 5. Script API (globals inside a script)

Time base: every frame has `dev_us` = **device time in µs since ESP32 boot** (exact, from the CAN receive
moment) and `wall` = UTC epoch seconds. Use `dev_us` for intervals. All waiting functions count only
**connected time**: if Wi-Fi drops, the tool reconnects (every 2 s), fetches the missed frames from the
device's replay buffer (last 512 frames) and the waits continue, so a short dropout does not produce false
timeouts or false "no reaction" results. Commands wait up to 120 s for the connection to come back and are
retried once after a reconnect (except `#send` / `#resume`, which could act twice). `mark()` never ends the
script if it fails - it only warns. If a dropout is longer than the device's replay buffer (a **DATA GAP**),
the missed frames are gone: `gap()` returns True once, and interval/timing checks over that window are not
trustworthy - treat the current step as *inconclusive* rather than a hit (see `sweep_reply.py`).

**Firmware ACTIVE watchdog**: if the TCP/USB or web client that switched the bus to ACTIVE disconnects and
does not come back within `#watchdog` seconds (default 30, `0` = off), the firmware itself reverts to LISTEN
and disables all rules (not latched). So even if a script's PC crashes mid-test, the machine stops being
driven. After a longer outage a reconnecting script may therefore find the device back in LISTEN - check
`#status` (`mode=`, `watchdog=`) and re-arm before continuing.

**Frame object** (`Frame`): `can_id`, `data` (tuple of ints), `dlc`, `ext`, `rtr`, `tx` (True = sent by us),
`reason` (rule name / "manual" for own frames), `dev_us`, `wall`, `id_str` ("481"), `data_str` ("11 00 27"),
`meaning`.

**Data patterns** (used by `wait_for`, `collect`, `make_pattern`): prefix match, `"11 00 27"`, `"11 ?? 2?"`
(`??` any byte, `?` any nibble), `"110027"`, or a list `[0x11, None, 0x27]`. Frame must have at least as many
bytes as the pattern.

| function | description |
|---|---|
| `mode("listen"\|"active"\|"closed")` | set bus mode |
| `rule("add <name> <definition>")`, `rule("del <name>")`, `rule("enable <name>")`, `rule("disable <name>")`, `rule("list")`, `rule("clear")` | firmware rules, see section 6 |
| `send("481#110027")`, `send(0x480, "00 01 28")`, `send(0x480, [0,1,0x28])` | send one frame now (ACTIVE only) |
| `wait_for(id, data=None, timeout=1.0, since_us=None)` | first received frame matching id/data with `dev_us >= since_us`. Default `since_us` = time of the last `send()` (so a fast answer is not missed), else now. Returns `Frame` or `None` |
| `collect(seconds, id=None, data=None)` | waits `seconds` of device time, returns all received frames in that window that match |
| `next_frame(timeout=1.0)` | next received frame in arrival order (stream processing), or `None`. Call `drain()` first to skip old ones |
| `drain()` | discard frames queued for `next_frame()` |
| `gap()` | True once if an unrecoverable DATA GAP happened since the last call (frames lost beyond the replay buffer); use it to mark a step inconclusive after a long dropout |
| `intervals(frames)` | list of intervals in ms between consecutive frames (by `dev_us`) |
| `now_us()` | current device time (µs since boot), same base as `frame.dev_us` |
| `sleep(s)` | sleep `s` seconds of connected time |
| `mark("text")` | marker at this moment → `.markers.txt` (+ shown in the web UI live view) |
| `log("text")`, `status("text")` | console + events file (`status` = progress line) |
| `result(name, ok, details="")` | test result; summary at the end; a failed one sets exit code 3 |
| `csv(name, **fields)` | append a row to `Messungen/<log>_<name>.csv` (header from the first call) |
| `ask("question")` | asks the human on the console (blocks!) — only for supervised tests |
| `cmd("#...")` | raw firmware command, returns reply lines; raises `DeviceError` on `#err` (use `check=False` to get the lines instead) |
| `args` | dict of `key=value` command-line arguments |
| `state` | dict persisted to `host/scripts/state/<script>.json` on every assignment → resume after stop/crash |
| `exit()` | end the script normally |
| also available | `dev` (Device), `decode`, `Frame`, `make_pattern`, `matches(frame, id, pattern)`, `utc_iso(epoch)` |

Useful raw commands for `cmd()`: `#status`, `#stats` (counters incl. lost frames, error counters,
`heap_free`), `#health` (heap, rx rate, drop counters, task stack headroom, reset reason), `#time`,
`#rule list`. Do not use `#reboot`, `#pins`, `#wifi`, `#bitrate`, `#limit`, `#watchdog`, `#selftest`.

The device sends `#warn dropped: …` if it has to drop frames/notifications under load, and `#health …`
periodically while the bus is busy — both arrive as ordinary notifications (logged, not command replies).

## 6. Firmware rules (real-time replies)

```
#rule add <name> match=<id>[/<mask>][:<bytes>] send=<id>:<bytes>[@<ms>] [send=...] [dlc=<n>] [once]
```
- `match` bytes are a **prefix**; `??` = any byte, `1?` = any low nibble; `/mask` masks the ID.
- `send` bytes may contain `{b0}`…`{b7}` = byte copied from the trigger frame; `@ms` = delay 0–5000 ms after
  the trigger. Up to 6 `send=` per rule, up to 16 rules. `once` = fire only once.
- `add` with an existing name **replaces** the rule (that is how a sweep swaps candidates).
- Rules act only in ACTIVE. Reaction < 1 ms + delay. Their frames appear in the log as TX with the rule name.

Example (hypothesis for the payment emulation):
```python
rule("add poll match=481:11 00 27 send=400:00 01 09 00 00 00 00 00@1 send=480:00 01 28@3")
```

## 7. Script template

```python
"""What this test checks, how to run it, which args it takes."""
PERIOD = float(args.get("period", 5.12))

mode("listen")
base = collect(12, 0x481, "11 00 27")                    # passive baseline first
result("poll present", len(base) >= 2, f"intervals ms {intervals(base)}")

mode("active")                                             # only if the test needs it
rule("add t1 match=481:11 00 27 send=400:00 01 09 00 00 00 00 00@1 send=480:00 01 28@3")
mark("t1 active")
seen = collect(30)                                         # everything during 30 s
new_ids = sorted({f.id_str for f in seen} - {"481", "4C6", "446", "401", "181"})
csv("t1", variant="00 01 28", frames=len(seen), new_ids=" ".join(new_ids))
result("t1 reaction", bool(new_ids), f"new IDs {new_ids}")
rule("del t1")
```
Guidelines: start with a passive baseline; one `mark()` per step so the human can match the log with
observations; write every tested variant to `csv()`; keep progress in `state` for long runs; keep the
device-specific constants at the top of the script; prefer rules over `send()` for anything timed.

## 8. Existing scripts

- `scripts/test_kasse.py` – baseline, ACTIVE without rules, then a few reply variants for the poll; asks the
  human about the display after each (supervised).
- `scripts/sweep_reply.py` – generic reply sweep for the poll `481 11 00 27`: ACK on 0x400 @1 ms + candidate on
  0x480 @3 ms via the rule "sweep", template `B0 01 28 B3 00 00 00 00` (B0 ∈ {00,01,10,11}, B3 ∈ 00..FF) plus
  3-byte variants = 1028 candidates, 2 polls each (~2.9 h). Hits: poll interval outside 4.5–6.0 s or no poll for
  > 6.0 s (measured from the last `11 00 27` or `11 00 21`, whichever is later; not checked for 3 s after a milk
  command `12 46 57`; logic in `host/sweep_logic.py`, tests in `host/tests/`), a frame not in the known list,
  or a new command byte on 0x481. Writes `_sweep.csv`, a marker per candidate, progress in
  `state`. Args: `start=<n>` (1-based), `resume=1`, `limit=<n>`, `per=<triggers>`, `pause_on_hit=0`, `list=1`.
  On a hit it asks the human (continue / stop); use `pause_on_hit=0` for unattended runs.
  Note: the bus cannot show **display** changes (e.g. "yellow drinks active"); the human should film the
  display, the markers allow matching video time and candidate.
