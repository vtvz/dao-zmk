# Add custom source files to the build
# Only compile startup_layer.c for central/dongle (not for peripheral keyboard halves)
if(CONFIG_ZMK_SPLIT_ROLE_CENTRAL OR NOT CONFIG_ZMK_SPLIT)
  target_sources(app PRIVATE src/startup_layer.c)
endif()

# Custom USB HID interface exposing per-half battery to the host (dongle only).
# Gated on the flag defined in the dao_dongle shield's Kconfig.defconfig.
target_sources_ifdef(CONFIG_ZMK_SPLIT_BATTERY_HID_REPORT app PRIVATE src/battery_hid.c)
