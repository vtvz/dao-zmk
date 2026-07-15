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
 *   HID_2 — standard battery interface (Usage Page 0x06 Generic Device
 *           Controls, Usage 0x20 Battery Strength) exposed as a FEATURE
 *           report. The Linux kernel's hid-input driver auto-maps this to a
 *           power_supply entry (visible in upower / the system tray) exactly
 *           like a wireless mouse. HID has no notion of two batteries, so this
 *           single value is the WORSE of the two halves (min), i.e. "charge
 *           when this hits low". Served on demand via the get_report callback.
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

/* ---- HID_2: standard battery, carried on a dummy mouse interface --------- */
/*
 * The kernel maps the Battery Strength usage to a power_supply only when the
 * interface ALSO registers a real input device — hidinput_has_been_populated()
 * ignores EV_PWR, so a battery-only interface is torn down and the battery
 * orphaned. A bare Button page inside a GenDevCtrls collection wasn't enough
 * (hid-generic didn't populate an input device from it).
 *
 * So we present a minimal *mouse* (Generic Desktop / Mouse / Pointer — the
 * exact shape wireless mice use, which hid-generic always turns into an input
 * device) and append the Battery Strength usage in the same input report. The
 * mouse never moves (we always send zeros); it exists only so the input device
 * registers and the battery survives.
 *
 * INPUT report layout (Report ID 2): [buttons(1B, always 0), x(1B,0), y(1B,0),
 * battery%(1B)]. A Feature copy of just the battery byte is also offered.
 */
#define REPORT_ID_STDBATT 0x02

static const uint8_t battery_report_desc[] = {
    0x05, 0x01,       /* Usage Page (Generic Desktop)                   */
    0x09, 0x02,       /* Usage (Mouse)                                  */
    0xA1, 0x01,       /* Collection (Application)                       */
    0x85, REPORT_ID_STDBATT, /*   Report ID (2)                         */
    0x09, 0x01,       /*   Usage (Pointer)                              */
    0xA1, 0x00,       /*   Collection (Physical)                        */
    /* 3 buttons (always 0) */
    0x05, 0x09,       /*     Usage Page (Button)                        */
    0x19, 0x01,       /*     Usage Minimum (1)                          */
    0x29, 0x03,       /*     Usage Maximum (3)                          */
    0x15, 0x00,       /*     Logical Minimum (0)                        */
    0x25, 0x01,       /*     Logical Maximum (1)                        */
    0x75, 0x01,       /*     Report Size (1)                            */
    0x95, 0x03,       /*     Report Count (3)                           */
    0x81, 0x02,       /*     Input (Data,Var,Abs)                       */
    0x75, 0x05,       /*     Report Size (5) padding                    */
    0x95, 0x01,       /*     Report Count (1)                           */
    0x81, 0x03,       /*     Input (Const)                              */
    /* X / Y (always 0) */
    0x05, 0x01,       /*     Usage Page (Generic Desktop)               */
    0x09, 0x30,       /*     Usage (X)                                  */
    0x09, 0x31,       /*     Usage (Y)                                  */
    0x15, 0x81,       /*     Logical Minimum (-127)                     */
    0x25, 0x7F,       /*     Logical Maximum (127)                      */
    0x75, 0x08,       /*     Report Size (8)                            */
    0x95, 0x02,       /*     Report Count (2)                           */
    0x81, 0x06,       /*     Input (Data,Var,Rel)                       */
    0xC0,             /*   End Collection (Physical)                    */
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

static int send_vendor_report(void)
{
    if (!vendor_dev) {
        return -ENODEV;
    }
    uint8_t buf[3] = { REPORT_ID_BATTERY, levels[0], levels[1] };

    if (k_sem_take(&report_sem, K_MSEC(100)) != 0) {
        LOG_WRN("battery report semaphore busy");
        return -EBUSY;
    }
    int err = hid_int_ep_write(vendor_dev, buf, sizeof(buf), NULL);
    if (err) {
        k_sem_give(&report_sem);
        LOG_ERR("hid_int_ep_write failed: %d", err);
    }
    return err;
}

/* ---- HID_2 ops: push INPUT on change + answer GET_REPORT(feature) -------- */

static K_SEM_DEFINE(battery_sem, 1, 1);

/* Pushed INPUT report: [id, buttons=0, x=0, y=0, battery%]. The mouse fields
 * stay zero; only the last byte (battery) ever changes. */
#define BATT_BYTE_INDEX 4
static uint8_t battery_report[5] = { REPORT_ID_STDBATT, 0, 0, 0, 0 };

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
        LOG_WRN("std battery report semaphore busy");
        return -EBUSY;
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
