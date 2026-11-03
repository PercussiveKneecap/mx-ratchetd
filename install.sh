#!/usr/bin/env bash
# Builds and installs mx-ratchetd as a systemd service.
# Requires: gcc (package base-devel) - nothing else.
#   sudo ./install.sh            install / update
#   sudo ./install.sh uninstall  remove everything
set -euo pipefail

BIN=/usr/local/bin/mx-ratchetd
UNIT=/etc/systemd/system/mx-ratchetd.service

if [[ $EUID -ne 0 ]]; then
    echo "Run as root: sudo $0 ${1:-}" >&2
    exit 1
fi

# Leftovers of the first (udev + Python) version
cleanup_v1() {
    rm -fv /usr/local/bin/mx-ratchet \
           /etc/systemd/system/mx-ratchet@.service \
           /etc/udev/rules.d/99-mx-ratchet.rules
}

if [[ "${1:-}" == "uninstall" ]]; then
    systemctl disable --now mx-ratchetd.service 2>/dev/null || true
    rm -fv "$BIN" "$UNIT"
    cleanup_v1
    systemctl daemon-reload
    udevadm control --reload
    echo "Removed."
    exit 0
fi

command -v gcc >/dev/null || { echo "gcc not found: sudo pacman -S --needed base-devel" >&2; exit 1; }

cd "$(dirname "$(readlink -f "$0")")"

gcc -O2 -Wall -Wextra -o mx-ratchetd mx-ratchetd.c -lsystemd
install -Dm755 mx-ratchetd "$BIN"
cleanup_v1

cat > "$UNIT" <<'EOF'
[Unit]
Description=Keep Logitech MX Master wheel in ratchet mode (Bluetooth)
Wants=bluetooth.service
After=bluetooth.service

[Service]
ExecStart=/usr/local/bin/mx-ratchetd
Restart=on-failure
RestartSec=5
NoNewPrivileges=yes
ProtectSystem=strict
ProtectHome=yes
PrivateTmp=yes
MemoryDenyWriteExecute=yes
RestrictAddressFamilies=AF_UNIX

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
udevadm control --reload
systemctl enable --now mx-ratchetd.service
systemctl restart mx-ratchetd.service

echo
echo "Installed. Follow the log with:  journalctl -fu mx-ratchetd"
