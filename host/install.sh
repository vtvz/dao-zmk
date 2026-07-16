#!/usr/bin/env bash
# Install the Dao keyboard battery host side on KDE Plasma 6 (Linux):
#   1. udev rule (grants read access to the dongle's hidraw node via `input` group)
#   2. the reader user systemd service (USB HID -> ~/.local/state/dao-battery.json)
#   3. the Plasma 6 plasmoid
#
# Re-runnable. Needs sudo for the udev rule + group; the rest is per-user.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "==> Installing udev rule (needs sudo)"
sudo cp "$HERE/udev/99-dao-dongle-battery.rules" /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger

if ! id -nG "$USER" | tr ' ' '\n' | grep -qx input; then
    echo "==> Adding $USER to the 'input' group (needs sudo; log out/in afterwards)"
    sudo usermod -aG input "$USER"
    NEED_RELOGIN=1
fi

echo "==> Installing reader + user service"
mkdir -p "$HOME/.local/bin"
install -m 0755 "$HERE/reader/dao-battery-reader.py" "$HOME/.local/bin/dao-battery-reader.py"
mkdir -p "$HOME/.config/systemd/user"
cp "$HERE/systemd/dao-battery-reader.service" "$HOME/.config/systemd/user/"
systemctl --user daemon-reload
systemctl --user enable --now dao-battery-reader.service || true

echo "==> Installing Plasma widget"
if command -v kpackagetool6 >/dev/null 2>&1; then
    kpackagetool6 --type Plasma/Applet --install "$HERE/plasmoid/dao-battery" 2>/dev/null \
        || kpackagetool6 --type Plasma/Applet --upgrade "$HERE/plasmoid/dao-battery"
else
    echo "   kpackagetool6 not found; copying manually"
    dest="$HOME/.local/share/plasma/plasmoids/com.vtvz.dao-battery"
    mkdir -p "$(dirname "$dest")"
    rm -rf "$dest"
    cp -r "$HERE/plasmoid/dao-battery" "$dest"
fi

echo
echo "Done."
if [ "${NEED_RELOGIN:-0}" = "1" ]; then
    echo "IMPORTANT: log out and back in so the 'input' group takes effect,"
    echo "then: systemctl --user restart dao-battery-reader.service"
fi
echo "Add the widget: right-click panel -> Add Widgets -> 'Dao Keyboard Battery'."
