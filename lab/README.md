# lab/ – can-lab: remote test bench for the ESP32 CAN tool

`can-lab` runs on a Raspberry Pi next to the coffee machine. It executes host test scripts
(`host/franke_can.py … script …`) against the ESP32 and exposes them as a **remote MCP server**, so
Claude (claude.ai custom connector) can write, run and evaluate test scripts on its own. A small web
page shows jobs and logs and has an emergency-stop button.

```
claude.ai ──MCP──> Cloudflare Tunnel ──> Pi: can_lab.py (127.0.0.1:8765) ──TCP :3333──> ESP32 ──CAN──> FM850
```

## Install / update on the Pi

```bash
# first install (also migrates an old ~/can-lab tarball install incl. its secret):
curl -fsSL https://raw.githubusercontent.com/angeeinstein/CAN-tool/main/lab/install.sh | bash

# update (git pull, reinstall packages, restart):
bash ~/CAN-tool/lab/install.sh --update

# remove the service (repo and data stay):
bash ~/CAN-tool/lab/install.sh --uninstall
```

Run as the normal user (not with sudo); the script calls sudo itself. It installs system packages,
creates a venv (Python ≥ 3.10, falls back to a uv-managed 3.11), writes the systemd unit `can-lab`,
checks that the ESP32 answers on port 3333 and prints the connector URL.

| What | Where |
|---|---|
| Code | `~/CAN-tool/lab/` (this folder, from git) |
| Host tool used | `~/CAN-tool/host/franke_can.py` (same commit → always matches the firmware) |
| Config + secret | `~/can-lab-data/config.json` (`device_ip`, `listen_port`, `secret`) |
| Test scripts | `~/can-lab-data/scripts/` (seeded from `lab/scripts/`, never overwritten) |
| Job history | `~/can-lab-data/data/` |
| CAN logs | `~/CAN-tool/Messungen/` (git-ignored) |

Service: `systemctl status can-lab`, `journalctl -u can-lab -f`.

## Cloudflare + claude.ai

- Cloudflare tunnel public hostname → `http://localhost:8765`, **no** Access policy (claude.ai cannot log in).
- Connector URL: `https://<hostname>/<secret>/mcp`, authentication "No sign-in".
  The secret path is the only lock – keep it private; to rotate, delete `secret` in config.json and
  restart the service.
- Job page in the browser: `https://<hostname>/<secret>/`.

## MCP tools

`device_status`, `set_device_ip`, `get_scripting_doc`, `list_scripts`, `get_script`, `put_script`,
`run_script`, `run_inline`, `job_status`, `job_output`, `stop_job`, `emergency_stop`, `list_logs`,
`read_log` (filtered by ID/time/payload), `log_summary`, `read_text_file` (markers/events/csv).

One job at a time (FIFO queue); jobs survive client disconnects. `stop_job` = Ctrl+C (script cleanup
→ LISTEN, rules off). `emergency_stop` also sends a latched `#stop` (release with Resume in the web UI).

## Notes for maintainers

- `can_lab.py` only calls `host/franke_can.py` as a subprocess (`<dev> script <file> key=value …` and
  `<dev> cmd "#status"` / `"#stop"`) and parses its console output for the log file name
  (`can_…​.log`). **If you change the CLI, the output format of the log path line, or the log location
  (`Messungen/`), update `can_lab.py` too.**
- Endpoint `GET/POST /<secret>/announce?ip=<ip>&fw=<ver>` lets the ESP32 report its IP after a Wi-Fi
  connect (DHCP, mDNS is blocked on the university network). Not yet called by the firmware.
- `lab/scripts/` holds the proven test scripts (reply_detect, error_state, bus_check, …); the working
  copies on the Pi live in `~/can-lab-data/scripts/` and may diverge.
