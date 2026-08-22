#!/usr/bin/env bash
# Runs INSIDE the zmk-build-arm container. RIGHT keeper only.
set -euo pipefail
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
cd /workspace
west zephyr-export

board=nocfree_right/nrf52833/zmk
name=nocfree_right
bdir="/tmp/zmk-build/$name"

# Vendored-patch integrity (pre-build, fail fast): see the left keeper script.
# A `west update` silently reverts the zmk/ local patches; an unknown defconfig
# symbol is only a warning, so the keeper would build green without the fix.
grep -q 'KSCAN_DIRECT_RESCHEDULE' /workspace/zmk/app/module/drivers/kscan/kscan_gpio_direct.c || {
  echo "!! zmk/ kscan dedicated-workqueue patch MISSING — re-apply patches/zmk-local-patches.patch"; exit 1; }
grep -q 'zmk_activity_note' /workspace/zmk/app/src/split/peripheral.c || {
  echo "!! zmk/ activity-sync peripheral patch MISSING (right would idle/deep-sleep mid-session)"; exit 1; }
grep -q 'ZMK_SPLIT_ROLE_PERIPHERAL' /workspace/zmk/app/src/backlight.c || {
  echo "!! zmk/ backlight peripheral-persistence patch MISSING (dark-boot-after-sleep returns)"; exit 1; }
grep -q 'SPLITDROP' /workspace/zmk/app/src/split/bluetooth/service.c || {
  echo "!! zmk/ split notify-drop trace MISSING (lost position state would be silent again)"; exit 1; }
echo "== building $board (out=$bdir) =="
rm -rf "$bdir"
mkdir -p "$bdir"

west build -p -s zmk/app -b "$board" -d "$bdir" \
  -- -DZMK_CONFIG=/workspace/config

CFG="$bdir/zephyr/.config"
grep -q '^CONFIG_NOCFREE_USB_RECOVERY=y' "$CFG" || { echo "!! missing USB recovery"; exit 1; }
if grep -q '^CONFIG_NOCFREE_TRIAL_AUTODFU=y' "$CFG"; then echo "!! trial autodfu in keeper"; exit 1; fi
if grep -q '^CONFIG_LOG_MODE_IMMEDIATE=y' "$CFG" && grep -q '^CONFIG_LOG_BACKEND_UART=y' "$CFG"; then
  echo "!! brick combo in keeper"; exit 1
fi
grep -q '^CONFIG_NOCFREE_EXPANDER_POLARITY_CLEAR=y' "$CFG" || { echo "!! polarity clear missing"; exit 1; }
grep -q '^CONFIG_ZMK_BACKLIGHT_AUTO_OFF_IDLE=y' "$CFG" || { echo "!! backlight idle-off missing"; exit 1; }
grep -q '^CONFIG_ZMK_BACKLIGHT_ON_START=y' "$CFG" || { echo "!! BACKLIGHT_ON_START missing"; exit 1; }
grep -q '^CONFIG_NOCFREE_BACKLIGHT_BOOT_ON=y' "$CFG" || { echo "!! backlight boot-on missing (ON_START alone boots dark — proven 08-15)"; exit 1; }
grep -q '^CONFIG_ZMK_PM_SOFT_OFF=y' "$CFG" || { echo "!! soft-off missing"; exit 1; }
grep -q '^CONFIG_ZMK_SLEEP=y' "$CFG" || { echo "!! deep sleep missing"; exit 1; }
grep -q '^CONFIG_ZMK_IDLE_SLEEP_TIMEOUT=1800000$' "$CFG" || { echo "!! deep sleep not 30 min"; exit 1; }
grep -q '^CONFIG_ZMK_IDLE_TIMEOUT=300000$' "$CFG" || { echo "!! idle timeout not 5 min"; exit 1; }
if grep -q '^CONFIG_ZMK_BLE_CLEAR_BONDS_ON_START=y' "$CFG"; then
  echo "!! CLEAR_BONDS_ON_START in keeper"; exit 1
fi
grep -q '^CONFIG_ZMK_KSCAN_DEDICATED_WORKQUEUE=y' "$CFG" || {
  echo "!! kscan dedicated workqueue missing (stuck-key/disconnect-cascade fix)"; exit 1; }
grep -q '^CONFIG_NOCFREE_NVS_BOOT_GC=y' "$CFG" || {
  echo "!! NVS boot GC missing (mid-session GC erase starves the radio schedule)"; exit 1; }
# This half is the notifier, so its TX pool depth is the one that matters.
grep -q '^CONFIG_BT_CONN_TX_MAX=8$' "$CFG" || { echo "!! right BT_CONN_TX_MAX not 8 (split notify drop)"; exit 1; }
grep -q '^CONFIG_BT_BUF_ACL_TX_COUNT=8$' "$CFG" || { echo "!! right BT_BUF_ACL_TX_COUNT not 8"; exit 1; }
grep -q '^CONFIG_BT_L2CAP_TX_BUF_COUNT=8$' "$CFG" || { echo "!! right BT_L2CAP_TX_BUF_COUNT not 8"; exit 1; }
# bcklight must survive omit-if-no-ref (BLFIX root cause).
if ! grep -q 'bcklight' "$bdir/zephyr/zephyr.dts" 2>/dev/null; then
  # fallback: strings in uf2 after build — check dts first
  if ! grep -qi 'bcklight\|behavior-backlight' "$bdir/zephyr/zephyr.dts"; then
    echo "!! bcklight behavior missing from dts (KEEP_BL?)"; exit 1
  fi
fi

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
# prove bcklight in dts (authoritative) and best-effort in UF2 strings
# (bind-mount race can make strings miss immediately after cp).
if ! grep -q 'bcklight' "$bdir/zephyr/zephyr.dts"; then
  echo "!! bcklight missing from zephyr.dts"; exit 1
fi
if strings "/workspace/${name}.uf2" | grep -q bcklight; then
  echo "bcklight present in UF2 OK"
else
  echo "warn: bcklight not seen in UF2 strings yet (dts has it); ok if bind-mount lag"
fi
echo "== right keeper done =="
