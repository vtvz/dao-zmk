# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

ZMK firmware config for the **Dao** split keyboard (nRF52840 halves) with a **dongle** (XIAO BLE) acting as BLE central. Self-contained: all board/shield definitions live in `config/boards/`, no external ZMK module dependency.

## Research before deciding

Web-search before changing config or asserting a root cause. ZMK/Zephyr change between versions, and most issues are already solved upstream — verify, don't guess from local greps.

## Commands

All builds run inside the `zmkfirmware/zmk-build-arm:stable` Docker image via `just`. Requires Docker, `just`, `yq`, `jq`.

```bash
just init                                # First-time only: clone zmk, west init/update (populates gitignored deps)
just build                               # Build every target in build.yaml, output to build/*.uf2
just build-part dao_left                 # Build a single board (no shield)
just build-part xiao_ble//zmk dao_dongle # Build a board + shield
just build-part dao_left settings_reset
just widget-install                      # (host) reinstall the KDE Plasma battery widget + reload plasmashell
```

`just build` wipes `build/*.uf2` first, reads the matrix from `build.yaml`, and calls `build-part` for each entry. Output naming: `<shield>-<board_slug>-zmk.uf2`, or `<board_slug>-zmk.uf2` when there's no shield, where `board_slug` is the board id with `/` flattened to `_` (so the dongle output is `dao_dongle-xiao_ble__zmk-zmk.uf2`).

**Dongle board id must be `xiao_ble//zmk`, not plain `xiao_ble`.** Since Zephyr 4.1, ZMK's board defaults (`CONFIG_ZMK_BLE`, `CONFIG_ZMK_USB`) live in the `/zmk` board _variant_. Building the dongle as plain `xiao_ble` produces firmware **with no BLE central** — the halves silently fail to connect. `build.yaml` uses `xiao_ble//zmk` for the dongle and its settings_reset.

There are no tests or linters in this repo — validation is "does it build." (QML in `host/plasmoid/` can be checked with `qmllint`.)

## Build dependencies (gitignored, not source)

`zmk/`, `zephyr/`, `modules/`, `.west/`, `ergonautkb-zmk-module/`, and `build/` are all gitignored and populated by `just init` / west. **Do not edit them** — they are upstream checkouts. The only source lives in `config/` and `host/`, plus the top-level `Justfile`, `build.yaml`, and `.github/workflows/build.yml`. `config/west.yml` pins zmk to `revision: main`.

## Architecture

**Three firmware targets, one keymap.** `config/dao.keymap` is the single source of truth for all layers, behaviors, and macros. Each target includes it:

- `dao_left.keymap` / `dao_right.keymap` — select a physical layout via `chosen { zmk,physical_layout = &dao_crkbd_layout; }`, then `#include "dao.keymap"`.
- `dao_dongle.keymap` — just `#include "dao.keymap"`; the dongle has no physical layout. **The dongle must carry the same keymap as the halves** because it's the central that resolves and forwards keystrokes over USB.

When editing keymap behavior, edit `dao.keymap` — never duplicate into the per-board files.

**Split roles** are set in tiny per-target `.conf` files; everything else is shared in `config/dao.conf`:

- `dao_left.conf` / `dao_right.conf` → `CONFIG_ZMK_SPLIT_ROLE_CENTRAL=n` (peripheral)
- `boards/shields/dao_dongle/dao_dongle.conf` → `CONFIG_ZMK_SPLIT_ROLE_CENTRAL=y` (central)
- `config/dao.conf` holds all shared BLE / power / debounce / mouse settings. ZMK silently ignores settings that don't apply to a given role (peripheral-only, central-only, USB-only, battery-only), so one shared file is intentional. Mouse support (`CONFIG_ZMK_MOUSE=y`) is required for the `mmv`/`msc`/`mkp` behaviors used in the FN layer.

**Dongle has no keys.** `boards/shields/dao_dongle/dao_dongle.overlay` wires `zmk,kscan = &mock_kscan` (a zero-event mock) and defines the `default_transform` matrix so forwarded peripheral positions map correctly.

**Custom C is central-only.** `config/src/startup_layer.c` auto-activates the COLEMAK layer (layer 1) at boot via `SYS_INIT`. `config/CMakeLists.txt` compiles it **only** for central/dongle (`CONFIG_ZMK_SPLIT_ROLE_CENTRAL OR NOT CONFIG_ZMK_SPLIT`) — the peripheral halves don't run keymap resolution, so they don't need it. Layer indices referenced from C must match the `#define`s in `dao.keymap` (`QWERTY 0 / COLEMAK 1 / VIM 2 / NUM 3 / SYMB 4 / FN 5`).

**Layer model** (in `dao.keymap`): QWERTY is the base; COLEMAK is a toggle-on overlay activated at boot and via the `en` macro (CAPS + `tog_on COLEMAK`), turned off by the `ru` macro. NUM + SYMB held together activate FN via `conditional_layers` (tri-layer). The `lm` macro emulates QMK's `LM()` (momentary layer + held modifier).

## Per-half battery over USB → host widget

ZMK reports battery only over the **BLE HID Battery Service**, never over USB HID — so a USB-connected dongle can't surface the halves' battery to the host through the standard channel. This repo adds that path (based on `bogamie/zmk-split-battery-tray`):

- **Firmware** `config/src/battery_hid.c` (dongle-only, gated on `CONFIG_ZMK_SPLIT_BATTERY_HID_REPORT` defined in the dao_dongle `Kconfig.defconfig`, compiled via `config/CMakeLists.txt`) adds **two extra USB HID interfaces** (so `CONFIG_USB_HID_DEVICE_COUNT=3` in `dao_dongle.conf`, on top of ZMK's keyboard interface). Requires `CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING=y`:
  - **`HID_1` — vendor L/R** (Usage Page `0xFF00`): pushes a 3-byte report `[0x01, left%, right%]` (`0xFF` = unknown) on each peripheral battery event. Feeds the custom widget below.
  - **`HID_2` — native battery**: the standard Generic Device Controls **Battery Strength** usage (`0x06`/`0x20`), reporting the **worse of the two halves** (min). The Linux kernel auto-maps this to a `power_supply` (shows in `upower` / the KDE tray natively, like a wireless mouse's battery). The device is renamed to **"Dao Keyboard"** via `USB_DEVICE_MANUFACTURER="Dao"` + product name `"Keyboard"` (in `dao_dongle.conf` / `Kconfig.defconfig`).
    - **Kernel gotcha:** Linux only keeps a HID battery if the interface *also* registers a real input device (`hidinput_has_been_populated()` ignores `EV_PWR`) — a battery-only interface is torn down and the battery orphaned. So the battery usage is wrapped in a **minimal dummy keyboard** (modifier byte always 0 + reserved byte + battery, in one INPUT report). This creates a harmless phantom "Dao Keyboard" input device that never sends a keypress. Reusing ZMK's real keyboard interface would be cleaner but its descriptor lives in ZMK core (`zmk/app/src/hid.c`, an unedited upstream checkout).
- **Events fire only on change.** ZMK's `battery.c` raises `battery_state_changed` only when the percentage differs from last — `CONFIG_ZMK_BATTERY_REPORT_INTERVAL` (60s here) controls measurement, not emission. To compensate, the dongle re-sends the vendor report every 3s (`RESEND_INTERVAL_S` in `battery_hid.c`), so a freshly started host reader sees values within seconds.
- **Left/right is auto-detected.** ZMK assigns peripheral slots by bonding order, not physical side — but key positions are global (left half = columns 0-5 of the dongle transform), so `battery_hid.c` learns each slot's side from the first keypress and always emits the vendor report in true [left, right] order.

**Host side (`host/`, KDE Plasma 6 on Linux):**

- `host/reader/dao-battery-reader.py` — finds the vendor HID interface **by report-descriptor prefix `06 00 ff`** (the `/dev/hidrawN` number is NOT stable across replug/reflash — never hardcode it), blocking-reads reports, writes `~/.local/state/dao-battery.json`.
- `host/systemd/dao-battery-reader.service` — runs the reader as a user service.
- `host/plasmoid/dao-battery/` — Plasma 6 plasmoid. Reads the state file via a `plasma5support` **executable DataSource** (plasmashell blocks `file://` XHR). Compact rep = keyboard glyph flanked by two L/R fill bars; popup mirrors them.
- `host/udev/99-dao-dongle-battery.rules` — grants access via `GROUP="input"` (add user with `usermod -aG input`). NOT via `TAG+="uaccess"`: systemd 258+ has a regression where uaccess ACLs aren't applied to `/dev/hidraw*`.
- `host/install.sh` — installs all of the above; `just widget-install` reinstalls the reader + user service + plasmoid and restarts both.

## Flashing order

Reset settings on **all three** devices first (`settings_reset-*.uf2`), then flash main firmware (`dao_left-zmk.uf2`, `dao_right-zmk.uf2`, `dao_dongle-xiao_ble__zmk-zmk.uf2`). Halves auto-connect to the dongle over BLE. After a settings reset, re-pair by resetting the dongle and a half at nearly the same time. See README.md for the full procedure.
