#!/usr/bin/env bash
# can-lab installer / updater (Raspberry Pi OS, Debian, Ubuntu).
#
#   First install (nothing on the Pi yet):
#     curl -fsSL https://raw.githubusercontent.com/angeeinstein/CAN-tool/main/lab/install.sh | bash
#   Inside an existing clone:   bash lab/install.sh              install / repair
#   Update:                     bash lab/install.sh --update     git pull + reinstall + restart
#   Remove the service:         bash lab/install.sh --uninstall
#
# Runtime data lives outside the repo in $CANLAB_HOME (default ~/can-lab-data):
#   config.json (secret!), scripts/, data/ (job history), .venv/
# Logs go to <repo>/Messungen (git-ignored). Re-running is always safe.
set -euo pipefail

REPO_URL="${CANLAB_REPO:-https://github.com/angeeinstein/CAN-tool.git}"
BRANCH="${CANLAB_BRANCH:-main}"
SERVICE=can-lab
DEFAULT_IP="10.240.8.42"
DEFAULT_PORT=8765
MODE="${1:-install}"

c_ok()   { printf '\e[32m[ok]\e[0m %s\n' "$*"; }
c_info() { printf '\e[36m[..]\e[0m %s\n' "$*"; }
c_warn() { printf '\e[33m[!!]\e[0m %s\n' "$*"; }
die()    { printf '\e[31m[xx]\e[0m %s\n' "$*" >&2; exit 1; }

if [[ $EUID -eq 0 ]]; then
  [[ -n "${CANLAB_ALLOW_ROOT:-}" ]] || die "run as your normal user (without sudo) - the script uses sudo where needed"
  SUDO=""
else
  command -v sudo >/dev/null || die "sudo missing"; SUDO="sudo"
fi
RUN_USER="$(id -un)"; RUN_GROUP="$(id -gn)"
CANLAB_HOME="${CANLAB_HOME:-$HOME/can-lab-data}"

# ------------------------------------------------------------------ where is the repo?
SELF="${BASH_SOURCE[0]:-}"
REPO=""
if [[ -n "$SELF" && -f "$SELF" ]]; then
  d="$(cd "$(dirname "$SELF")" && pwd)"
  [[ -f "$d/can_lab.py" && -f "$d/../host/franke_can.py" ]] && REPO="$(cd "$d/.." && pwd)"
fi
if [[ -z "$REPO" ]]; then
  # piped through curl, or copied somewhere else: clone (or reuse) ~/CAN-tool and continue from there
  REPO="${CANLAB_REPO_DIR:-$HOME/CAN-tool}"
  if ! command -v git >/dev/null; then $SUDO apt-get update -qq && $SUDO apt-get install -y -qq git >/dev/null; fi
  if [[ -d "$REPO/.git" ]]; then
    c_info "using existing clone $REPO"
  else
    c_info "cloning $REPO_URL -> $REPO"
    git clone --branch "$BRANCH" "$REPO_URL" "$REPO" || die "git clone failed"
  fi
  exec bash "$REPO/lab/install.sh" "$@"
fi
LAB="$REPO/lab"

# ------------------------------------------------------------------ uninstall
if [[ "$MODE" == "--uninstall" ]]; then
  $SUDO systemctl disable --now "$SERVICE" 2>/dev/null || true
  $SUDO rm -f "/etc/systemd/system/$SERVICE.service"; $SUDO systemctl daemon-reload
  c_ok "service removed. Left untouched: $REPO (repo, logs in Messungen) and $CANLAB_HOME (config, scripts, venv)."
  exit 0
fi

# ------------------------------------------------------------------ update: git pull, then re-run the NEW installer
if [[ "$MODE" == "--update" ]]; then
  c_info "updating $REPO ..."
  git -C "$REPO" fetch --quiet origin "$BRANCH" || die "git fetch failed (network?)"
  if ! git -C "$REPO" diff --quiet || ! git -C "$REPO" diff --cached --quiet; then
    c_warn "local changes in the repo - stashing them (git stash list to see them)"
    git -C "$REPO" stash push --quiet -m "can-lab auto-stash $(date -Iseconds)"
  fi
  OLD="$(git -C "$REPO" rev-parse --short HEAD)"
  git -C "$REPO" checkout --quiet "$BRANCH"
  git -C "$REPO" merge --ff-only --quiet "origin/$BRANCH" || git -C "$REPO" reset --hard --quiet "origin/$BRANCH"
  NEW="$(git -C "$REPO" rev-parse --short HEAD)"
  [[ "$OLD" == "$NEW" ]] && c_ok "already up to date ($NEW)" || c_ok "updated $OLD -> $NEW"
  exec bash "$LAB/install.sh" --post-update
fi

echo "== can-lab installer =="
echo "repo: $REPO   data: $CANLAB_HOME   user: $RUN_USER"

# ------------------------------------------------------------------ system packages
c_info "system packages ..."
export DEBIAN_FRONTEND=noninteractive
if command -v apt-get >/dev/null; then
  $SUDO apt-get update -qq || c_warn "apt-get update failed - continuing"
  $SUDO apt-get install -y -qq git python3 python3-venv python3-pip python3-dev build-essential libffi-dev curl ca-certificates >/dev/null \
    || c_warn "some apt packages could not be installed - continuing"
fi
command -v python3 >/dev/null || die "python3 not found"

PY=python3
if ! $PY -c 'import sys;sys.exit(0 if sys.version_info>=(3,10) else 1)'; then
  c_warn "Python $($PY -V) is too old (need >= 3.10) - installing Python 3.11 via uv"
  command -v uv >/dev/null || [[ -x "$HOME/.local/bin/uv" ]] || curl -LsSf https://astral.sh/uv/install.sh | sh || die "uv install failed"
  UV="$(command -v uv || echo "$HOME/.local/bin/uv")"
  "$UV" python install 3.11 || die "uv python install failed"
  PY="$("$UV" python find 3.11)"
fi
c_ok "python: $($PY --version)"

# ------------------------------------------------------------------ data dir + migration from the old ~/can-lab tarball install
mkdir -p "$CANLAB_HOME/scripts" "$CANLAB_HOME/data" "$REPO/Messungen"
chmod 700 "$CANLAB_HOME"
OLD="$HOME/can-lab"
if [[ -f "$OLD/can_lab.py" && "$OLD" != "$LAB" && ! -f "$CANLAB_HOME/config.json" ]]; then
  c_info "migrating old install from $OLD ..."
  [[ -f "$OLD/config.json" ]] && cp "$OLD/config.json" "$CANLAB_HOME/config.json"
  cp -n "$OLD"/scripts/*.py "$CANLAB_HOME/scripts/" 2>/dev/null || true
  [[ -d "$OLD/data" ]] && cp -rn "$OLD/data/." "$CANLAB_HOME/data/" 2>/dev/null || true
  cp -n "$OLD"/CAN-tool/Messungen/* "$REPO/Messungen/" 2>/dev/null || true
  c_ok "migrated (config/secret, scripts, job history, logs). The old folder $OLD can be deleted afterwards."
fi
# seed scripts from the repo (never overwrites scripts that already exist on the Pi)
for f in "$LAB"/scripts/*.py; do [[ -e "$CANLAB_HOME/scripts/$(basename "$f")" ]] || cp "$f" "$CANLAB_HOME/scripts/"; done

# ------------------------------------------------------------------ venv + packages
VENV="$CANLAB_HOME/.venv"
if [[ -x "$VENV/bin/python" ]] && ! "$VENV/bin/python" -c 'import sys;sys.exit(0 if sys.version_info>=(3,10) else 1)'; then rm -rf "$VENV"; fi
[[ -x "$VENV/bin/python" ]] || { c_info "creating venv ..."; "$PY" -m venv "$VENV" || die "venv creation failed"; }
c_info "python packages (first time on a Pi can take a few minutes) ..."
"$VENV/bin/python" -m pip install -q --upgrade pip wheel setuptools || c_warn "pip upgrade failed - continuing"
EXTRA=(); { grep -qi raspbian /etc/os-release 2>/dev/null || [[ -f /etc/rpi-issue ]]; } && EXTRA=(--extra-index-url https://www.piwheels.org/simple)
REQS=(-r "$LAB/requirements.txt"); [[ -f "$REPO/host/requirements.txt" ]] && REQS+=(-r "$REPO/host/requirements.txt")
if ! "$VENV/bin/python" -m pip install -q "${EXTRA[@]}" "${REQS[@]}"; then
  c_warn "full install failed - retrying with only the required packages"
  if ! "$VENV/bin/python" -m pip install -q "${EXTRA[@]}" -r "$LAB/requirements.txt"; then
    $SUDO apt-get install -y -qq rustc cargo >/dev/null 2>&1 || true
    "$VENV/bin/python" -m pip install "${EXTRA[@]}" -r "$LAB/requirements.txt" || die "python packages could not be installed"
  fi
fi
"$VENV/bin/python" -c "import mcp.server.fastmcp, uvicorn, serial" || die "package import check failed"
c_ok "python packages ok"

# ------------------------------------------------------------------ config.json
CFG="$CANLAB_HOME/config.json"
if [[ -f "$CFG" ]]; then
  c_ok "keeping config.json"
else
  IP="$DEFAULT_IP"
  if [[ -r /dev/tty ]]; then read -r -p "ESP32 IP address [$DEFAULT_IP]: " ans </dev/tty || true; IP="${ans:-$DEFAULT_IP}"; fi
  PORT=$DEFAULT_PORT
  while ss -ltn 2>/dev/null | grep -q ":$PORT "; do PORT=$((PORT+1)); done
  SECRET="$("$VENV/bin/python" -c 'import secrets;print(secrets.token_urlsafe(24))')"
  printf '{\n  "device_ip": "%s",\n  "listen_host": "127.0.0.1",\n  "listen_port": %s,\n  "secret": "%s"\n}\n' "$IP" "$PORT" "$SECRET" > "$CFG"
  c_ok "config.json written"
fi
chmod 600 "$CFG"
get() { "$VENV/bin/python" -c "import json,sys;print(json.load(open('$CFG')).get('$1','$2'))"; }
PORT="$(get listen_port $DEFAULT_PORT)"; SECRET="$(get secret '')"; IP="$(get device_ip '')"

# ------------------------------------------------------------------ ESP32 reachable?
if [[ -n "$IP" ]] && "$VENV/bin/python" -c "import socket,sys;s=socket.socket();s.settimeout(4);s.connect(('$IP',3333))" 2>/dev/null; then
  c_ok "ESP32 $IP:3333 reachable"
else
  c_warn "ESP32 $IP:3333 not reachable right now - service is installed anyway (fix with set_device_ip or config.json)"
fi

# ------------------------------------------------------------------ systemd
$SUDO tee "/etc/systemd/system/$SERVICE.service" >/dev/null <<EOF
[Unit]
Description=can-lab (Franke CAN test runner + MCP server)
After=network-online.target
Wants=network-online.target

[Service]
User=$RUN_USER
Group=$RUN_GROUP
WorkingDirectory=$LAB
Environment=CANLAB_HOME=$CANLAB_HOME
Environment=PYTHONUNBUFFERED=1
ExecStart=$VENV/bin/python $LAB/can_lab.py
Restart=always
RestartSec=5
KillSignal=SIGINT
TimeoutStopSec=30

[Install]
WantedBy=multi-user.target
EOF
$SUDO systemctl daemon-reload
$SUDO systemctl enable "$SERVICE" >/dev/null 2>&1
if [[ "$MODE" == "--post-update" ]] && systemctl is-active --quiet "$SERVICE"; then
  c_info "restarting service (a running job is stopped cleanly; its device cleanup runs) ..."
fi
$SUDO systemctl restart "$SERVICE"
ok=0; for _ in $(seq 1 25); do curl -fs "http://127.0.0.1:$PORT/$SECRET/" >/dev/null && { ok=1; break; }; sleep 1; done
[[ $ok == 1 ]] || { $SUDO journalctl -u "$SERVICE" -n 30 --no-pager; die "service does not answer (log above)"; }
c_ok "service running on 127.0.0.1:$PORT  (version $(git -C "$REPO" rev-parse --short HEAD))"

if ! command -v cloudflared >/dev/null; then
  c_warn "cloudflared not installed - see lab/README.md"
elif ! systemctl show -p ExecStart cloudflared 2>/dev/null | grep -q -- "--token"; then
  c_info "cloudflared uses a config file: add  hostname -> http://127.0.0.1:$PORT  above the final http_status:404 rule"
else
  c_info "cloudflared (dashboard-managed): public hostname must point to http://localhost:$PORT"
fi

cat <<EOF

================================================================
 can-lab running.   Job page:  http://127.0.0.1:$PORT/$SECRET/
 Connector URL:     https://<your-hostname>/$SECRET/mcp
 Update later:      bash $LAB/install.sh --update
 Data / config:     $CANLAB_HOME      Logs: $REPO/Messungen
================================================================
EOF
