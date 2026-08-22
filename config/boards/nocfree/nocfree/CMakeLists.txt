# Board-provided sources. Zephyr compiles these into the application for any of the
# three NocFree boards.
#
# recovery.c is not optional on this hardware: the halves have no reset pinhole and
# the dongle has no keys, so without it a build that boots with a broken kscan is
# unrecoverable. See ../../../../RECOVERY.md.

zephyr_library()
zephyr_library_sources_ifdef(CONFIG_NOCFREE_USB_RECOVERY src/recovery.c)
zephyr_library_sources_ifdef(CONFIG_NOCFREE_EXPANDER_POLARITY_CLEAR src/expander_polarity_clear.c)
# Always compile; DT_INST_FOREACH is empty when the node is disabled.
zephyr_library_sources(src/expander_int_wake.c)
zephyr_library_sources_ifdef(CONFIG_NOCFREE_TRIAL_AUTODFU src/trial_autodfu.c)
zephyr_library_sources_ifdef(CONFIG_NOCFREE_CLOCK_PROBE src/clock_probe.c)
zephyr_library_sources_ifdef(CONFIG_NOCFREE_KEY_SELFTEST src/key_selftest.c)
zephyr_library_sources_ifdef(CONFIG_NOCFREE_BACKLIGHT_PROBE src/backlight_probe.c)
zephyr_library_sources_ifdef(CONFIG_NOCFREE_BACKLIGHT_SYNC src/split_backlight_sync.c)
zephyr_library_sources_ifdef(CONFIG_NOCFREE_BACKLIGHT_BOOT_ON src/backlight_boot_on.c)
zephyr_library_sources_ifdef(CONFIG_NOCFREE_BATTERY_BAS_MIN src/battery_bas_min.c)
# Pure-Zephyr APIs (settings/nvs); needs no zmk includes.
zephyr_library_sources_ifdef(CONFIG_NOCFREE_NVS_BOOT_GC src/nvs_boot_gc.c)

# key_selftest.c and split_backlight_sync.c call into ZMK proper
# (zmk/event_manager.h, zmk/split/central.h). Same include gap as
# mode_switch.c below: board libraries don't get zmk/app/include.
if(CONFIG_NOCFREE_KEY_SELFTEST OR CONFIG_NOCFREE_MODE_SWITCH OR CONFIG_NOCFREE_BACKLIGHT_SYNC OR CONFIG_NOCFREE_BACKLIGHT_BOOT_ON OR CONFIG_NOCFREE_BATTERY_BAS_MIN)
  zephyr_library_include_directories(${APPLICATION_SOURCE_DIR}/include)
endif()
zephyr_library_sources_ifdef(CONFIG_NOCFREE_MODE_SWITCH src/mode_switch.c)

# (mode_switch.c's include note: board libraries get zmk/app/module/include
# but NOT zmk/app/include; the block above covers both files.)

# These boards have no reset button (halves) and no keys (dongle). If the USB
# recovery shim is ever configured out, a build that boots with broken kscan is
# unrecoverable. Fail loudly at configure time rather than shipping that.
#
# Double-tap DFU is NOT "nRF52840-only" (Adafruit bootloader implements it for
# nRF52833 too) -- but it needs a PIN reset of the RESET net with power held,
# and these cases expose no pinhole. Software recovery is still mandatory.
if(NOT CONFIG_NOCFREE_USB_RECOVERY)
  message(FATAL_ERROR
    "CONFIG_NOCFREE_USB_RECOVERY is not set. These boards have no exposed "
    "reset pinhole and the dongle has no keys, so this shim is the only "
    "software path back into the bootloader. Refusing to build.")
endif()

# The 2026-08-09 brick: LOG_MODE_IMMEDIATE + LOG_BACKEND_UART into a CDC
# console that is not yet USB-enabled, without the trial watchdog. Refuse
# that combination at configure time so it cannot ship as a keeper.
if(CONFIG_LOG_MODE_IMMEDIATE AND CONFIG_LOG_BACKEND_UART AND NOT CONFIG_NOCFREE_TRIAL_AUTODFU)
  message(FATAL_ERROR
    "CONFIG_LOG_MODE_IMMEDIATE + CONFIG_LOG_BACKEND_UART without "
    "CONFIG_NOCFREE_TRIAL_AUTODFU. That combination halted the right half "
    "pre-USB on 2026-08-09. Use ./build.sh --diag (forces trial) or drop "
    "the UART log backend. See diag.conf and RECOVERY_RIGHT.md.")
endif()

# The bluetooth position must not silently land on the dongle's profile.
if(CONFIG_NOCFREE_MODE_SWITCH AND
   CONFIG_NOCFREE_MODE_SWITCH_BT_PROFILE EQUAL CONFIG_NOCFREE_MODE_SWITCH_DONGLE_PROFILE)
  message(FATAL_ERROR
    "NOCFREE_MODE_SWITCH_BT_PROFILE and NOCFREE_MODE_SWITCH_DONGLE_PROFILE are "
    "both ${CONFIG_NOCFREE_MODE_SWITCH_BT_PROFILE}. The bluetooth and 2.4GHz "
    "switch positions would select the same BLE profile, so the switch could "
    "not distinguish them.")
endif()
