/*
 * Copyright (c) 2025
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/init.h>
#include <zmk/keymap.h>

/**
 * Auto-activate COLEMAK layer on startup
 *
 * This function is called during system initialization to automatically
 * activate layer 1 (COLEMAK) when the keyboard boots up.
 */
static int activate_startup_layer(void) {
    // Activate COLEMAK (layer 1) on boot
    // false = non-locking activation (can be toggled off by macros)
    zmk_keymap_layer_activate(1, false);

    return 0;
}

// Run at APPLICATION init level, same priority as keymap_init
SYS_INIT(activate_startup_layer, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
