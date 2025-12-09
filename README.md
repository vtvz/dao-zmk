# DAO ZMK WITH DONGLE

Self-contained ZMK firmware configuration for Dao keyboard with dongle setup.

## Hardware Setup

This configuration supports:
- **Dongle**: Seeeduino XIAO BLE (acts as central device)
- **Left half**: Dao left peripheral board (nRF52840)
- **Right half**: Dao right board (nRF52840)

## Building Firmware

### Prerequisites
- Docker
- Just (command runner)

### Build Commands

Initialize the build environment (first time only):
```bash
just init
```

Build all firmware files:
```bash
just build
```

Build specific board:
```bash
just build-part dao_left
just build-part dao_right
just build-part seeeduino_xiao_ble dao_dongle
```

## Flashing Instructions

### 1. Reset Settings (Important!)

Before flashing new firmware, reset settings on all devices:

1. Flash `settings_reset-dao_left-zmk.uf2` to left half
2. Flash `settings_reset-dao_right-zmk.uf2` to right half
3. Flash `settings_reset-seeeduino_xiao_ble-zmk.uf2` to dongle

### 2. Flash Main Firmware

After resetting settings, flash the main firmware:

1. Flash `dao_left-zmk.uf2` to left half
2. Flash `dao_right-zmk.uf2` to right half
3. Flash `dao_dongle-seeeduino_xiao_ble-zmk.uf2` to dongle

### 3. Pairing

After flashing, the keyboard halves will automatically connect to the dongle via Bluetooth.

## Configuration Files

- `config/dao.keymap` - Keymap configuration
- `config/dao.conf` - Global configuration
- `config/boards/arm/dao_left/` - Left half board definition
- `config/boards/arm/dao_right/` - Right half board definition
- `config/boards/shields/dao_dongle/` - Dongle shield configuration

## Architecture

This configuration is completely self-contained and does not depend on external ZMK modules:

- All board definitions are local in `config/boards/`
- Dongle uses mock kscan (no physical keys)
- Left and right halves act as BLE peripherals
- Dongle acts as BLE central, forwarding key presses via USB

## Build Output

After building, you'll find the following files in `build/`:
- `dao_dongle-seeeduino_xiao_ble-zmk.uf2` - Dongle firmware
- `dao_left-zmk.uf2` - Left half firmware
- `dao_right-zmk.uf2` - Right half firmware
- `settings_reset-*.uf2` - Settings reset firmware for each device
