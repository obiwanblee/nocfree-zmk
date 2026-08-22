/*
 * NocFree "&" three-position output-mode switch.
 *
 * Reproduces the stock firmware's switch behaviour on ZMK:
 *
 *   P0.17 low  -> mode 2, 2.4 GHz
 *   P0.15 low  -> mode 1, Bluetooth (advertises "NocFree &")
 *   neither low -> mode 0, wired (USB HID)
 *
 * Two behaviours of the original are deliberately preserved:
 *
 *   - P0.17 takes precedence. The stock code branches on it first and never
 *     samples P0.15 when it is asserted, so only three of the four encodings
 *     are reachable. If both ever read asserted, 2.4 GHz wins.
 *   - The mode is derived from the pins, never persisted (stock keeps it in
 *     RAM only; no EEPROM path writes it).
 *
 * REBOOT ON SWITCH CHANGE (history inverted 2026-08-11): we originally
 * dropped the stock firmware's reboot-on-change (SCB->AIRCR write after
 * debounce) and re-selected endpoints live, ZMK `&out`-style. The live path
 * MISBEHAVED — it leaves Windows host links up (RPA vs identity) so the
 * dongle never gets a free peripheral slot — so the SHIPPED config sets
 * CONFIG_NOCFREE_MODE_SWITCH_REBOOT=y and the vendor warm reboot is the
 * production behaviour. The live path remains only as the non-REBOOT
 * fallback and for the boot-time apply (which is already a fresh boot).
 *
 * ORDERING -- the part that is easy to get wrong.
 *
 * ZMK persists the endpoint preference under `endpoints/preferred2` and the
 * active BLE profile under `ble/active_profile`. Those are restored by
 * settings_load(), which main() calls AFTER every SYS_INIT level has already
 * run (zmk/app/src/main.c:25-27). So a mode selection made from any SYS_INIT
 * hook -- at any priority, including APPLICATION 99 -- is silently overwritten
 * by flash a moment later. Raising the init priority cannot fix this.
 *
 * The fix is to hang off settings itself: register a handler whose h_commit
 * runs at the end of settings_load(), and apply from there.
 *
 * Commit ORDER between handlers is not left to chance either. Zephyr's
 * settings_commit() (zephyr/subsys/settings/src/settings.c:266-317) walks
 * handlers in ascending `cprio`, and every ZMK handler uses the plain
 * SETTINGS_STATIC_HANDLER_DEFINE macro, which is cprio 0. Registering at
 * cprio 1 therefore guarantees we commit strictly after ZMK's own -- including
 * zmk_ble_complete_startup in ble.c -- rather than hoping a delay is long
 * enough. The remaining short delay is only to move the BLE calls off main()'s
 * context onto the system workqueue; correctness does not depend on it.
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/addr.h>

#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/endpoints_types.h>
#include <zmk/event_manager.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/usb.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define MODE_SWITCH_NODE DT_NODELABEL(mode_switch)

#if !DT_NODE_EXISTS(MODE_SWITCH_NODE)
#error "CONFIG_NOCFREE_MODE_SWITCH is set but no `mode_switch` devicetree node exists."
#endif

static const struct gpio_dt_spec sw_bt = GPIO_DT_SPEC_GET(MODE_SWITCH_NODE, bluetooth_gpios);
static const struct gpio_dt_spec sw_24 = GPIO_DT_SPEC_GET(MODE_SWITCH_NODE, dongle_gpios);

enum nocfree_mode {
    NOCFREE_MODE_WIRED = 0,
    NOCFREE_MODE_BLUETOOTH = 1,
    NOCFREE_MODE_2P4GHZ = 2,
};

static const char *const mode_names[] = {"wired", "bluetooth", "2.4GHz"};

static struct gpio_callback sw_bt_cb;
static struct gpio_callback sw_24_cb;
static struct k_work_delayable apply_work;

/* Set once the first apply has run, so the interrupt path can tell a genuine
 * user flip from the initial settling of the pins. */
static bool applied_once;
static enum nocfree_mode last_mode;
/* Live mode for conn callbacks (2.4G must not let Windows reclaim the link). */
static enum nocfree_mode current_mode = NOCFREE_MODE_WIRED;
/* USB plug/unplug must re-run apply even if the slider did not move (flash
 * sessions and ZMK endpoint restore leave preferred transport stale). */
static bool force_reapply;

static enum nocfree_mode read_mode(void) {
    /* 2.4 GHz is tested first and short-circuits, exactly as the stock decoder
     * does -- it branches on P0.17 before ever sampling P0.15. */
    if (gpio_pin_get_dt(&sw_24) > 0) {
        return NOCFREE_MODE_2P4GHZ;
    }
    if (gpio_pin_get_dt(&sw_bt) > 0) {
        return NOCFREE_MODE_BLUETOOTH;
    }
    return NOCFREE_MODE_WIRED;
}

/*
 * Drop host BLE link(s) — not the split central role (right half).
 *
 * Profile-index disconnect alone is unreliable: Windows often stays linked
 * under an RPA while profiles[] store the identity address, so
 * bt_conn_lookup_addr_le misses and Windows keeps the peripheral slot —
 * BT → 2.4G then looks like "dongle never works" until a power cycle.
 *
 * Walk every LE link and drop those where *we* are the peripheral (host HOG).
 */
static void disconnect_host_conn(struct bt_conn *conn, void *user_data) {
    struct bt_conn_info info;
    char addr[BT_ADDR_LE_STR_LEN];
    int *count = user_data;

    if (bt_conn_get_info(conn, &info) || info.type != BT_CONN_TYPE_LE) {
        return;
    }
    if (info.role != BT_CONN_ROLE_PERIPHERAL) {
        return; /* leave split central → right alone */
    }

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    LOG_INF("mode switch: drop host link %s", addr);
    (void)bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    if (count) {
        (*count)++;
    }
}

static void disconnect_all_peripheral_hosts(void) {
    int n = 0;

    bt_conn_foreach(BT_CONN_TYPE_LE, disconnect_host_conn, &n);
    LOG_INF("mode switch: dropped %d peripheral-role host link(s)", n);
}

static void disconnect_active_host_ble(void) {
    /* USB mode: tear down every host HOG, not only active profile lookup. */
    disconnect_all_peripheral_hosts();
}

static bool addr_matches_profile(const bt_addr_le_t *peer, uint8_t index) {
    bt_addr_le_t *stored;

    if (!peer || index >= ZMK_BLE_PROFILE_COUNT) {
        return false;
    }
    stored = zmk_ble_profile_address(index);
    if (!stored || bt_addr_le_cmp(stored, BT_ADDR_LE_ANY) == 0) {
        return false;
    }
    return bt_addr_le_cmp(stored, peer) == 0;
}

/* Drop only Windows/phone hosts — never the dongle slot peer. */
static void disconnect_non_dongle_host_conn(struct bt_conn *conn, void *user_data) {
    struct bt_conn_info info;
    const bt_addr_le_t *dst;
    char addr[BT_ADDR_LE_STR_LEN];
    int *count = user_data;

    if (bt_conn_get_info(conn, &info) || info.type != BT_CONN_TYPE_LE) {
        return;
    }
    if (info.role != BT_CONN_ROLE_PERIPHERAL) {
        return;
    }

    dst = bt_conn_get_dst(conn);
    if (addr_matches_profile(dst, CONFIG_NOCFREE_MODE_SWITCH_DONGLE_PROFILE)) {
        return; /* keep dongle */
    }

    for (uint8_t i = 0; i < ZMK_BLE_PROFILE_COUNT; i++) {
        if (i == CONFIG_NOCFREE_MODE_SWITCH_DONGLE_PROFILE) {
            continue;
        }
        if (addr_matches_profile(dst, i)) {
            bt_addr_le_to_str(dst, addr, sizeof(addr));
            LOG_INF("mode switch: kick non-dongle host %s (profile %u)", addr, i);
            (void)bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
            if (count) {
                (*count)++;
            }
            return;
        }
    }
}

static void disconnect_non_dongle_hosts(void) {
    int n = 0;

    bt_conn_foreach(BT_CONN_TYPE_LE, disconnect_non_dongle_host_conn, &n);
    if (n) {
        LOG_INF("mode switch: kicked %d non-dongle host(s)", n);
    }
}

/*
 * Short burst of Windows kicks after entering 2.4G (not forever — continuous
 * clear/kick was killing a working dongle link whenever USB re-enumerated).
 */
static struct k_work_delayable kick_hosts_work;
static uint8_t kick_hosts_left;

static void kick_hosts_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    if (current_mode != NOCFREE_MODE_2P4GHZ) {
        kick_hosts_left = 0;
        return;
    }

    disconnect_non_dongle_hosts();
    if (zmk_ble_active_profile_index() != CONFIG_NOCFREE_MODE_SWITCH_DONGLE_PROFILE) {
        (void)zmk_ble_prof_select(CONFIG_NOCFREE_MODE_SWITCH_DONGLE_PROFILE);
    }

    if (kick_hosts_left > 0) {
        kick_hosts_left--;
        k_work_reschedule(&kick_hosts_work, K_MSEC(300));
    }
}

static void schedule_2p4g_host_kicks(void) {
    kick_hosts_left = 5; /* ~100ms then 5 x 300ms */
    k_work_reschedule(&kick_hosts_work, K_MSEC(100));
}

/*
 * 2.4GHz advertising watchdog. ZMK restarts advertising only from BT event
 * callbacks and trusts its cached advertising_status; a single failed
 * bt_le_adv_start (adv restart racing conn cleanup after a disconnect) or a
 * stale status kills the radio silently and permanently (proven 2026-08-11:
 * dongle scanned 4.5 min at a left that stopped advertising after one
 * rejected pair). While parked on the dongle profile and unconnected, force a
 * stop+start cycle every few seconds; silent no-op once the dongle links.
 */
/*
 * QUEUE PLACEMENT IS LOAD-BEARING -- this watchdog MUST stay on the SYSTEM
 * workqueue. Its clear path runs bt_unpair(), which on a peer with a live
 * conn calls bt_conn_disconnect() -> bt_hci_cmd_send_sync(). On this stack
 * (Zephyr 4.1, BT_RECV_WORKQ_BT) send_sync is lethal from the BT RX
 * workqueue (self-wait -> BT_ASSERT -> CPU halt) and empirically unsafe
 * even from a dedicated workqueue. The system workqueue is the ONE thread
 * with an explicit survival path: send_sync detects it and drains the
 * command queue in-place (hci_core.c:410). Do not "clean this up" onto
 * its own queue.
 */
static struct k_work_delayable adv_watchdog_work;

/*
 * Bond-deadlock self-heal (added 2026-08-12, the day it bit four times).
 * The bridge wipes its bonds on every radio start and fresh-pairs, but ZMK
 * only accepts a pair to an OPEN profile; ours re-opens only on full apply
 * (boot / switch flip). So a dongle-only restart (replug, USB hiccup) locks
 * into connect→PAIR_NOT_ALLOWED(err 9)→retry forever, and the user has to
 * power-cycle the left. Heal: after 10 consecutive unconnected ticks (30 s)
 * clear the dongle-slot bond ONCE per unconnected episode — the bridge knocks
 * every ~10 s, so the next attempt pairs fresh. Once-per-episode (not every
 * 30 s) to spare settings-flash writes when no dongle is around; a spurious
 * clear is benign — encryption fails, the bridge drops its bond and
 * fresh-pairs into the now-open slot, which is the designed recovery path.
 */
static uint8_t adv_unconnected_ticks;

/* zmk_ble_active_profile_is_connected() matches by the profile's STORED peer
 * address, which is unset/stale exactly when a fresh pairing is in flight —
 * so "unconnected" does NOT mean "no one is here". Count live peripheral-role
 * connections (host links; the split link is central-role) so the self-heal
 * never wipes a bond out from under an in-progress pairing. */
static void count_peripheral_conns_cb(struct bt_conn *conn, void *data) {
    struct bt_conn_info info;

    if (bt_conn_get_info(conn, &info) == 0 && info.role == BT_CONN_ROLE_PERIPHERAL &&
        (info.state == BT_CONN_STATE_CONNECTED || info.state == BT_CONN_STATE_CONNECTING)) {
        (*(int *)data)++;
    }
}

static bool any_live_peripheral_conn(void) {
    int count = 0;

    bt_conn_foreach(BT_CONN_TYPE_LE, count_peripheral_conns_cb, &count);
    return count > 0;
}

/* "Healthy" for the self-heal counter = connected AND encrypted. During an
 * err-9 pairing loop the connection is transiently up (address matches, state
 * CONNECTED) for seconds per cycle BEFORE security fails — the old counter
 * reset on those phantoms and the self-heal starved forever (2026-08-15 left
 * trial: 300 s deadlocked with the heal never firing). */
static bool active_profile_secured(void) {
    struct bt_conn *conn =
        bt_conn_lookup_addr_le(BT_ID_DEFAULT, zmk_ble_active_profile_addr());
    struct bt_conn_info info;
    bool healthy;

    if (!conn) {
        return false;
    }
    healthy = bt_conn_get_info(conn, &info) == 0 && info.state == BT_CONN_STATE_CONNECTED &&
              bt_conn_get_security(conn) >= BT_SECURITY_L2;
    bt_conn_unref(conn);
    return healthy;
}

static void adv_watchdog_handler(struct k_work *work) {
    ARG_UNUSED(work);

    if (current_mode != NOCFREE_MODE_2P4GHZ) {
        return;
    }
    bool conn = zmk_ble_active_profile_is_connected();
    bool sec = conn && active_profile_secured();
    bool lpc = any_live_peripheral_conn();

    if (conn && sec) {
        adv_unconnected_ticks = 0;
    } else {
        /* printk (not LOG): survives LOG_MODE_MINIMAL diag builds AND keeper
         * builds with logging off — the 08-15 self-heal no-fire could not be
         * diagnosed because the watchdog's state was invisible. One line per
         * 3 s, only while unhealthy. */
        printk("ADVWDG: conn=%d sec=%d lpc=%d ticks=%u prof=%d\n", conn, sec, lpc,
               adv_unconnected_ticks, zmk_ble_active_profile_index());
        if (!conn) {
            (void)zmk_ble_force_readvertise();
        }
        if (adv_unconnected_ticks < UINT8_MAX) {
            adv_unconnected_ticks++;
        }
        /* Guards (2026-08-14 review): only ever clear the DONGLE slot — a
         * keymap &bt BT_SEL can move the active profile while this watchdog
         * runs, and clearing then would wipe the Windows bond; and never
         * clear mid-connection (the err-9 loop has disconnected gaps every
         * cycle, so a deferred clear still fires within a tick or two). */
        /* Two-stage heal (2026-08-15, after the live no-fire): from 30 s,
         * clear politely — only in a conn-free moment. From 60 s, clear
         * UNCONDITIONALLY: the polite guard can starve if the dongle's
         * ~5 s reconnect cadence phase-locks against this 3 s tick (or conn
         * cleanup lags under churn), and starvation-with-no-escalation is
         * the one failure consistent with the observed no-fire. A mid-
         * pairing clear is safe: that attempt dies, the next knock hits an
         * open slot — precisely the boot-time behavior, proven daily. */
        if (adv_unconnected_ticks >= 10 &&
            zmk_ble_active_profile_index() == CONFIG_NOCFREE_MODE_SWITCH_DONGLE_PROFILE &&
            (!lpc || adv_unconnected_ticks >= 20)) {
            printk("ADVWDG: not-secured %us — CLEARING dongle slot bond (deadlock heal%s)\n",
                   adv_unconnected_ticks * 3, lpc ? ", forced past live conn" : "");
            zmk_ble_clear_bonds();
            /* Counter reset (NOT a once-per-episode latch — that was its own
             * trap: if a clear fires and pairing still fails, one-shot means
             * no second chance until a healthy session that never comes).
             * Reset paces retries at one clear per 30-60 s while stuck:
             * bounded settings wear, guaranteed eventual convergence. */
            adv_unconnected_ticks = 0;
        }
    }
    /* Fixed 3 s cadence. A 30 s "when healthy" backoff was tried 2026-08-18 to
     * cut workqueue contention, but it widened adv-death recovery from ~3 s to
     * ~30 s (a real disconnect where ZMK's own readvertise also fails now waits
     * out this sleep) and produced a complete disconnect the same day. The
     * backoff saved only ~18 wakes/min; the backlight-poll trim (1 s -> 5 s)
     * carries the contention win without touching recovery latency. Reverted. */
    k_work_reschedule(&adv_watchdog_work, K_SECONDS(3));
}

static void start_adv_watchdog(void) { k_work_reschedule(&adv_watchdog_work, K_SECONDS(3)); }

static void stop_adv_watchdog(void) { (void)k_work_cancel_delayable(&adv_watchdog_work); }

static void stop_2p4g_host_kicks(void) {
    kick_hosts_left = 0;
    (void)k_work_cancel_delayable(&kick_hosts_work);
}

/*
 * While in 2.4GHz, reject hosts bonded on other profiles (Windows/phone).
 * Do not reject "unknown" peers — the dongle often uses an RPA that will not
 * byte-match the stored identity, and that check broke 2.4G completely.
 */
static void mode_switch_host_connected(struct bt_conn *conn, uint8_t err) {
    struct bt_conn_info info;
    const bt_addr_le_t *dst;
    char addr[BT_ADDR_LE_STR_LEN];
    uint8_t dongle = CONFIG_NOCFREE_MODE_SWITCH_DONGLE_PROFILE;

    if (err || current_mode != NOCFREE_MODE_2P4GHZ) {
        return;
    }

    if (bt_conn_get_info(conn, &info) || info.role != BT_CONN_ROLE_PERIPHERAL) {
        return;
    }

    dst = bt_conn_get_dst(conn);
    bt_addr_le_to_str(dst, addr, sizeof(addr));

    for (uint8_t i = 0; i < ZMK_BLE_PROFILE_COUNT; i++) {
        if (i == dongle) {
            continue;
        }
        if (addr_matches_profile(dst, i)) {
            LOG_INF("mode switch: 2.4GHz — rejecting non-dongle host %s (profile %u)", addr, i);
            bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
            return;
        }
    }
}

BT_CONN_CB_DEFINE(nocfree_mode_switch_conn_cb) = {
    .connected = mode_switch_host_connected,
};

/*
 * Full apply when the physical switch (or boot) selects a mode.
 *
 * 2.4G entry: the dongle-slot bond clear does NOT happen here anymore — it
 * moved to mode_switch_commit() (synchronous, settings-commit time, before
 * any scanner can connect). Clearing here — delayed work — raced the dongle
 * connecting first and yanked its in-flight pairing on deep-sleep wakes.
 * NOTE (2026-08-15): "keep the bond, unauth-overwrite accepts same-address
 * fresh pairs" was tried and is FALSE on this stack (err-9 deadlock,
 * re-proven by the 08-15 left trial) — the slot must genuinely be open
 * before the bridge knocks. light_reassert_mode() stays bond-preserving.
 */
static void apply_mode(enum nocfree_mode mode) {
    int ret = 0;
    int pin_24 = gpio_pin_get_dt(&sw_24);
    int pin_bt = gpio_pin_get_dt(&sw_bt);

    LOG_INF("mode switch: pins 2.4G=%d bt=%d -> %s (active profile was %d)", pin_24, pin_bt,
            mode_names[mode], zmk_ble_active_profile_index());

    current_mode = mode;

    switch (mode) {
    case NOCFREE_MODE_WIRED:
        stop_2p4g_host_kicks();
        stop_adv_watchdog();
        ret = zmk_endpoint_set_preferred_transport(ZMK_TRANSPORT_USB);
        disconnect_active_host_ble();
        break;

    case NOCFREE_MODE_BLUETOOTH:
        stop_2p4g_host_kicks();
        stop_adv_watchdog();
        if (zmk_ble_active_profile_index() == CONFIG_NOCFREE_MODE_SWITCH_DONGLE_PROFILE) {
            ret = zmk_ble_prof_select(CONFIG_NOCFREE_MODE_SWITCH_BT_PROFILE);
            if (ret < 0) {
                LOG_WRN("mode switch: could not leave the dongle profile: %d", ret);
            }
        }
        ret = zmk_endpoint_set_preferred_transport(ZMK_TRANSPORT_BLE);
        break;

    case NOCFREE_MODE_2P4GHZ:
        /* Proven path: free NON-DONGLE host HOGs, park on profile 0, prefer
         * BLE, brief kicks. The dongle-slot bond clear happened already —
         * synchronously in mode_switch_commit() before advertising was
         * connectable — so this delayed apply must NOT clear or disconnect
         * the dongle profile: the bridge may already be mid-pairing with the
         * freshly-opened slot by the time this work item runs, and yanking
         * it here was the inconsistent-wake bug. */
        disconnect_non_dongle_hosts();
        ret = zmk_ble_prof_select(CONFIG_NOCFREE_MODE_SWITCH_DONGLE_PROFILE);
        if (ret < 0) {
            LOG_WRN("mode switch: could not select dongle profile %d: %d",
                    CONFIG_NOCFREE_MODE_SWITCH_DONGLE_PROFILE, ret);
        } else {
            LOG_INF("mode switch: active profile now %d (dongle slot, bond kept)",
                    zmk_ble_active_profile_index());
        }
        ret = zmk_endpoint_set_preferred_transport(ZMK_TRANSPORT_BLE);
        disconnect_non_dongle_hosts();
        schedule_2p4g_host_kicks();
        start_adv_watchdog();
        break;

    default:
        return;
    }

    if (ret < 0) {
        LOG_ERR("mode switch: failed to select %s: %d", mode_names[mode], ret);
        return;
    }

    LOG_INF("mode switch: %s (profile %d)", mode_names[mode], zmk_ble_active_profile_index());
}

/*
 * USB plug/unplug while already on 2.4G must NOT tear down the dongle bond
 * (that was the regression: clear_bonds + disconnect_all on every CDC event).
 * Only re-assert preferred transport and kick known Windows peers.
 */
static void light_reassert_mode(enum nocfree_mode mode) {
    LOG_INF("mode switch: light re-assert %s (USB event, keep bonds)", mode_names[mode]);
    current_mode = mode;

    switch (mode) {
    case NOCFREE_MODE_WIRED:
        stop_adv_watchdog();
        (void)zmk_endpoint_set_preferred_transport(ZMK_TRANSPORT_USB);
        break;
    case NOCFREE_MODE_BLUETOOTH:
        stop_adv_watchdog();
        if (zmk_ble_active_profile_index() == CONFIG_NOCFREE_MODE_SWITCH_DONGLE_PROFILE) {
            (void)zmk_ble_prof_select(CONFIG_NOCFREE_MODE_SWITCH_BT_PROFILE);
        }
        (void)zmk_endpoint_set_preferred_transport(ZMK_TRANSPORT_BLE);
        break;
    case NOCFREE_MODE_2P4GHZ:
        if (zmk_ble_active_profile_index() != CONFIG_NOCFREE_MODE_SWITCH_DONGLE_PROFILE) {
            (void)zmk_ble_prof_select(CONFIG_NOCFREE_MODE_SWITCH_DONGLE_PROFILE);
        }
        (void)zmk_endpoint_set_preferred_transport(ZMK_TRANSPORT_BLE);
        disconnect_non_dongle_hosts();
        schedule_2p4g_host_kicks();
        start_adv_watchdog();
        break;
    default:
        break;
    }
}

static void apply_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    enum nocfree_mode mode = read_mode();
    bool forced = force_reapply;

    force_reapply = false;

    if (applied_once && mode == last_mode && !forced) {
        /* Bounce through intermediate switch states — ignore. */
        return;
    }

    /* Same mode, USB re-enum: light path only (do not kill dongle pair). */
    if (applied_once && forced && mode == last_mode) {
        light_reassert_mode(mode);
        return;
    }

    /* Vendor behaviour: warm reboot on any REAL switch change — regardless of
     * `forced`. A USB conn-state event landing in the same debounce window as
     * a flip (the primary undock gesture: unplug + flip within ~150 ms) used
     * to collapse into one forced run that skipped the reboot and took the
     * live-apply path this option exists to avoid (Windows keeps its RPA
     * links up and the dongle never gets a peripheral slot). */
    if (applied_once && mode != last_mode && IS_ENABLED(CONFIG_NOCFREE_MODE_SWITCH_REBOOT)) {
        LOG_INF("mode switch: %s selected, rebooting to apply", mode_names[mode]);
        sys_reboot(SYS_REBOOT_WARM);
        return;
    }

    if (forced) {
        LOG_INF("mode switch: full re-apply %s (mode change or first boot)", mode_names[mode]);
    }

    last_mode = mode;
    applied_once = true;
    apply_mode(mode);
}

static void sw_isr(const struct device *port, struct gpio_callback *cb, gpio_port_pins_t pins) {
    ARG_UNUSED(port);
    ARG_UNUSED(cb);
    ARG_UNUSED(pins);

    /* Debounce by resubmission: each edge pushes the deadline out, so the work
     * runs once the contact has been quiet for the full window. The stock
     * firmware uses 30 ms plus five samples. */
    k_work_reschedule(&apply_work, K_MSEC(CONFIG_NOCFREE_MODE_SWITCH_DEBOUNCE_MS));
}

/* Runs at the END of settings_load(), so the persisted endpoint preference and
 * active profile have already been restored and cannot overwrite us. */
static int mode_switch_commit(void) {
    /* 2.4G boot: open the dongle slot SYNCHRONOUSLY, here — not in the
     * delayed apply. The bridge fresh-pairs on every radio start, and
     * same-address unauth-overwrite does NOT hold on this stack (every
     * historical err-9 loop; 2026-08-15 left trial re-proved it), so a
     * retained bond deadlocks pairing. Clearing in the delayed apply raced
     * the dongle connecting first and yanked its in-flight pairing (the
     * inconsistent deep-sleep wakes). Here we are cprio 1 = strictly after
     * zmk_ble_complete_startup within the SAME synchronous settings walk:
     * advertising is microseconds old, no scanner can have connected yet —
     * eager-clear reliability with no race window. */
    if (read_mode() == NOCFREE_MODE_2P4GHZ) {
        (void)zmk_ble_prof_select(CONFIG_NOCFREE_MODE_SWITCH_DONGLE_PROFILE);
        zmk_ble_clear_bonds();
        LOG_INF("mode switch: dongle slot cleared at settings-commit (no adv window)");
    }
    k_work_reschedule(&apply_work, K_MSEC(CONFIG_NOCFREE_MODE_SWITCH_APPLY_DELAY_MS));
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE_WITH_CPRIO(nocfree_mode_switch, "nocfree_mode", NULL, NULL,
                                          mode_switch_commit, NULL, 1);

/*
 * After a USB flash/plug session, settings or the USB endpoint stack can leave
 * preferred transport / BLE host state wrong for the *current* slider position.
 * Re-read the pins and force apply_mode() so 2.4G (dongle) recovers without a
 * full power cycle when the user unplugs and the switch is already on 2.4G.
 */
static int mode_switch_usb_listener(const zmk_event_t *eh) {
    const struct zmk_usb_conn_state_changed *ev = as_zmk_usb_conn_state_changed(eh);

    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    force_reapply = true;
    k_work_reschedule(&apply_work, K_MSEC(150));
    LOG_INF("mode switch: USB conn_state=%d — schedule force re-apply", (int)ev->conn_state);
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(nocfree_mode_switch_usb, mode_switch_usb_listener);
ZMK_SUBSCRIPTION(nocfree_mode_switch_usb, zmk_usb_conn_state_changed);

static int configure_pin(const struct gpio_dt_spec *spec, struct gpio_callback *cb,
                         const char *what) {
    int ret;

    if (!gpio_is_ready_dt(spec)) {
        LOG_ERR("mode switch: %s GPIO not ready", what);
        return -ENODEV;
    }

    /* GPIO_PULL_UP comes from the devicetree flags, matching the stock
     * INPUT_PULLUP; the contact shorts the pin to ground. */
    ret = gpio_pin_configure_dt(spec, GPIO_INPUT);
    if (ret < 0) {
        LOG_ERR("mode switch: cannot configure %s: %d", what, ret);
        return ret;
    }

    ret = gpio_pin_interrupt_configure_dt(spec, GPIO_INT_EDGE_BOTH);
    if (ret < 0) {
        /* Not fatal: without interrupts the boot-time read still works, so the
         * switch is honoured on power-up exactly as the stock firmware does.
         * Only live switching is lost. */
        LOG_WRN("mode switch: no interrupt on %s (%d); live switching disabled", what, ret);
        return 0;
    }

    gpio_init_callback(cb, sw_isr, BIT(spec->pin));
    ret = gpio_add_callback(spec->port, cb);
    if (ret < 0) {
        LOG_WRN("mode switch: cannot add callback for %s: %d", what, ret);
    }

    return 0;
}

static int mode_switch_init(void) {
    int ret;

    k_work_init_delayable(&apply_work, apply_work_handler);
    k_work_init_delayable(&kick_hosts_work, kick_hosts_work_handler);
    k_work_init_delayable(&adv_watchdog_work, adv_watchdog_handler);

    ret = configure_pin(&sw_24, &sw_24_cb, "2.4GHz");
    if (ret < 0) {
        return ret;
    }

    ret = configure_pin(&sw_bt, &sw_bt_cb, "bluetooth");
    if (ret < 0) {
        return ret;
    }

    /* Deliberately NOT selecting the endpoint here -- see the ordering note at
     * the top of this file. settings_load() has not run yet and would overwrite
     * anything set now. mode_switch_commit() does the first apply. */
    LOG_DBG("mode switch: pins configured, deferring selection until settings load");
    return 0;
}

SYS_INIT(mode_switch_init, APPLICATION, CONFIG_NOCFREE_MODE_SWITCH_INIT_PRIORITY);
