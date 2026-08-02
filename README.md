# DAO ZMK WITH DONGLE

Self-contained ZMK firmware configuration for Dao keyboard with dongle setup.

> **Disclaimer:** this project is mostly vibecoded, not thoughtfully hand-crafted.
> It is, however, battle-tested — it drives the author's daily-driver keyboard.
> Read the code with that in mind before copying anything.

## Hardware Setup

This configuration supports:
- **Dongle**: Seeed XIAO BLE (acts as BLE central, connects to the host over USB)
- **Left half**: Dao left peripheral board (nRF52840)
- **Right half**: Dao right peripheral board (nRF52840)

## Building Firmware

### Prerequisites
- Docker
- Just (command runner)
- `yq`, `jq`

### Build Commands

Initialize the build environment (first time only):
```bash
just init
```

Build all firmware files:
```bash
just build
```

Build a specific target:
```bash
just build-part dao_left
just build-part dao_right
just build-part xiao_ble//zmk dao_dongle
```

> The dongle board id must be `xiao_ble//zmk` (the ZMK board *variant*), not
> plain `xiao_ble` — since Zephyr 4.1 the plain board builds without BLE and
> the halves silently fail to connect.

## Flashing Instructions

### 1. Reset Settings (Important!)

Before flashing new firmware, reset settings on all devices:

1. Flash `settings_reset-dao_left-zmk.uf2` to left half
2. Flash `settings_reset-dao_right-zmk.uf2` to right half
3. Flash `settings_reset-xiao_ble__zmk-zmk.uf2` to dongle

### 2. Flash Main Firmware

After resetting settings, flash the main firmware:

1. Flash `dao_left-zmk.uf2` to left half
2. Flash `dao_right-zmk.uf2` to right half
3. Flash `dao_dongle-xiao_ble__zmk-zmk.uf2` to dongle

### 3. Pairing

The halves pair to the dongle automatically. After a settings reset it can
help to reset the dongle and a half at nearly the same time so they
re-discover each other.

## Per-Half Battery on the Host

ZMK only reports battery over BLE, so the dongle adds two extra USB HID
interfaces to carry it to the host:

- A **native battery** entry ("Dao Keyboard") that shows up in `upower` / the
  system tray automatically, reporting the worse of the two halves.
- A **vendor interface** with separate left/right levels, consumed by the
  bundled KDE Plasma 6 widget (keyboard glyph flanked by two fill bars).
  Left/right is auto-detected from keystrokes, so it survives re-pairing in
  any order.

Host-side install (udev rule, reader service, plasmoid):

```bash
./host/install.sh        # first-time setup; adds you to the `input` group (re-login needed)
just widget-install      # reinstall reader + service + widget after changes
```

## Configuration Files

- `config/dao.keymap` - Keymap (single source of truth for all three targets)
- `config/dao.conf` - Shared configuration
- `config/boards/dao/dao_left/` - Left half board definition
- `config/boards/dao/dao_right/` - Right half board definition
- `config/boards/shields/dao_dongle/` - Dongle shield configuration
- `config/src/battery_hid.c` - Battery-over-USB firmware (dongle only)
- `host/` - Linux reader service + KDE Plasma widget

## Architecture

This configuration is completely self-contained and does not depend on external ZMK modules:

- All board definitions are local in `config/boards/`
- Dongle uses mock kscan (no physical keys)
- Left and right halves act as BLE peripherals
- Dongle acts as BLE central, forwarding key presses via USB

## Build Output

After building, you'll find the following files in `build/`:
- `dao_dongle-xiao_ble__zmk-zmk.uf2` - Dongle firmware
- `dao_left-zmk.uf2` - Left half firmware
- `dao_right-zmk.uf2` - Right half firmware
- `settings_reset-*.uf2` - Settings reset firmware for each device
