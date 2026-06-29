/*
 * machine.c — facade that auto-detects FTMS vs iFit treadmills.
 *
 * Owns a single unified BLE scan (one ble_gap_disc): each advert is classified
 * as FTMS (0x1826) or iFit (0x1533) and collected into one device list tagged
 * with its protocol. machine_connect() dispatches to the matching adapter; the
 * adapters report connect/disconnect back via an event hook so the facade owns
 * reconnect + NVS persistence (so the same flow works for either protocol).
 */
#include "machine.h"
#include "machine_ftms.h"
#include "machine_ifit.h"

#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

#define TAG     "machine"
#define NVS_NS  "ftms"
#define NVS_KEY "last"

static uint8_t       s_own_addr_type = BLE_OWN_ADDR_RANDOM;
static ftms_device_t s_devs[FTMS_MAX_DEVICES];
static int           s_ndev;
static ftms_device_t s_pending;   /* device currently being connected */
static bool          s_scanning;  /* suppress reconnect while scan is pending */

/* ---- NVS persistence of the last device (incl. protocol) ---------------- */

static void save_last(const ftms_device_t *d) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, NVS_KEY, d, sizeof *d);
    nvs_commit(h);
    nvs_close(h);
}

static bool load_last(ftms_device_t *d) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t sz = sizeof *d;
    bool ok = (nvs_get_blob(h, NVS_KEY, d, &sz) == ESP_OK && sz == sizeof *d);
    nvs_close(h);
    return ok;
}

/* ---- adapter connect/disconnect events ---------------------------------- */

static void on_evt(int connected) {
    if (connected) {
        save_last(&s_pending);   /* remember what we connected to */
        ESP_LOGI(TAG, "connected (%s)",
                 s_pending.proto == MACHINE_PROTO_IFIT ? "iFit" : "FTMS");
    } else {
        if (s_scanning) return;  /* cancelled intentionally; scan will start */
        ESP_LOGI(TAG, "disconnected — reconnecting to last");
        machine_try_last();      /* one re-attempt; its failure falls to scan */
    }
}

/* ---- unified scan ------------------------------------------------------- */

static void adv_name(const uint8_t *data, uint8_t len, char *out, int outlen) {
    out[0] = '\0';
    uint8_t i = 0;
    while (i < len) {
        uint8_t l = data[i];
        if (l < 1 || (uint16_t)i + 1u + l > len) break;
        uint8_t t = data[i + 1];
        if (t == 0x08 || t == 0x09) {
            int nl = l - 1; if (nl > outlen - 1) nl = outlen - 1;
            memcpy(out, &data[i + 2], nl); out[nl] = '\0'; return;
        }
        i += (uint8_t)(l + 1);
    }
}

static int gap_scan_cb(struct ble_gap_event *e, void *arg) {
    (void)arg;
    if (e->type != BLE_GAP_EVENT_DISC) return 0;
    struct ble_gap_disc_desc *d = &e->disc;

    int proto;
    if (machine_ifit_is_ifit_adv(d->data, d->length_data))      /* prefer iFit */
        proto = MACHINE_PROTO_IFIT;
    else if (machine_ftms_is_ftms_adv(d->data, d->length_data))
        proto = MACHINE_PROTO_FTMS;
    else
        return 0;

    ftms_device_t dev;
    dev.addr_type = d->addr.type;
    memcpy(dev.addr, d->addr.val, 6);
    dev.rssi = d->rssi;
    dev.proto = (uint8_t)proto;
    adv_name(d->data, d->length_data, dev.name, FTMS_NAME_LEN);
    s_ndev = ftms_devlist_upsert(s_devs, s_ndev, &dev);
    return 0;
}

/* ---- public API --------------------------------------------------------- */

void machine_set_addr_type(uint8_t t) {
    s_own_addr_type = t;
    machine_ftms_set_addr_type(t);
    machine_ifit_set_addr_type(t);
}

void machine_set_data_cb(machine_state_cb cb) {
    machine_ftms_set_data_cb(cb);
    machine_ifit_set_data_cb(cb);
    machine_ftms_set_event_cb(on_evt);
    machine_ifit_set_event_cb(on_evt);
}

void machine_start_scan(void) {
    s_ndev = 0;
    s_scanning = true;
    ble_gap_conn_cancel();   /* cancel any in-progress connection attempt */
    ble_gap_disc_cancel();   /* cancel any in-progress scan */
    s_scanning = false;
    struct ble_gap_disc_params p = { .filter_duplicates = 0, .passive = 0,
        .filter_policy = BLE_HCI_SCAN_FILT_NO_WL };
    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &p, gap_scan_cb, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY)
        ESP_LOGE(TAG, "ble_gap_disc failed: %d", rc);
    else
        ESP_LOGI(TAG, "scanning (FTMS + iFit)…");
}

int machine_get_devices(ftms_device_t *out, int max) {
    int n = s_ndev < max ? s_ndev : max;
    memcpy(out, s_devs, (size_t)n * sizeof(ftms_device_t));
    ftms_devlist_sort(out, n);
    return n;
}

void machine_connect(const ftms_device_t *dev) {
    s_pending = *dev;
    if (dev->proto == MACHINE_PROTO_IFIT) machine_ifit_connect(dev);
    else                                  machine_ftms_connect(dev);
}

void machine_try_last(void) {
    ftms_device_t d;
    if (load_last(&d)) {
        ESP_LOGI(TAG, "reconnecting to last (%s)",
                 d.proto == MACHINE_PROTO_IFIT ? "iFit" : "FTMS");
        machine_connect(&d);
    } else {
        machine_start_scan();
    }
}

bool machine_connected(void) {
    return machine_ftms_connected() || machine_ifit_connected();
}

const ftms_device_t *machine_connected_device(void) {
    if (machine_connected()) return &s_pending;
    return NULL;
}

bool machine_connecting(void) {
    return machine_ftms_connecting() || machine_ifit_connecting();
}

int8_t machine_conn_rssi(void) {
    if (machine_ifit_connected()) return machine_ifit_conn_rssi();
    if (machine_ftms_connected()) return machine_ftms_conn_rssi();
    return 0;
}

bool machine_set_speed(float kmh) {
    return machine_ftms_set_speed(kmh);
}

bool machine_set_incline(float pct) {
    return machine_ftms_set_incline(pct);
}

bool machine_stop(void) {
    return machine_ftms_stop();
}
