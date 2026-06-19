#!/usr/bin/env bash
# Install the native Wave:3 daemon, CLI, systemd user service, D-Bus policy,
# and WirePlumber/PipeWire configuration.
# Run as a normal user; only the udev rule step needs root.

set -euo pipefail

PREFIX="${PREFIX:-$HOME/.local}"
BIN_DIR="$PREFIX/bin"
SERVICE_DIR="$HOME/.config/systemd/user"
DBUS_DIR="$HOME/.local/share/dbus-1/services"
WP_CONF_DIR="$HOME/.config/wireplumber/wireplumber.conf.d"
WP_SCRIPT_DIR="$HOME/.local/share/wireplumber/scripts"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$REPO_ROOT"

if [[ -t 1 ]]; then
    BOLD='\033[1m'
    GREEN='\033[0;32m'
    YELLOW='\033[0;33m'
    CYAN='\033[0;36m'
    NC='\033[0m'
else
    BOLD='' GREEN='' YELLOW='' CYAN='' NC=''
fi

info()  { echo -e "${CYAN}==>${NC} $*"; }
ok()    { echo -e "${GREEN}✓${NC} $*"; }
warn()  { echo -e "${YELLOW}!${NC} $*"; }

info "Building wave3-daemon..."
make -C src clean
make -C src
ok "Build complete"

info "Installing binaries to $BIN_DIR..."
install -Dm755 src/wave3-daemon "$BIN_DIR/wave3-daemon"
install -Dm755 bin/wave3ctl "$BIN_DIR/wave3ctl"
install -Dm755 src/cfg_probe "$BIN_DIR/wave3-cfg-probe"
install -Dm755 src/watch_state "$BIN_DIR/wave3-watch-state"
install -Dm755 src/device_info "$BIN_DIR/wave3-device-info"
install -Dm755 src/auto_probe "$BIN_DIR/wave3-auto-probe"
install -Dm755 src/poll_observatory "$BIN_DIR/wave3-poll-observatory"
ok "Binaries installed"

info "Installing systemd user service..."
install -Dm644 systemd/wave3-daemon.service "$SERVICE_DIR/wave3-daemon.service"
ok "Service installed"

info "Installing D-Bus service activation file..."
install -Dm644 dbus/org.wave3.Daemon.service "$DBUS_DIR/org.wave3.Daemon.service"
ok "D-Bus service file installed"

info "Installing WirePlumber configuration..."
install -Dm644 wireplumber/wireplumber.conf.d/50-elgato-wave3.conf "$WP_CONF_DIR/50-elgato-wave3.conf"
install -Dm644 wireplumber/scripts/elgato-wave3.lua "$WP_SCRIPT_DIR/elgato-wave3.lua"
ok "WirePlumber config installed"

info "Reloading systemd user daemon..."
systemctl --user daemon-reload || true
ok "Systemd reloaded"

echo ""
echo -e "${BOLD}Installation complete.${NC}"
echo ""
echo "Next steps (require root):"
echo "  sudo install -Dm644 $REPO_ROOT/udev/50-elgato-wave3.rules /etc/udev/rules.d/"
echo "  sudo install -Dm644 $REPO_ROOT/dbus/org.wave3.Daemon-system.conf /etc/dbus-1/system.d/org.wave3.Daemon.conf"
echo "  sudo udevadm control --reload-rules"
echo "  sudo udevadm trigger --subsystem-match=usb --attr-match=idVendor=0fd9 --attr-match=idProduct=0070"
echo ""
echo "Then restart WirePlumber and start the daemon:"
echo "  systemctl --user restart wireplumber"
echo "  systemctl --user enable --now wave3-daemon"
echo ""
echo "Use the CLI:"
echo "  wave3ctl status"
echo "  wave3ctl mute toggle"
echo "  wave3ctl volume 75"
