/*
 * Boot-time NVS garbage-collection pre-pay.
 *
 * THE PROBLEM: the settings store is NVS -- append-only 4 KB sectors; when
 * the active sector fills, GC copies live entries forward and ERASES the old
 * sector. A page erase is ~85 ms ATOMIC, and under
 * SOC_FLASH_NRF_RADIO_SYNC_TICKER it needs one contiguous radio-free slot of
 * that length -- which a schedule running a 7.5 ms split link (plus the 15 ms
 * host link on the left) NEVER has. So a mid-session GC can only time out
 * (~90 ms per attempt) and thrash, degrading the radio schedule and blocking
 * the system workqueue that asked (settings/BT stores -- the same workqueue
 * the adv self-heal watchdog lives on). Mid-session writes cluster in the
 * minutes after a deep-sleep wake (bond rewrite + CCC + backlight), which is
 * exactly when the sector tips over.
 *
 * THE FIX: never let GC arm mid-session. Every boot, BEFORE the radio comes
 * up (SYS_INIT prio < ZMK_BLE_INIT_PRIORITY=50, so bt_enable has not run and
 * flash ops need no timeslot at all), check the NVS free space; if the
 * session budget is not available, force the rotation NOW by padding a
 * scratch settings key until NVS garbage-collects, then delete the scratch.
 * Sessions then always start with guaranteed headroom, and the erase that
 * can never be granted mid-session never needs to be.
 *
 * Uses only public API: settings_storage_get() hands back the backend's
 * struct nvs_fs (settings_nvs.c), nvs_calc_free_space() measures, and the
 * padding goes through settings_save_one/settings_delete so no NVS id
 * arithmetic can collide with the settings backend's internals.
 *
 * Wear note: the padding only runs when the sector was about to rotate
 * anyway -- this MOVES the same erase to boot, it does not add erases.
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/sys/printk.h>
#include <zephyr/settings/settings.h>
#include <zephyr/fs/nvs.h>

#define GC_SCRATCH_KEY "nocfree/gcpad"
#define GC_PAD_LEN 128
#define GC_MAX_ROUNDS 40 /* 40*~136B > one 4 KB sector: always enough to tip */

static int nvs_boot_gc_init(void)
{
	struct nvs_fs *fs = NULL;
	ssize_t free_before, free_now;
	uint8_t pad[GC_PAD_LEN];
	int rounds = 0;
	int err;

	/* Idempotent; makes the backend usable this early. */
	err = settings_subsys_init();
	if (err) {
		printk("nvsgc: settings init err %d — skipping\n", err);
		return 0; /* never fail boot for this */
	}

	err = settings_storage_get((void **)&fs);
	if (err || fs == NULL) {
		printk("nvsgc: no storage handle (err %d) — skipping\n", err);
		return 0;
	}

	free_before = nvs_calc_free_space(fs);
	if (free_before < 0) {
		printk("nvsgc: free-space query err %d — skipping\n", (int)free_before);
		return 0;
	}

	if (free_before >= CONFIG_NOCFREE_NVS_BOOT_GC_MIN_FREE) {
		return 0; /* headroom fine; touch nothing */
	}

	memset(pad, 0xa5, sizeof(pad));
	free_now = free_before;
	while (free_now < CONFIG_NOCFREE_NVS_BOOT_GC_MIN_FREE &&
	       rounds < GC_MAX_ROUNDS) {
		err = settings_save_one(GC_SCRATCH_KEY, pad, sizeof(pad));
		if (err) {
			/* An erase failure here (radio off) would be real
			 * hardware trouble; say so and stop — boot goes on. */
			printk("nvsgc: pad write err %d at round %d\n", err, rounds);
			break;
		}
		rounds++;
		free_now = nvs_calc_free_space(fs);
		if (free_now < 0) {
			break;
		}
	}

	(void)settings_delete(GC_SCRATCH_KEY);

	printk("nvsgc: free %d -> %d after %d pad rounds (GC pre-paid at boot)\n",
	       (int)free_before, (int)free_now, rounds);
	return 0;
}

SYS_INIT(nvs_boot_gc_init, APPLICATION, CONFIG_NOCFREE_NVS_BOOT_GC_INIT_PRIORITY);
