# Add custom source files to the build
# Only compile startup_layer.c for central/dongle (not for peripheral keyboard halves)
if(CONFIG_ZMK_SPLIT_ROLE_CENTRAL OR NOT CONFIG_ZMK_SPLIT)
  target_sources(app PRIVATE src/startup_layer.c)
endif()
