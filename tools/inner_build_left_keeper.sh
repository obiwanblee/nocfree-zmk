#!/usr/bin/env bash
# Runs INSIDE the zmk-build-arm container. LEFT keeper only (no right flash
# path). Same gates as inner_build_keepers.sh for the left half.
set -euo pipefail
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
cd /workspace
west zephyr-export

board=nocfree_left/nrf52833/zmk
name=nocfree_left
bdir="/tmp/zmk-build/$name"

# Vendored-patch integrity (pre-build, fail fast): the load-bearing fixes live
# as LOCAL patches applied to the zmk/ west tree (patches/zmk-local-patches.patch),
# which is NOT tracked here. A `west update` or fresh checkout silently reverts
# them, and an unknown Kconfig symbol in a defconfig is only a WARNING — so the
# keeper would build green with the fix missing. These sentinels exist only in
# patched sources.
grep -q 'KSCAN_DIRECT_RESCHEDULE' /workspace/zmk/app/module/drivers/kscan/kscan_gpio_direct.c || {
  echo "!! zmk/ kscan dedicated-workqueue patch MISSING — re-apply patches/zmk-local-patches.patch"; exit 1; }
grep -q 'CONFIG_BOARD_NOCFREE_LEFT' /workspace/zmk/app/src/endpoints.c || {
  echo "!! zmk/ endpoints no-USB-fallback patch MISSING (BT/2.4G would leak HID to USB)"; exit 1; }
grep -q 'zmk_ble_force_readvertise' /workspace/zmk/app/src/ble.c || {
  echo "!! zmk/ force-readvertise patch MISSING (adv watchdog would not link)"; exit 1; }
grep -q 'HOGDROP' /workspace/zmk/app/src/hog.c || {
  echo "!! zmk/ HOGDROP patch MISSING — re-apply patches/zmk-local-patches.patch"; exit 1; }
echo "== building $board (out=$bdir) =="
rm -rf "$bdir"
mkdir -p "$bdir"

# Studio needs the studio-rpc-usb-uart snippet (second CDC ACM + KEEP_ALL flag;
# keymap undefs KEEP_ALL for flash size — see nocfree_left.keymap).
west build -p -s zmk/app -b "$board" -d "$bdir" \
  -S studio-rpc-usb-uart \
  -- -DZMK_CONFIG=/workspace/config

CFG="$bdir/zephyr/.config"
grep -q '^CONFIG_NOCFREE_USB_RECOVERY=y' "$CFG" || { echo "!! missing USB recovery"; exit 1; }
if grep -q '^CONFIG_NOCFREE_TRIAL_AUTODFU=y' "$CFG"; then echo "!! trial autodfu in keeper"; exit 1; fi
if grep -q '^CONFIG_LOG_MODE_IMMEDIATE=y' "$CFG" && grep -q '^CONFIG_LOG_BACKEND_UART=y' "$CFG"; then
  echo "!! brick combo in keeper"; exit 1
fi
grep -q '^CONFIG_NOCFREE_EXPANDER_POLARITY_CLEAR=y' "$CFG" || { echo "!! polarity clear missing"; exit 1; }
if grep -q '^CONFIG_BT_CTLR_PHY_2M=y' "$CFG"; then
  echo "!! left keeper has PHY 2M on -- Windows pair regression"; exit 1
fi
if grep -q '^CONFIG_ZMK_BLE_PASSKEY_ENTRY=y' "$CFG"; then
  echo "!! left keeper has passkey entry"; exit 1
fi
grep -q '^CONFIG_BT_SMP_ALLOW_UNAUTH_OVERWRITE=y' "$CFG" || {
  echo "!! missing unauth overwrite (proven with Windows pairfix)"; exit 1
}
grep -q '^CONFIG_ZMK_BACKLIGHT=y' "$CFG" || { echo "!! backlight missing"; exit 1; }
grep -q '^CONFIG_NOCFREE_BACKLIGHT_SYNC=y' "$CFG" || { echo "!! bl sync missing"; exit 1; }
grep -q '^CONFIG_ZMK_BACKLIGHT_AUTO_OFF_IDLE=y' "$CFG" || { echo "!! backlight idle-off missing"; exit 1; }
grep -q '^CONFIG_ZMK_PM_SOFT_OFF=y' "$CFG" || { echo "!! soft-off missing"; exit 1; }
grep -q '^CONFIG_ZMK_SLEEP=y' "$CFG" || { echo "!! deep sleep missing"; exit 1; }
# Value greps are $-anchored: unanchored '=2' happily matched '=20' etc.
grep -q '^CONFIG_ZMK_IDLE_SLEEP_TIMEOUT=1800000$' "$CFG" || { echo "!! deep sleep not 30 min"; exit 1; }
grep -q '^CONFIG_BT_PERIPHERAL_PREF_LATENCY=2$' "$CFG" || { echo "!! host-link latency not 2 (matches dongle pin, key-order fix)"; exit 1; }
grep -q '^CONFIG_BT_PERIPHERAL_PREF_MIN_INT=12$' "$CFG" || { echo "!! host-link interval not 15 ms (matches dongle pin, key-order fix)"; exit 1; }
grep -q '^CONFIG_ZMK_SPLIT_BLE_PREF_LATENCY=1$' "$CFG" || { echo "!! split-link latency not 1 (key-order fix)"; exit 1; }
grep -q '^CONFIG_ZMK_STUDIO=y' "$CFG" || { echo "!! ZMK Studio missing on left"; exit 1; }
grep -q '^CONFIG_ZMK_IDLE_TIMEOUT=300000$' "$CFG" || { echo "!! idle timeout not 5 min"; exit 1; }
if grep -q '^CONFIG_ZMK_BLE_CLEAR_BONDS_ON_START=y' "$CFG"; then
  echo "!! CLEAR_BONDS_ON_START in keeper"; exit 1
fi
grep -q '^CONFIG_ZMK_KSCAN_DEDICATED_WORKQUEUE=y' "$CFG" || {
  echo "!! kscan dedicated workqueue missing (stuck-key/disconnect-cascade fix)"; exit 1; }
grep -q '^CONFIG_NOCFREE_NVS_BOOT_GC=y' "$CFG" || {
  echo "!! NVS boot GC missing (mid-session GC erase starves the radio schedule)"; exit 1; }
grep -q '^CONFIG_NOCFREE_ACTIVITY_SYNC=y' "$CFG" || {
  echo "!! activity sync missing (right idles/deep-sleeps mid-session without it)"; exit 1; }
# Split-link drop hardening: deeper TX pool, and a central position queue big
# enough to absorb the release-everything burst on peripheral disconnect.
grep -q '^CONFIG_BT_BUF_ACL_TX_COUNT=8$' "$CFG" || { echo "!! left BT_BUF_ACL_TX_COUNT not 8"; exit 1; }
grep -q '^CONFIG_BT_L2CAP_TX_BUF_COUNT=8$' "$CFG" || { echo "!! left BT_L2CAP_TX_BUF_COUNT not 8"; exit 1; }
grep -q '^CONFIG_ZMK_SPLIT_BLE_CENTRAL_POSITION_QUEUE_SIZE=16$' "$CFG" || {
  echo "!! left central position queue not 16 (disconnect release burst drops)"; exit 1; }
mkdir -p /workspace/releases /workspace/build_export/$name
cp "$bdir/zephyr/zmk.uf2" "/workspace/${name}.uf2"
cp "$bdir/zephyr/zmk.hex" "/workspace/${name}.hex"
cp "$CFG" "/workspace/${name}.config"
cp "$bdir/zephyr/zmk.uf2" "$bdir/zephyr/zmk.hex" "$CFG" \
   "/workspace/build_export/$name/"
# Rename on copy — a bare multi-source cp kept the zmk.uf2 basename, so
# releases/ never actually got updated keeper names (2026-08-12 stale-flash).
cp "$bdir/zephyr/zmk.uf2" "/workspace/releases/${name}.uf2"
cp "$CFG" "/workspace/releases/${name}.config"
cp "$bdir/zephyr/zmk.hex" "/workspace/releases/${name}.hex" 2>/dev/null || true
echo "-> ${name}.uf2 ($(stat -c%s /workspace/${name}.uf2) bytes) + releases/"
echo "== left keeper done =="
