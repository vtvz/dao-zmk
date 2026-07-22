/*
 * Exposes the left and right split-half battery levels of a ZMK split
 * keyboard to the host over USB, since ZMK only reports battery over BLE.
 * The dongle (central) presents two extra USB HID interfaces:
 *
 *   HID_1 — vendor interface (Usage Page 0xFF00). Pushes a 3-byte INPUT
 *           report [0x01, left%, right%] (0xFF = unknown) on every peripheral
 *           battery event. Read by our host reader/widget, which finds it by
 *           the report-descriptor prefix 06 00 ff.
 *
 *   HID_2 — standard battery: the Generic Device Controls "Battery Strength"
 *           usage (0x06/0x20) in an INPUT report, wrapped in a minimal dummy
 *           keyboard (see below). The Linux kernel's hid-input driver auto-maps
 *           it to a power_supply entry (visible in upower / the system tray)
 *           like a wireless mouse/keyboard. HID has no notion of two batteries,
 *           so this single value is the WORSE of the two halves (min), i.e.
 *           "charge when this hits low". Pushed on change; also readable via
 *           GET_REPORT (feature).
 *
 * Dongle (central) build only. Requires CONFIG_USB_HID_DEVICE_COUNT=3.
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/device.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/usb/class/usb_hid.h>
#include <zephyr/logging/log.h>

#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>

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

static const struct device *vendor_dev;
static const struct device *battery_dev;
static K_SEM_DEFINE(report_sem, 1, 1);
static uint8_t levels[2] = { BATTERY_UNKNOWN, BATTERY_UNKNOWN };

/* Combined level for the standard interface: the worse of the two known
 * halves, so the host shows "time to charge" for whichever needs it first. */
static uint8_t combined_level(void)
{
    uint8_t l = levels[0], r = levels[1];
    if (l == BATTERY_UNKNOWN) return (r == BATTERY_UNKNOWN) ? 0 : r;
    if (r == BATTERY_UNKNOWN) return l;
    return l < r ? l : r;
}

/* ---- HID_1 ops: push report on change ----------------------------------- */

static void vendor_int_in_ready_cb(const struct device *dev)
{
    ARG_UNUSED(dev);
    k_sem_give(&report_sem);
}

static const struct hid_ops vendor_ops = {
    .int_in_ready = vendor_int_in_ready_cb,
};

/* buf is static so it outlives the async USB write (nrfx doesn't copy it —
 * a stack buffer would be a data race). Only touched from the work queue. */
static uint8_t vendor_buf[3] = { REPORT_ID_BATTERY, 0, 0 };

static int send_vendor_report(void)
{
    if (!vendor_dev) {
        return -ENODEV;
    }
    /* The int_in_ready completion returns this semaphore. If a previous write
     * raced a USB disconnect, that completion never fires and the semaphore is
     * stranded (a documented Zephyr legacy-stack issue). So on timeout we
     * reset it rather than blocking forever — the next send then proceeds. */
    if (k_sem_take(&report_sem, K_MSEC(100)) != 0) {
        /* Reset leaves the count at 0 (i.e. taken), which is the state we want
         * to proceed with the write below. */
        LOG_WRN("vendor report sem stranded; resetting");
        k_sem_reset(&report_sem);
    }
    vendor_buf[1] = levels[0];
    vendor_buf[2] = levels[1];
    int err = hid_int_ep_write(vendor_dev, vendor_buf, sizeof(vendor_buf), NULL);
    if (err) {
        k_sem_give(&report_sem);
        LOG_ERR("hid_int_ep_write failed: %d", err);
    }
    return err;
}

/* ---- HID_2 ops: push INPUT on change + answer GET_REPORT(feature) -------- */

static K_SEM_DEFINE(battery_sem, 1, 1);

/* Pushed INPUT report: [id, modifiers=0, reserved=0, battery%]. The keyboard
 * fields stay zero; only the last byte (battery) ever changes. */
#define BATT_BYTE_INDEX 3
static uint8_t battery_report[4] = { REPORT_ID_STDBATT, 0, 0, 0 };

static void battery_int_in_ready_cb(const struct device *dev)
{
    ARG_UNUSED(dev);
    k_sem_give(&battery_sem);
}

/* Feature report is just [id, battery%] — no button field. */
static uint8_t battery_feature[2] = { REPORT_ID_STDBATT, 0 };

static int battery_get_report_cb(const struct device *dev, struct usb_setup_packet *setup,
                                 int32_t *len, uint8_t **data)
{
    ARG_UNUSED(dev);
    if ((setup->wValue >> 8) != HID_GET_REPORT_TYPE_FEATURE) {
        return -ENOTSUP;
    }
    battery_feature[1] = combined_level();
    *data = battery_feature;
    *len = sizeof(battery_feature);
    return 0;
}

static const struct hid_ops battery_ops = {
    .int_in_ready = battery_int_in_ready_cb,
    .get_report = battery_get_report_cb,
};

static int send_battery_report(void)
{
    if (!battery_dev) {
        return -ENODEV;
    }
    if (k_sem_take(&battery_sem, K_MSEC(100)) != 0) {
        /* Recover a semaphore stranded by a write racing a USB disconnect
         * (see send_vendor_report). Reset leaves it taken, ready for the write. */
        LOG_WRN("std battery report sem stranded; resetting");
        k_sem_reset(&battery_sem);
    }
    battery_report[BATT_BYTE_INDEX] = combined_level();
    int err = hid_int_ep_write(battery_dev, battery_report, sizeof(battery_report), NULL);
    if (err) {
        k_sem_give(&battery_sem);
        LOG_ERR("std battery hid_int_ep_write failed: %d", err);
    }
    return err;
}

/* ---- init: register both extra interfaces ------------------------------- */

static int register_hid(const char *name, const uint8_t *desc, size_t desc_len,
                        const struct hid_ops *ops, const struct device **out)
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
    *out = dev;
    return 0;
}

static int zmk_split_battery_hid_init(void)
{
    int err = register_hid("HID_1", vendor_report_desc, sizeof(vendor_report_desc),
                           &vendor_ops, &vendor_dev);
    if (err) {
        return err;
    }
    err = register_hid("HID_2", battery_report_desc, sizeof(battery_report_desc),
                       &battery_ops, &battery_dev);
    if (err) {
        return err;
    }
    LOG_INF("split battery HID interfaces initialized (HID_1 vendor, HID_2 standard)");
    return 0;
}

SYS_INIT(zmk_split_battery_hid_init, APPLICATION, 91);

/* ---- periodic re-send -------------------------------------------------- */
/*
 * ZMK only raises battery events on change, so a host reader that connects
 * between changes would see nothing for a long time. Re-send the current
 * values on a timer so a freshly-connected reader/widget shows status within
 * one interval instead of waiting for the next battery change. The USB writes
 * must run in a work item, not the timer ISR.
 */
#define RESEND_INTERVAL_S 3

static void resend_work_cb(struct k_work *work)
{
    ARG_UNUSED(work);
    send_vendor_report();
    send_battery_report();
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
    const struct zmk_peripheral_battery_state_changed *ev =
        as_zmk_peripheral_battery_state_changed(eh);
    if (!ev) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    if (ev->source < ARRAY_SIZE(levels)) {
        levels[ev->source] = ev->state_of_charge;
        send_vendor_report();
        send_battery_report();
        LOG_DBG("battery update src=%u soc=%u", ev->source, ev->state_of_charge);
    } else {
        LOG_WRN("battery event from unexpected source=%u", ev->source);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(zmk_split_battery_listener, battery_listener);
ZMK_SUBSCRIPTION(zmk_split_battery_listener, zmk_peripheral_battery_state_changed);
