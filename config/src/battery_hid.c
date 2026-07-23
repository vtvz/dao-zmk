/*
 * Exposes the left and right split-half battery levels of a ZMK split
 * keyboard to the host over USB, since ZMK only reports battery over BLE.
 * The dongle (central) presents two extra USB HID interfaces:
 *
 *   HID_1 — vendor interface (Usage Page 0xFF00). Pushes a 3-byte INPUT
 *           report [0x01, left%, right%] (0xFF = unknown) on every peripheral
 *           battery event AND every RESEND_INTERVAL_S seconds, so a host
 *           reader that (re)connects sees values within one interval. Read by
 *           our host reader/widget, which finds it by VID:PID + the
 *           report-descriptor prefix 06 00 ff.
 *
 *   HID_2 — standard battery: the Generic Device Controls "Battery Strength"
 *           usage (0x06/0x20) in an INPUT report, wrapped in a minimal dummy
 *           keyboard (see below). The Linux kernel's hid-input driver auto-maps
 *           it to a power_supply entry (visible in upower / the system tray)
 *           like a wireless mouse/keyboard. HID has no notion of two batteries,
 *           so this single value is the WORSE of the two halves (min), i.e.
 *           "charge when this hits low". Pushed on change; the kernel also
 *           polls it via GET_REPORT (feature) at enumeration, so it needs no
 *           periodic resend. While both halves are still unknown nothing is
 *           sent/answered — reporting 0% would trigger false low-battery
 *           warnings on the host.
 *
 * NOTE on left/right: ZMK assigns peripheral slot indices by bonding order,
 * not by physical side — after re-pairing in the other order, slot 0 is the
 * right half. The mapping is AUTO-DETECTED from keystrokes: key positions are
 * global (the dongle's default_transform gives the left half columns 0-5), so
 * the first keypress from a slot reveals its side and the vendor report is
 * emitted in true [left, right] order from then on.
 *
 * Dongle (central) build only. Requires CONFIG_USB_HID_DEVICE_COUNT=3.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/device.h>
#include <zephyr/sys/iterable_sections.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/usb/class/usb_hid.h>
#include <zephyr/logging/log.h>

#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/split/central.h>
#include <zmk/split/transport/central.h>

LOG_MODULE_REGISTER(zmk_split_battery, CONFIG_ZMK_LOG_LEVEL);

#define REPORT_ID_BATTERY 0x01
#define BATTERY_UNKNOWN   0xFF

/* HID report type lives in the high byte of GET_REPORT's wValue; 0x03 = feature.
 * (Zephyr's legacy usb_hid.h doesn't export a constant for this.) */
#define HID_GET_REPORT_TYPE_FEATURE 0x03

/* ---- HID_1: vendor L/R input report ------------------------------------- */

static const uint8_t vendor_report_desc[] = {
    0x06, 0x00, 0xFF, /* Usage Page (Vendor-Defined 0xFF00)             */
    0x09, 0x01,       /* Usage (0x01) — split battery device            */
    0xA1, 0x01,       /* Collection (Application)                       */
    0x85, REPORT_ID_BATTERY, /*   Report ID (1)                         */
    0x15, 0x00,       /*   Logical Minimum (0)                          */
    0x26, 0xFF, 0x00, /*   Logical Maximum (255)                        */
    0x75, 0x08,       /*   Report Size (8)                              */
    0x95, 0x01,       /*   Report Count (1)                             */
    0x09, 0x01,       /*   Usage (0x01) — left battery                  */
    0x81, 0x02,       /*   Input (Data, Variable, Absolute)             */
    0x09, 0x02,       /*   Usage (0x02) — right battery                 */
    0x81, 0x02,       /*   Input (Data, Variable, Absolute)             */
    0xC0,             /* End Collection                                 */
};

/* ---- HID_2: standard battery, carried on a dummy keyboard interface ------ */
/*
 * The kernel maps the Battery Strength usage to a power_supply only when the
 * interface ALSO registers a real input device — hidinput_has_been_populated()
 * ignores EV_PWR, so a battery-only interface is torn down and the battery
 * orphaned. A bare Button page inside a GenDevCtrls collection wasn't enough.
 *
 * So we present a minimal *keyboard* (Generic Desktop / Keyboard — which
 * hid-generic always turns into an input device) and append the Battery
 * Strength usage in the same input report. The keyboard reports only its
 * modifier byte (always 0, no key can be pressed); it exists solely so the
 * input device registers and the battery survives. A keyboard (vs a mouse) is
 * used so the phantom device reads as a keyboard battery — semantically apt.
 *
 * INPUT report layout (Report ID 2): [modifiers(1B, always 0), reserved(1B,0),
 * battery%(1B)]. A Feature copy of just the battery byte is also offered.
 */
#define REPORT_ID_STDBATT 0x02

static const uint8_t battery_report_desc[] = {
    0x05, 0x01,       /* Usage Page (Generic Desktop)                   */
    0x09, 0x06,       /* Usage (Keyboard)                               */
    0xA1, 0x01,       /* Collection (Application)                       */
    0x85, REPORT_ID_STDBATT, /*   Report ID (2)                         */
    /* Modifier byte (8 bits, always 0) — makes hid-generic build an input dev */
    0x05, 0x07,       /*   Usage Page (Keyboard/Keypad)                 */
    0x19, 0xE0,       /*   Usage Minimum (Left Control)                 */
    0x29, 0xE7,       /*   Usage Maximum (Right GUI)                    */
    0x15, 0x00,       /*   Logical Minimum (0)                          */
    0x25, 0x01,       /*   Logical Maximum (1)                          */
    0x75, 0x01,       /*   Report Size (1)                              */
    0x95, 0x08,       /*   Report Count (8)                             */
    0x81, 0x02,       /*   Input (Data,Var,Abs) — modifiers             */
    0x75, 0x08,       /*   Report Size (8)                              */
    0x95, 0x01,       /*   Report Count (1)                             */
    0x81, 0x03,       /*   Input (Const) — reserved byte                */
    /* Battery Strength in the same input report */
    0x05, 0x06,       /*   Usage Page (Generic Device Controls)         */
    0x09, 0x20,       /*   Usage (Battery Strength)                     */
    0x15, 0x00,       /*   Logical Minimum (0)                          */
    0x25, 0x64,       /*   Logical Maximum (100)                        */
    0x75, 0x08,       /*   Report Size (8)                              */
    0x95, 0x01,       /*   Report Count (1)                             */
    0x81, 0x02,       /*   Input (Data,Var,Abs)                         */
    0x09, 0x20,       /*   Usage (Battery Strength) — feature copy      */
    0xB1, 0x02,       /*   Feature (Data,Var,Abs)                       */
    0xC0,             /* End Collection (Application)                   */
};

/* Latest state of charge per peripheral slot (slot order = bonding order). */
static uint8_t levels[2] = { BATTERY_UNKNOWN, BATTERY_UNKNOWN };

/* Which slot is the physical LEFT half. Defaults to 0 and is auto-corrected on
 * the first keypress from either half (see position listener below). */
static uint8_t left_slot;

/* Key positions belonging to the LEFT half, as a bitmask over the position
 * indices of the dongle's default_transform (dao_dongle.overlay): each row
 * lists 6-7 left-column (0-5) positions, then the mirrored right block.
 *   row 0: positions 0-6,   row 1: 14-19,   row 2: 26-31,   thumbs: 38-40 */
#define LEFT_POS_MASK \
    ((0x7FULL << 0) | (0x3FULL << 14) | (0x3FULL << 26) | (0x7ULL << 38))

static bool position_is_left(uint32_t position)
{
    return position < 64 && ((LEFT_POS_MASK >> position) & 1);
}

/* Combined level for the standard interface: the worse of the two known
 * halves, so the host shows "time to charge" for whichever needs it first.
 * Returns BATTERY_UNKNOWN while neither half has reported. */
static uint8_t combined_level(void)
{
    uint8_t l = levels[0], r = levels[1];
    if (l == BATTERY_UNKNOWN) return r;
    if (r == BATTERY_UNKNOWN) return l;
    return l < r ? l : r;
}

/* Reset slots whose peripheral is currently disconnected back to UNKNOWN.
 * ZMK only raises battery events, never "peripheral gone", so without this a
 * powered-off half would keep showing its last percentage forever. With it,
 * switching a half off makes its widget bar go to "—" within one resend
 * interval — which is also how to verify the L/R mapping: turn one half off
 * and see which bar drops. Uses the public split-transport API only. */
static void refresh_connected_slots(void)
{
    STRUCT_SECTION_FOREACH(zmk_split_transport_central, t) {
        if (!t->api || !t->api->get_available_source_ids) {
            continue;
        }
        if (t->api->get_status && !t->api->get_status().available) {
            continue;
        }

        uint8_t sources[ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT];
        int n = t->api->get_available_source_ids(sources);
        if (n < 0) {
            return; /* transient query failure — keep last known values */
        }

        bool present[ARRAY_SIZE(levels)] = { false };
        for (int i = 0; i < n; i++) {
            if (sources[i] < ARRAY_SIZE(levels)) {
                present[sources[i]] = true;
            }
        }
        for (size_t s = 0; s < ARRAY_SIZE(levels); s++) {
            if (!present[s]) {
                levels[s] = BATTERY_UNKNOWN;
            }
        }
        return; /* only one split transport is active in this build (BLE) */
    }
}

/* ---- shared TX machinery ------------------------------------------------- */
/*
 * One send path for both interfaces. The static buffers outlive the async USB
 * write (nrfx doesn't copy them), and every buffer mutation and write happens
 * on the system work queue only, so a buffer is never touched from two
 * threads. The int_in_ready completion returns the per-interface semaphore.
 *
 * If a write races a USB disconnect, the completion never fires and the
 * semaphore is stranded (a documented Zephyr legacy-stack issue). A single
 * timeout, however, may just mean the host is slow to poll the endpoint — in
 * that case the in-flight transfer still owns the buffer and we must not touch
 * it. So a timed-out send is skipped, and only after several consecutive
 * timeouts is the completion declared lost and the semaphore recovered.
 */
#define SEM_STRANDED_THRESHOLD 3

struct hid_tx {
    const struct device *dev; /* set at init */
    struct k_sem sem;
    uint8_t *buf;             /* [report id, payload...] — static storage */
    size_t len;
    uint8_t timeouts;         /* consecutive semaphore timeouts */
    const char *tag;          /* for logs */
};

static uint8_t vendor_buf[3] = { REPORT_ID_BATTERY, BATTERY_UNKNOWN, BATTERY_UNKNOWN };
static uint8_t battery_buf[4] = { REPORT_ID_STDBATT, 0, 0, 0 };

static struct hid_tx vendor_tx = {
    .buf = vendor_buf, .len = sizeof(vendor_buf), .tag = "vendor",
};
static struct hid_tx battery_tx = {
    .buf = battery_buf, .len = sizeof(battery_buf), .tag = "std battery",
};

static void int_in_ready_cb(const struct device *dev)
{
    struct hid_tx *tx = (dev == vendor_tx.dev)  ? &vendor_tx
                      : (dev == battery_tx.dev) ? &battery_tx
                                                : NULL;
    if (tx) {
        k_sem_give(&tx->sem);
    }
}

/* Runs on the system work queue only. Copies payload into the report buffer
 * (after byte 0, the report ID) once this send owns it, then writes. */
static int hid_tx_send(struct hid_tx *tx, const uint8_t *payload, size_t payload_len)
{
    if (!tx->dev) {
        return -ENODEV;
    }
    if (k_sem_take(&tx->sem, K_MSEC(100)) != 0) {
        if (++tx->timeouts < SEM_STRANDED_THRESHOLD) {
            /* Completion may be merely late; the in-flight transfer still
             * owns the buffer. Skip this send — the next tick retries. */
            return -EBUSY;
        }
        LOG_WRN("%s completion stranded; recovering semaphore", tx->tag);
        k_sem_reset(&tx->sem); /* leaves it taken — this send owns the buffer */
    }
    tx->timeouts = 0;
    memcpy(tx->buf + 1, payload, payload_len);
    int err = hid_int_ep_write(tx->dev, tx->buf, tx->len, NULL);
    if (err) {
        k_sem_give(&tx->sem);
        LOG_ERR("%s hid_int_ep_write failed: %d", tx->tag, err);
    }
    return err;
}

static int send_vendor_report(void)
{
    /* Emit in true [left, right] order using the auto-detected slot mapping. */
    uint8_t payload[2] = { levels[left_slot], levels[left_slot ^ 1] };

    return hid_tx_send(&vendor_tx, payload, sizeof(payload));
}

static int send_battery_report(void)
{
    uint8_t level = combined_level();

    if (level == BATTERY_UNKNOWN) {
        /* Nothing from the halves yet — stay silent rather than report 0%,
         * which the host would treat as a critically low battery. */
        return 0;
    }
    uint8_t payload[3] = { 0, 0, level }; /* modifiers, reserved, battery% */

    return hid_tx_send(&battery_tx, payload, sizeof(payload));
}

/* ---- HID_2 GET_REPORT(feature) ------------------------------------------ */

/* Feature report is just [id, battery%] — no keyboard fields. */
static uint8_t battery_feature[2] = { REPORT_ID_STDBATT, 0 };

static int battery_get_report_cb(const struct device *dev, struct usb_setup_packet *setup,
                                 int32_t *len, uint8_t **data)
{
    ARG_UNUSED(dev);
    if ((setup->wValue >> 8) != HID_GET_REPORT_TYPE_FEATURE) {
        return -ENOTSUP;
    }
    uint8_t level = combined_level();
    if (level == BATTERY_UNKNOWN) {
        /* No data yet; stall rather than claim 0%. The kernel retries on the
         * next enumeration and gets pushed INPUT updates once values arrive. */
        return -ENOTSUP;
    }
    battery_feature[1] = level;
    *data = battery_feature;
    *len = sizeof(battery_feature);
    return 0;
}

static const struct hid_ops vendor_ops = {
    .int_in_ready = int_in_ready_cb,
};

static const struct hid_ops battery_ops = {
    .int_in_ready = int_in_ready_cb,
    .get_report = battery_get_report_cb,
};

/* ---- init: register both extra interfaces ------------------------------- */

static int register_hid(const char *name, const uint8_t *desc, size_t desc_len,
                        const struct hid_ops *ops, struct hid_tx *tx)
{
    const struct device *dev = device_get_binding(name);
    if (!dev) {
        LOG_ERR("device_get_binding(%s) NULL — is CONFIG_USB_HID_DEVICE_COUNT high enough?", name);
        return -ENODEV;
    }
    usb_hid_register_device(dev, desc, desc_len, ops);
    int err = usb_hid_init(dev);
    if (err) {
        LOG_ERR("usb_hid_init(%s) failed: %d", name, err);
        return err;
    }
    k_sem_init(&tx->sem, 1, 1);
    tx->dev = dev;
    return 0;
}

static int zmk_split_battery_hid_init(void)
{
    int err = register_hid("HID_1", vendor_report_desc, sizeof(vendor_report_desc),
                           &vendor_ops, &vendor_tx);
    if (err) {
        return err;
    }
    err = register_hid("HID_2", battery_report_desc, sizeof(battery_report_desc),
                       &battery_ops, &battery_tx);
    if (err) {
        return err;
    }
    LOG_INF("split battery HID interfaces initialized (HID_1 vendor, HID_2 standard)");
    return 0;
}

SYS_INIT(zmk_split_battery_hid_init, APPLICATION, 91);

/* ---- sending: event-driven + periodic vendor re-send --------------------- */
/*
 * All sends run on the system work queue (never in the BT RX thread that
 * delivers battery events, and never in the timer ISR):
 *
 *   report_work — sends both interfaces; submitted by the battery listener.
 *   resend_work — sends the vendor report only; submitted by a periodic
 *                 timer so a host reader that (re)connects sees values within
 *                 one interval. The standard interface needs no periodic
 *                 resend: the kernel polls its feature report at enumeration
 *                 and receives pushed updates on change — resending it every
 *                 few seconds would just churn the host's power_supply and
 *                 defeat USB autosuspend.
 */
#define RESEND_INTERVAL_S 3

static void report_work_cb(struct k_work *work)
{
    ARG_UNUSED(work);
    send_vendor_report();
    send_battery_report();
}
K_WORK_DEFINE(report_work, report_work_cb);

static void resend_work_cb(struct k_work *work)
{
    ARG_UNUSED(work);
    refresh_connected_slots();
    send_vendor_report();
}
K_WORK_DEFINE(resend_work, resend_work_cb);

static void resend_timer_cb(struct k_timer *timer)
{
    ARG_UNUSED(timer);
    k_work_submit(&resend_work);
}
K_TIMER_DEFINE(resend_timer, resend_timer_cb, NULL);

static int start_resend_timer(void)
{
    k_timer_start(&resend_timer, K_SECONDS(RESEND_INTERVAL_S), K_SECONDS(RESEND_INTERVAL_S));
    return 0;
}

SYS_INIT(start_resend_timer, APPLICATION, 92);

static int battery_listener(const zmk_event_t *eh)
{
    const struct zmk_peripheral_battery_state_changed *bat_ev =
        as_zmk_peripheral_battery_state_changed(eh);
    if (bat_ev) {
        if (bat_ev->source < ARRAY_SIZE(levels)) {
            /* bat_ev->source is the peripheral slot (bonding order — see file
             * header). Defer the USB writes to the work queue: this listener
             * runs in the BT RX thread and must not block on USB. */
            levels[bat_ev->source] = bat_ev->state_of_charge;
            k_work_submit(&report_work);
            LOG_DBG("battery update src=%u soc=%u", bat_ev->source, bat_ev->state_of_charge);
        } else {
            LOG_WRN("battery event from unexpected source=%u", bat_ev->source);
        }
        return ZMK_EV_EVENT_BUBBLE;
    }

    /* Keystrokes reveal which slot is which side: the position tells the
     * physical half, the source tells the slot. Only act when the learned
     * mapping actually changes (i.e. once after a reversed re-pair). */
    const struct zmk_position_state_changed *pos_ev = as_zmk_position_state_changed(eh);
    if (pos_ev && pos_ev->source < ARRAY_SIZE(levels)) {
        uint8_t detected = position_is_left(pos_ev->position)
                               ? pos_ev->source
                               : (pos_ev->source ^ 1);
        if (detected != left_slot) {
            left_slot = detected;
            LOG_INF("left half detected in slot %u", left_slot);
            k_work_submit(&report_work);
        }
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(zmk_split_battery_listener, battery_listener);
ZMK_SUBSCRIPTION(zmk_split_battery_listener, zmk_peripheral_battery_state_changed);
ZMK_SUBSCRIPTION(zmk_split_battery_listener, zmk_position_state_changed);
