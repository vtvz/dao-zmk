# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

ZMK firmware config for the **Dao** split keyboard (nRF52840 halves) with a **dongle** (XIAO BLE) acting as BLE central. Self-contained: all board/shield definitions live in `config/boards/`, no external ZMK module dependency.

## Research before deciding

Web-search before changing config or asserting a root cause. ZMK/Zephyr change between versions, and most issues are already solved upstream — verify, don't guess from local greps.

## Commands

All builds run inside the `zmkfirmware/zmk-build-arm:stable` Docker image via `just`. Requires Docker, `just`, `yq`, `jq`.

```bash
just init                          # First-time only: clone zmk, west init/update (populates gitignored deps)
just build                         # Build every target in build.yaml, output to build/*.uf2
just build-part dao_left           # Build a single board (no shield)
just build-part xiao_ble dao_dongle    # Build a board + shield
just build-part dao_left settings_reset
```

`just build` wipes `build/*.uf2` first, reads the matrix from `build.yaml`, and calls `build-part` for each entry. Output naming: `<shield>-<board>-zmk.uf2`, or `<board>-zmk.uf2` when there's no shield.

There are no tests or linters in this repo — validation is "does it build."

## Build dependencies (gitignored, not source)

`zmk/`, `zephyr/`, `modules/`, `.west/`, `ergonautkb-zmk-module/`, and `build/` are all gitignored and populated by `just init` / west. **Do not edit them** — they are upstream checkouts. The only source lives in `config/`, plus the top-level `Justfile`, `build.yaml`, and `.github/workflows/build.yml`. `config/west.yml` pins zmk to `revision: main`.

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

## Flashing order

Reset settings on **all three** devices first (`settings_reset-*.uf2`), then flash main firmware (`dao_left-zmk.uf2`, `dao_right-zmk.uf2`, `dao_dongle-xiao_ble-zmk.uf2`). Halves auto-connect to the dongle over BLE. See README.md for the full procedure.