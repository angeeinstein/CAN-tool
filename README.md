# Franke FM850 CAN sniffer & payment-system emulator (ESP32)

An ESP32 with an SN65HVD230 transceiver sits on the coffee machine's CAN bus (125 kbit/s, CAN 2.0A) and

1. **listens**: streams every frame with a µs timestamp to the PC or browser, without touching the bus (LISTEN = no ACK, no TX),
2. **takes part**: in ACTIVE mode it sends frames and answers frames within milliseconds using rules that run in the firmware (payment-system emulation).

Everything is controlled live over **USB**, **TCP** (port 3333) or the **web interface**: mode, rules, sending frames, firmware updates (OTA). You never need to reflash over USB once it is installed.

```
platformio.ini            envs: esp32dev, esp32-s3-devkitc-1, esp32-c3-devkitm-1 (ESP-IDF 5.2), native (unit tests)
CMakeLists.txt, src/CMakeLists.txt   ESP-IDF project / component (PlatformIO runs ESP-IDF's CMake build)
sdkconfig.defaults(.<target>)        ESP-IDF settings that differ from its defaults, each with the reason
partitions.csv            flash layout (same as before: NVS at 0x9000, 2 OTA slots, core dump)
src/idf_component.yml     components from the Espressif registry (mDNS, LED strip)
include/config.h          pins, defaults, limits, credential defaults
include/secrets.example.h copy to include/secrets.h (git-ignored)
lib/franke_core/          pure C++ (no ESP dependency): SLCAN codec, rule engine, rate limiter -> unit-tested
src/main.cpp              app_main + main task: executes commands, WLAN state machine, watchdogs, LED, BOOT button
src/sys.*                 time, NVS settings, heap figures, deferred reboot
src/can_bus.*             TWAI driver: modes, RX task, TX gate, bus-off recovery, statistics
src/host_io.*             non-blocking output to USB / TCP / browser, replay buffer
src/host_serial.*         USB link: UART0 (classic ESP32) or USB-Serial/JTAG (S3/C3)
src/commands.*            command queue + SLCAN / "#" command dispatcher
src/rules.*               rule table, action scheduler task, NVS save/load
src/net_wifi.cpp, net.h   WLAN (saved networks, scan, reconnect, enterprise, AP always on), mDNS
src/net_tcp.cpp           TCP server (port 3333, keepalive)
src/web.*                 web server, WebSocket sender, OTA upload
src/health.*              reset reason, crash dump report, load statistics, flood self-test
src/clock.*               NTP / host time
src/status_led.*          status LED (GPIO or RGB)
web/index.html            web UI (embedded gzipped into the firmware at build time)
tools/embed_web.py        build step: web/index.html -> src/generated/web_index_html.h
test/                     Unity tests for lib/franke_core (pio test -e native)
host/franke_can.py        PC tool: monitor, logging, test scripts, log decoding
host/known_frames.py      table of known frames (spec section 8)
host/scripts/test_kasse.py  payment-emulation test (spec section 9)
```

---

## 1. Hardware

| ESP32 | SN65HVD230 (ARCELI) |
|---|---|
| 3V3 | 3V3 |
| GND | GND (**also connect to machine GND**) |
| GPIO 13 (S3/C3: GPIO 5) | CTX |
| GPIO 27 (S3/C3: GPIO 4) | CRX |
| | CANH / CANL → the **inner two contacts** of the 4P4C/6P4C plug at the "HUB 1H 326 444" |

- **Remove the 120 Ω resistor R2** on the transceiver module. The bus is already terminated (JP3 on the power board and the milk module).
- On the classic ESP32, don't use strapping pins or pins that are active during boot for CTX/CRX (GPIO 0, 2, 5, 12, 14, 15): they glitch the bus at power-up or prevent booting (GPIO 12).
- You can change the pins in `include/config.h` (`CAN_TX_PIN`, `CAN_RX_PIN`, `STATUS_LED_PIN`, `ESTOP_BUTTON_PIN`).
- Status LED: short flash per received frame (LISTEN), solid with short dark flashes (ACTIVE), fast blinking = bus-off.
  On S3/C3 DevKits this is the RGB LED (green = LISTEN, red = ACTIVE, yellow = bus-off). On the classic DevKit it is GPIO 2.
- **BOOT button** (GPIO 0, C3: GPIO 9) = emergency stop.

### Installed in the machine
- The ESP32 then needs its own supply instead of laptop USB, e.g. a buck converter from the machine's supply voltage to 5 V (into the board's 5V/VIN pin). Machine GND, transceiver GND and ESP32 GND must be connected. **Check the voltage and pinout of the machine first.** The outer pins of the modular plug are still unidentified.
- Keep the antenna away from metal sheets if possible; check the RSSI in the web UI (`System` tab) before closing the machine.

## 2. Build & first flash (USB)

Requires VS Code + PlatformIO. The firmware is a native ESP-IDF 5.2 project (`framework = espidf`); the first
build downloads ESP-IDF's registry components (mDNS, LED strip) into `managed_components/` and takes a few minutes.

```
pio run -e esp32dev -t upload          # classic ESP32 DevKit (UART bridge, 921600 baud)
pio run -e esp32-s3-devkitc-1 -t upload
pio run -e esp32-c3-devkitm-1 -t upload
pio device monitor -e esp32dev         # type #help + Enter
```

S3/C3 use native USB (USB-Serial/JTAG), so connect the board's "USB" port. A USB upload writes bootloader,
partition table and app, and resets the OTA selection; the settings in NVS are kept.

**Coming from the Arduino firmware (fw 1.x): flash once over USB.** The ESP-IDF firmware needs its own bootloader
(rollback support) - OTA only replaces the app. All settings (saved WLANs, CAN pins, rules, STOP latch,
watchdog) are read from the same NVS keys and survive. After that, updates work over OTA again.

**ESP-IDF configuration:** `sdkconfig.defaults` (+ `sdkconfig.defaults.esp32` / `.esp32s3` / `.esp32c3`) lists every
setting that differs from ESP-IDF's defaults, with the reason. PlatformIO generates `sdkconfig.<env>` from it on the
first build (git-ignored). To change something: `pio run -e esp32dev -t menuconfig`, then copy the option into
`sdkconfig.defaults` and delete `sdkconfig.<env>` so it is regenerated.

**Flash layout** (`partitions.csv`, all boards use 4 MB): NVS at 0x9000 (unchanged), 2 × 1.875 MB app slots for OTA,
an unused 128 KB data partition, and a 64 KB `coredump` partition where ESP-IDF stores a crash report.
**Changing the partition table requires a flash over USB**; OTA cannot do it.

## 3. WLAN, web interface, OTA

**Access point, always on:** `franke-can` (password `AP_PASSWORD`, default `franke-can`), web UI at **http://192.168.4.1**. You can always reach the device this way, even if no WLAN is configured or in range. The ESP32 has only one radio, so the AP follows the channel of the WLAN it is connected to, and AP clients may stall for 1–2 s while the device scans for a lost network.

**WLAN networks, like on a phone:** `System → WLAN` in the web UI:
- *Scan* lists the networks in range with signal strength, channel and lock icon. Tap one and enter its password to save and connect.
- Up to **8 saved networks**, each with *Connect* and *Delete*. Hidden networks can be added by SSID.
- **Reconnect:** at boot the device connects directly to the last used network (no scan, connected within a few seconds). If the connection drops it first retries that network, then scans every 3 s (every 15 s while a client uses the AP) and connects to the strongest saved network in range. After 5 failed attempts it slows to every 20 s (60 s with AP clients).
- On first boot the network from `include/secrets.h` (`WIFI_SSID`, `WIFI_PASSWORD`) is added to the list. `secrets.h` also sets `AP_PASSWORD`, `WEB_USER` and `WEB_PASSWORD`. Never put real passwords into `secrets.example.h`, which is tracked by git.
- Over USB or TCP: `#wifi`, `#wifi scan`, `#wifi scanresult`, `#wifi add "My Net" "password"`, `#wifi connect "My Net"`, `#wifi del "My Net"` (quotes allow spaces; escape `\"` and `\\`).
- **Enterprise networks** (WPA2-Enterprise with username + password, e.g. eduroam or a university WLAN, PEAP/TTLS): `#wifi add "eduroam" "password" "user@uni.example"`, or pick the network in the web UI (it asks for the username when the network is enterprise). The login is sent as both identity and username. No CA certificate is used, so the RADIUS server is **not** verified, and credentials are stored unencrypted in NVS — the same trade-off as the display firmware this was taken from. In `secrets.h`, `WIFI_USERNAME` seeds an enterprise network on first boot. The login is a TLS handshake (PEAP) that needs ~40 KB of heap at its peak; on an ESP32 without PSRAM the firmware therefore lends its replay buffer to the login (`#info replay buffer released ...`; frames of those seconds are reported as `DATA GAP` to reconnecting clients) and uses ESP-IDF's dynamic TLS buffers. A login the server rejects (wrong password) is **not retried automatically**, so the account is not locked; press *Connect* to retry.

**CAN pins** are configurable in `System → CAN transceiver pins` (or `#pins 13 27`). The dropdowns only offer pins that are safe for this chip. The setting is saved in flash and applied immediately (the bus re-opens in LISTEN). Rewire before you apply.

**Web interface:** http://franke-can.local (or the IP from `#wifi`), login `WEB_USER` / `WEB_PASSWORD` (default `admin` / `franke`, **change them in secrets.h**; while the defaults or the placeholders of `secrets.example.h` are in use, the firmware warns at boot and `#status` shows `web_auth=default` / `ap_auth=default`). Tabs:
- **Live**: frames with decoding, unknown frames highlighted, ID filter (`481,4C6`, `!181`), "changes only", pause
- **IDs**: per-ID table with count, interval avg/min/max, last data
- **Control & Send**: LISTEN/ACTIVE/CLOSED (ACTIVE asks for confirmation), echo, send frames, repeat sending, history
- **Rules**: list, enable/disable, edit, delete, add (with spec examples), save/load to flash
- **Log**: record in the browser and download as candump `.log`
- **System**: status, statistics, bitrate, TX limit, USB output format, WLAN, **firmware update**, reboot, console
- red **STOP** button = emergency stop

**Time (all wall-clock times are UTC):** the ESP32 syncs its clock via NTP once its WLAN has internet. Without NTP (e.g. AP only), the first connected browser or PC tool sets the clock. Frame timestamps stay "µs since boot"; `#time` gives the matching epoch time, so browser and PC map timestamps identically, independent of network latency. Log file names and all displayed times are UTC (`…Z`).

**Markers:** the **📍 Marker** button in the header (hotkey `M`) takes the time at the press and then asks for an optional note (Enter saves, even when empty). Markers go to every connected client: the live view, the browser recording (downloaded as `can_…Z.markers.txt` next to the `.log`), and the Python tool's `Messungen/can_…Z.markers.txt`. Format: `(1762440000.123456) 2026-11-06T15:12:03.123456Z coffee started`, the same timestamps as the `.log`, which stays pure candump. In the Python monitor type `m` (then the note) or `m <note>`; in scripts `mark("note")`; over USB `#mark <note>`.

**Recordings in the browser (Log tab):** *Start new recording* / *Stop* (two separate buttons, a double click cannot lose anything). Recordings are written continuously into the browser's storage (IndexedDB) and stay in the *Saved recordings* list until you delete them (with confirmation); download each as `.log` and `.markers.txt`. Reloading or closing the page loses at most ~1 s: a running recording continues after a reload. If no browser storage is available, the page says so and keeps the recording in memory only.

**Connection losses:** the ESP32 keeps the last 512 frames and 32 notifications (markers, `#stop` …) in RAM (`#replay <since_us>`). After a reconnect, the web UI and the Python tool fetch what they missed, so the log has no hole. If the gap was longer than the buffer (~4 min at idle traffic) or the device restarted, a `DATA GAP: …` marker is written to the markers file instead.

**Firmware update (OTA):** `pio run -e esp32dev` → in the web UI `System → Firmware update` choose `firmware/franke-can_esp32dev.bin` → *Upload & flash*. The firmware checks the image (magic byte, chip type, project name), switches the bus to LISTEN, flashes the other app slot, verifies it (checksum + SHA-256) and reboots. A failed or interrupted upload leaves the running firmware untouched. The web UI is part of the firmware, so it is updated too. Without a browser: `curl -u admin:<password> --data-binary @firmware/franke-can_esp32dev.bin http://<ip>/update` (`-F "fw=@…"` works too).

**Rollback:** a firmware installed over OTA starts "pending". If it crashes before it has run for 30 s (`APP_VALID_AFTER_S`), the bootloader automatically goes back to the previous firmware. After 30 s it is confirmed (`#info firmware confirmed`).

## 4. Operating modes and safety

| Mode | TWAI | Behaviour |
|---|---|---|
| `LISTEN` (**after every boot**) | listen-only | receives everything, **no ACK, never transmits** |
| `ACTIVE` | normal | ACK, manual sending, rules active |
| `CLOSED` | driver stopped | nothing |

- Only ACTIVE can transmit. The check runs for every single frame, under a mutex that also guards driver reconfiguration.
- **Emergency stop** (`#stop`, BOOT button, STOP in the web UI, TX rate limit, OTA start): LISTEN, all rules disabled, pending rule actions and TX queue cleared.
- **STOP latch:** every emergency stop except OTA also sets a latch (kept across reboots, shown in the web UI header and as `stop_latch=1 stops=N` in `#status`). While it is set, `#mode active`, `#send`, `#rule add/enable/load` and SLCAN `O`/`t` are refused and running PC scripts end immediately (exit code 2). Release it with `#resume` (any link) or the **Resume** button in the web UI. `#stop nolatch` stops without latching (used by scripts for their own cleanup).
- **TX rate limit**: max. 50 frames/s (`#limit <n>`). Exceeding it triggers the emergency stop.
- **ACTIVE watchdog:** if the TCP or web client that switched the bus to ACTIVE disconnects and does not reconnect within `#watchdog` seconds (default 30, `0` = off, `watchdog=` in `#status`), the firmware reverts to LISTEN and disables all rules — **not** latched. So if a controlling PC or phone drops off the network mid-test, the machine stops being driven even without the host. USB is never watched (unplugging it resets the board anyway).

### Diagnostics under load

A sustained CAN flood (e.g. ~400 frames/s) must not reboot the device. Tools to find and prove this, none needing USB:

- **Reset reason:** every boot sends `#info reset reason=<…> boots=N crashes=N` (and shows it in the System tab). After a crash/watchdog/brownout reset, `#warn before the … reset: …` reports the heap, drop and stack snapshot from the *previous* run — kept in RTC memory, which survives everything but a power cut — so you see the state just before the crash without a serial cable.
- **Crash location:** on a crash ESP-IDF writes a core dump into the `coredump` partition. After the reboot the firmware reports `#warn crash: task=<name> pc=0x… bt=<addresses>` (also in `#health` and System → Diagnostics) and erases the dump. Translate the addresses with the `firmware.elf` of exactly that build:
  `~/.platformio/packages/toolchain-xtensa-esp-elf/bin/xtensa-esp32-elf-addr2line -pfiaC -e .pio/build/esp32dev/firmware.elf <addresses>`
- **Task watchdog:** a task that hangs for 10 s (`CONFIG_ESP_TASK_WDT_TIMEOUT_S`) restarts the device, with a crash report.
- **Health:** `#health` on demand, and automatically every 5 s while the bus is busy (> `HEALTH_BUSY_FPS`) or a self-test runs. `#warn dropped: …` is sent whenever a drop counter grows. All of this goes to the Python tool and the web UI (System → Diagnostics).
- **Flood self-test:** `#selftest flood <fps> <secs>` injects synthetic RX frames through the same output path real frames take (host queue, USB/TCP/web, replay ring), so a load problem can be reproduced on the desk. The frames are **not** transmitted on the bus and trigger no rules.
- **Web back-pressure:** live frames go to the browsers in batches every `WEB_WS_FLUSH_MS` (100 ms), from fixed buffers - nothing is ever queued per browser on the heap. If the live buffer is full, the rest is dropped and counted (`ws_dropped` / `#warn dropped … web_bytes=…`). A browser that does not take a batch within `WEB_SEND_TIMEOUT_S` (3 s) is disconnected (`ws_slow_closes`); it reconnects by itself and fetches the missed frames with `#replay`. Recordings (browser IndexedDB, Python log) are never thinned on purpose - losses are reported.
- **Bus-off**: message `#busoff`, automatic recovery, then LISTEN.
- In LISTEN the ESP32 does not ACK. With the milk module unplugged the machine repeats a frame endlessly (`0x181 00 00 01` every 736 µs). That is expected.
- Rules are only kept in RAM. `#rule save` / `#rule load` use flash, but rules are **never loaded automatically** at boot.

## 5. Protocol (USB, TCP, WebSocket)

### SLCAN (Lawicel), so SavvyCAN and python-can (`slcan`) work directly
`O` open → ACTIVE, `L` open listen-only → LISTEN, `C` close, `S0`–`S8` bitrate (`S4` = 125k), `tIIILDD..` / `TIIIIIIIILDD..` send, `Z0/Z1` timestamps, `V`, `N`, `F` status flags.
Received frames: `tIIILDD..[TTTT]\r`. **Note:** many tools send `O` when connecting, which switches the bus to ACTIVE. In SavvyCAN, enable "Listen only" (sends `L`).

**Several commands at once** (web console, Python monitor, `franke_can.py … cmd`): separate them with `;` or new lines, e.g. `#mode active; #send 481#110027; #stats`. They run one after another and stop at the first `#err`. A `;` inside "quotes" or written as `\;` does not split. In the web console, Enter runs, Shift+Enter adds a line, ↑/↓ browse the history.

### Extension commands (`#...`)
Every command is answered with data lines (if any) plus **one** final `#ok ...` / `#err ...` line, **only on the link that sent it**. Frames and notifications (`#stop`, `#busoff`, `#wifi`, `#ota`, `#txerr`, `#warn` ...) go to all links. `#` lines end with `\n\r`, so terminals show proper lines and SLCAN parsers skip them.

| Command | |
|---|---|
| `#help`, `#status`, `#version` | info |
| `#time`, `#time set <epoch_ms>` | `#time <boot_us> <epoch_us> <ntp\|host\|none>`; set only works without NTP |
| `#mark [@<boot_us>] [note]` | marker; sent to all links as `#mark <boot_us> <epoch_us> <note>` |
| `#replay <since_us>` | frames (`#prx`/`#ptx`) and notifications (`#pmsg <us> <line>`) from the RAM buffer after that time; `#replay gap <from> <to>` if it does not reach back far enough |
| `#mode listen\|active\|closed`, `#stop [nolatch]`, `#resume` | modes / emergency stop (latches unless `nolatch`) / release the STOP latch |
| `#send 481 11 00 27` or `#send 481#110027` | send a frame (ACTIVE only) |
| `#format slcan\|text`, `#ts us\|ms\|off` | `text`/`us`: `#rx <us> <id> <dlc> <data>` instead of SLCAN |
| `#echo on\|off` | own frames as `#tx <us> <id> <dlc> <data> <reason>` (reason = `manual`, `slcan` or rule name) |
| `#stats [reset]` | rx/tx, lost frames (host queue, driver RX queue, FIFO), TEC/REC, bus errors, bus-off count |
| `#bitrate <bps>`, `#limit <frames/s>` | |
| `#watchdog [secs]` | show/set the ACTIVE watchdog (default 30 s, `0` = off); see below |
| `#health` | heap (free/min/largest block), rx rate, drop counters, task stack headroom, reset reason |
| `#selftest flood <fps> <secs>`, `#selftest stop` | inject synthetic RX frames (id 7FF) into the output path to reproduce load on the desk — **not** put on the bus |
| `#wifi`, `#wifi scan`, `#wifi scanresult`, `#wifi add\|connect\|del "<ssid>" ["<password>"] ["<enterprise user>"]` | WLAN state, saved networks, scan (see section 3) |
| `#pins`, `#pins <tx> <rx>` | show / set CAN pins (only safe pins are accepted) |
| `#rule ...` | see below |
| `#reboot` | |

### Rules (run in the firmware, independent of USB/WLAN latency)
```
#rule add <name> match=<id>[/<mask>][:<bytes>] send=<id>:<bytes>[@<ms>] [send=...] [dlc=<n>] [once]
#rule del <name> | #rule enable <name|all> | #rule disable <name|all> | #rule list | #rule clear | #rule save | #rule load
```
- `match` bytes are a prefix: `??` = any byte, `1?` = any low nibble. `/mask` masks the ID (`480/7F0` = 0x480–0x48F).
- `send` bytes can contain `{b0}`…`{b7}` = byte from the trigger frame. `@ms` = delay 0–5000 ms after the trigger.
- Up to 16 rules with up to 6 actions each. `once` fires once until the rule is re-enabled or re-added. `add` with an existing name replaces that rule.
- Rules only act in ACTIVE mode.

```
#rule add poll  match=481:11 00 27  send=400:00 01 09@1  send=480:00 01 28@3
#rule add anfr  match=481:11 00 21  send=400:00 01 09@1  send=480:00 01 22@3
```

## 6. Python host tool

```
cd host
py -m pip install -r requirements.txt
py franke_can.py COM7 monitor                       # live view + log in Messungen/
py franke_can.py COM7 monitor --exclude 181 --changes
py franke_can.py socket://franke-can.local:3333 monitor   # same over WLAN
py franke_can.py COM7 cmd "#status" "#rule list"
py franke_can.py COM7 script scripts/test_kasse.py  # test script; ends with #stop
py franke_can.py decode ../Messungen/can_2026-11-06_14-30-00.log [--id 481] [--stats-only]
```
- **monitor**: time, RX/TX, ID, data, meaning; unknown frames in yellow, own frames in magenta. Type `#...` commands or frames (`481#124655`), `quit` to exit. The tool switches the firmware to text format + echo and restores the previous settings on exit. With `prompt_toolkit` installed, the input line stays put at the bottom.
- **Log**: `Messungen/can_<date>_<time>Z.log` (UTC) in candump format (`(1730860000.123456) can0 481#114627`), readable by SavvyCAN, can-utils and python-can. Notifications, commands and script results go into `….events.txt`, markers into `….markers.txt` next to it. Timestamps are device µs mapped via the device clock (NTP), so intervals are exact.
- **decode** merges `….markers.txt` (if present) into the output at the right position.
- **Connection lost** (WLAN, USB unplugged): the tool keeps trying to reconnect every 2 s, then fetches the missed frames from the device buffer (`#replay`), so the log continues without a hole.
- **Scripts** (`py franke_can.py <port> script scripts/x.py key=value …`) run on the PC and get `send()`, `rule()`, `mode()`, `wait_for()`, `collect()`, `next_frame()`, `mark()`, `csv()`, `result()`, `state` (resume), `args` and more as globals. They survive short connection drops (waits count only connected time, missed frames are replayed). Full reference and safety rules: **[host/SCRIPTING.md](host/SCRIPTING.md)**. Examples: `scripts/test_kasse.py`, `scripts/sweep_reply.py`.
- **decode** also reads `.asc`, `.blf`, ... via python-can and prints statistics per ID and per known frame type.

## 7. Tests

```
pio test -e native
```
This needs a host `gcc`/`g++` in PATH (e.g. MSYS2 or WinLibs). It tests the SLCAN codec and the rule engine (parsing, matching, wildcards, placeholders, once, replace) plus the rate limiter.

## 8. Notes / deviations from the spec
- `#tx` lines also contain the DLC (`#tx <us> <id> <dlc> <data> <reason>`) so that data and reason can be separated unambiguously.
- Command replies only go to the link that sent the command (USB, TCP or browser), so the web UI's periodic polling doesn't clutter the USB stream.
- SLCAN parser and rule engine live in `lib/franke_core` (no ESP dependency) so they can be unit-tested on the PC. `src/commands.cpp` and `src/rules.cpp` are the firmware glue.
- WLAN is always built in (web UI + OTA). TCP port 3333 provides the same protocol as USB. Only one TCP client at a time; a stalled client never blocks USB or CAN, and its frame output is dropped and counted (`tcp_dropped`). Command replies (e.g. `#replay`) wait for the client instead, so nothing of a reply is lost. TCP keepalive closes a connection whose PC vanished (router off, laptop asleep) after ~25 s, so the slot is free for its reconnect and the ACTIVE watchdog notices.
- Firmware 2.x is a native ESP-IDF 5.2 application (1.x was Arduino-ESP32 2.0 / IDF 4.4). Besides the cleaner structure this brings: LISTEN mode that never drives the bus even on errors (`TWAI_ERRATA_FIX_LISTEN_ONLY_DOM`), CAN reception during flash writes (`TWAI_ISR_IN_IRAM`), crash reports, OTA rollback, task watchdog with restart, and a web server whose memory use does not grow with slow clients.
- Not tested on real hardware yet: all firmware variants build, and the host-side logic is covered by the unit tests and a simulated device.
