/*
 * machine_ifit.c — BLE central for iFit/NordicTrack/ProForm treadmills.
 *
 * Proprietary "Nordic-base" service (from qdomyos-zwift proformtreadmill.cpp):
 *   service 00001533-1412-efde-1523-785feabcd123
 *   notify  00001535-1412-efde-1523-785feabcd123  (Treadmill Data frames)
 *   write   00001534-1412-efde-1523-785feabcd123  (poll/keepalive + control)
 *
 * Flow: scan → match the 1533 service in the advert → connect → discover the
 * service + notify/write chars → enable notifications → periodically write the
 * keepalive poll so the treadmill streams data → parse 20-byte data frames
 * (ifit_parse) → integrate distance from speed → invoke the state callback.
 *
 * STATUS: hardware-verified on a live iFit treadmill (decode + T-series poll +
 * full chain to RSC). Not yet wired into ui.c — needs a FTMS-vs-iFit selector.
 */
#include "machine_ifit.h"
#include "ifit_parse.h"

#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/util/util.h"
#include "nimble/ble.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>

#define TAG "machine_ifit"
#define NOTIF_BUF_SIZE 64

/* 128-bit UUIDs, little-endian byte order for BLE_UUID128_INIT. */
static const ble_uuid128_t IFIT_SVC = BLE_UUID128_INIT(
    0x23,0xd1,0xbc,0xea,0x5f,0x78,0x23,0x15,0xde,0xef,0x12,0x14,0x33,0x15,0x00,0x00);
static const ble_uuid128_t IFIT_NOTIFY = BLE_UUID128_INIT(
    0x23,0xd1,0xbc,0xea,0x5f,0x78,0x23,0x15,0xde,0xef,0x12,0x14,0x35,0x15,0x00,0x00);
static const ble_uuid128_t IFIT_WRITE = BLE_UUID128_INIT(
    0x23,0xd1,0xbc,0xea,0x5f,0x78,0x23,0x15,0xde,0xef,0x12,0x14,0x34,0x15,0x00,0x00);

/* The raw 16-byte service UUID (LE) for matching it in advertising data. */
static const uint8_t IFIT_SVC_RAW[16] = {
    0x23,0xd1,0xbc,0xea,0x5f,0x78,0x23,0x15,0xde,0xef,0x12,0x14,0x33,0x15,0x00,0x00};

/* NordicTrack 6.5S (T6.5S v81) init sequence — must be sent before streaming
 * starts, one command per timer tick. (qdomyos-zwift proformtreadmill.cpp) */
static const uint8_t INIT_00[] = {0xfe,0x02,0x08,0x02};
static const uint8_t INIT_01[] = {0xff,0x08,0x02,0x04,0x02,0x04,0x02,0x04,0x81,0x87,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
static const uint8_t INIT_02[] = {0xff,0x08,0x02,0x04,0x02,0x04,0x04,0x04,0x80,0x88,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
static const uint8_t INIT_03[] = {0xff,0x08,0x02,0x04,0x02,0x04,0x04,0x04,0x88,0x90,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
static const uint8_t INIT_04[] = {0xfe,0x02,0x0a,0x02};
static const uint8_t INIT_05[] = {0xff,0x0a,0x02,0x04,0x02,0x06,0x02,0x06,0x82,0x00,0x00,0x8a,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
static const uint8_t INIT_06[] = {0xff,0x0a,0x02,0x04,0x02,0x06,0x02,0x06,0x84,0x00,0x00,0x8c,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
static const uint8_t INIT_07[] = {0xff,0x08,0x02,0x04,0x02,0x04,0x02,0x04,0x95,0x9b,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
static const uint8_t INIT_08[] = {0xfe,0x02,0x2c,0x04};
static const uint8_t INIT_09[] = {0x00,0x12,0x02,0x04,0x02,0x28,0x04,0x28,0x90,0x07,0x01,0xce,0xc4,0xb0,0xaa,0xa2,0xa8,0x94,0x96,0x96};
static const uint8_t INIT_10[] = {0x01,0x12,0xac,0xa8,0xa2,0xba,0xd0,0xdc,0xce,0xfe,0x14,0x00,0x3a,0x52,0x78,0x64,0x86,0xa6,0xfc,0x18};
static const uint8_t INIT_11[] = {0xff,0x08,0x32,0x4a,0xa0,0x88,0x02,0x00,0x00,0x44,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
static const uint8_t INIT_12[] = {0xfe,0x02,0x19,0x03};
static const uint8_t INIT_13[] = {0x00,0x12,0x02,0x04,0x02,0x15,0x04,0x15,0x02,0x00,0x0f,0x00,0x10,0x00,0xd8,0x1c,0x48,0x00,0x00,0xe0};
static const uint8_t INIT_14[] = {0xff,0x07,0x00,0x00,0x00,0x10,0x00,0x08,0x6e,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
static const uint8_t INIT_15[] = {0xfe,0x02,0x17,0x03};
static const uint8_t INIT_16[] = {0x00,0x12,0x02,0x04,0x02,0x13,0x04,0x13,0x02,0x0c,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
static const uint8_t INIT_17[] = {0xff,0x05,0x00,0x80,0x00,0x00,0xa5,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};

typedef struct { const uint8_t *d; uint16_t n; } ifit_cmd_t;
static const ifit_cmd_t INIT_SEQ[] = {
    {INIT_00,sizeof INIT_00},{INIT_01,sizeof INIT_01},{INIT_02,sizeof INIT_02},
    {INIT_03,sizeof INIT_03},{INIT_04,sizeof INIT_04},{INIT_05,sizeof INIT_05},
    {INIT_06,sizeof INIT_06},{INIT_07,sizeof INIT_07},{INIT_08,sizeof INIT_08},
    {INIT_09,sizeof INIT_09},{INIT_10,sizeof INIT_10},{INIT_11,sizeof INIT_11},
    {INIT_12,sizeof INIT_12},{INIT_13,sizeof INIT_13},{INIT_14,sizeof INIT_14},
    {INIT_15,sizeof INIT_15},{INIT_16,sizeof INIT_16},{INIT_17,sizeof INIT_17},
};
#define N_INIT ((int)(sizeof INIT_SEQ / sizeof INIT_SEQ[0]))

/* NordicTrack 6.5S keepalive poll — 6 phases, cycled after init completes.
 * (qdomyos-zwift proformtreadmill.cpp, nordictrack_t65s_treadmill variant) */
static const uint8_t POLL0[] = {0xfe,0x02,0x19,0x03};
static const uint8_t POLL1[] = {0x00,0x12,0x02,0x04,0x02,0x15,0x04,0x15,0x02,0x00,
                                0x0f,0x80,0x0a,0x41,0x00,0x00,0x00,0x00,0x00,0x00};
static const uint8_t POLL2[] = {0xff,0x07,0x00,0x00,0x00,0x81,0x00,0x10,0x86,0x00,
                                0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
static const uint8_t POLL3[] = {0xfe,0x02,0x14,0x03};
static const uint8_t POLL4[] = {0x00,0x12,0x02,0x04,0x02,0x10,0x04,0x10,0x02,0x00,
                                0x0a,0x1b,0x94,0x30,0x00,0x00,0x40,0x50,0x00,0x80};
static const uint8_t POLL5[] = {0xff,0x02,0x18,0x27,0x00,0x00,0x00,0x00,0x00,0x00,
                                0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
static const ifit_cmd_t POLL_SEQ[] = {
    {POLL0,sizeof POLL0},{POLL1,sizeof POLL1},{POLL2,sizeof POLL2},
    {POLL3,sizeof POLL3},{POLL4,sizeof POLL4},{POLL5,sizeof POLL5},
};
#define N_POLL ((int)(sizeof POLL_SEQ / sizeof POLL_SEQ[0]))

/* Belt-start sequence (nordictrack_t65s_treadmill_81_miles variant) — required to
 * get the belt moving from a dead stop; forceSpeed() alone only adjusts speed while
 * already moving. Sent right after POLL5/noOpData6, same as the vendor's case 5.
 * (qdomyos-zwift proformtreadmill.cpp, requestStart handling) */
static const uint8_t START_01[] = {0xfe,0x02,0x20,0x03};
static const uint8_t START_02[] = {0x00,0x12,0x02,0x04,0x02,0x1c,0x04,0x1c,0x02,0x09,0x00,0x00,0x40,0x02,0x18,0x40,0x00,0x00,0x80,0x30};
static const uint8_t START_03[] = {0xff,0x0e,0x2a,0x00,0x00,0xc7,0x20,0x58,0x02,0x00,0xb4,0x00,0x58,0x02,0x00,0xee,0x00,0x00,0x00,0x00};
static const uint8_t START_04[] = {0xfe,0x02,0x11,0x02};
static const uint8_t START_05[] = {0xff,0x11,0x02,0x04,0x02,0x0d,0x04,0x0d,0x02,0x02,0x03,0x10,0xa0,0x00,0x00,0x00,0x0a,0x00,0xd2,0x00};
static const ifit_cmd_t START_SEQ[] = {
    {START_01,sizeof START_01},{START_02,sizeof START_02},{START_03,sizeof START_03},
    {START_04,sizeof START_04},{START_05,sizeof START_05},
};
#define N_START ((int)(sizeof START_SEQ / sizeof START_SEQ[0]))

/* ---- state -------------------------------------------------------------- */

static machine_state_cb s_cb;
static machine_event_cb s_evt_cb;
void machine_ifit_set_event_cb(machine_event_cb cb) { s_evt_cb = cb; }
bool machine_ifit_is_ifit_adv(const uint8_t *data, uint8_t len);
static uint8_t          s_own_addr_type = BLE_OWN_ADDR_RANDOM;
static uint16_t         s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t         s_notify_handle, s_write_handle, s_svc_end;
static bool             s_connecting;
static esp_timer_handle_t s_poll_timer;
static int              s_poll_step;   /* 0..N_INIT-1 = init cmds, N_INIT+ = poll cycle */

static ftms_device_t    s_devs[FTMS_MAX_DEVICES];
static int              s_ndev;

static float            s_distance_m;     /* integrated */
static int64_t          s_last_rx_us;

static void start_scan(void);
static int  gap_event_cb(struct ble_gap_event *event, void *arg);

/* ---- advert helpers ----------------------------------------------------- */

/* True if the advert lists the iFit 128-bit service UUID (AD type 0x06/0x07). */
static bool adv_has_ifit(const uint8_t *data, uint8_t len) {
    uint8_t i = 0;
    while (i < len) {
        uint8_t l = data[i];
        if (l < 1 || (uint16_t)i + 1u + l > len) break;
        uint8_t t = data[i + 1];
        if (t == 0x06 || t == 0x07) {
            for (uint8_t j = 2; j + 16 <= (uint16_t)l + 1; j += 16)
                if (memcmp(&data[i + j], IFIT_SVC_RAW, 16) == 0) return true;
        }
        i += (uint8_t)(l + 1);
    }
    return false;
}

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

/* ---- keepalive poll ----------------------------------------------------- */

static void poll_write(const uint8_t *d, uint16_t n) {
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || !s_write_handle) return;
    ble_gattc_write_no_rsp_flat(s_conn_handle, s_write_handle, d, n);
}

/* NordicTrack 6.5S speed/incline control write (qdomyos-zwift proformtreadmill.cpp,
 * forceSpeed/forceIncline, nordictrack_t65s_treadmill branch: checksum = low byte + 0x12).
 * Preceded by a no-op frame, same as the vendor driver. Value is *100, little-endian,
 * already in the km/h / % units ifit_parse.c decodes notifications into.
 *
 * IMPORTANT: the vendor driver only ever calls this from inside its poll state
 * machine's "case 2" step (right after writing the POLL2/noOpData3 frame) — control
 * writes fired outside that slot are ignored by the treadmill firmware. See
 * s_req_speed/s_req_incline below; do not call ctrl_write() directly from the
 * public API. */
static void ctrl_write(uint8_t kind /* 0x01=speed, 0x02=incline */, float value) {
    static const uint8_t noop[] = {0xfe, 0x02, 0x0d, 0x02};
    uint8_t w[] = {0xff, 0x0d, 0x02, 0x04, 0x02, 0x09, 0x04, 0x09, 0x02, 0x01,
                   kind, 0xbc, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    uint16_t v = (uint16_t)(value * 100.0f);
    w[11] = v & 0xFF;
    w[12] = (v >> 8) & 0xFF;
    w[14] = w[11] + 0x12;
    poll_write(noop, sizeof noop);
    poll_write(w, sizeof w);
}

/* Pending control requests, applied only at poll phase 2 (see ctrl_write comment).
 * Sentinels match the vendor driver's requestSpeed/requestInclination conventions. */
#define REQ_SPEED_NONE   -1.0f
#define REQ_INCLINE_NONE -100.0f
static float s_req_speed    = REQ_SPEED_NONE;
static float s_req_incline  = REQ_INCLINE_NONE;
static float s_cur_speed_kmh;   /* last decoded speed, from handle_notify */
static bool  s_req_start;       /* belt is stopped and a nonzero speed was requested */

static void poll_cb(void *arg) {
    (void)arg;
    if (s_poll_step < N_INIT) {
        const ifit_cmd_t *c = &INIT_SEQ[s_poll_step];
        poll_write(c->d, c->n);
        ESP_LOGD(TAG, "init %d/%d", s_poll_step + 1, N_INIT);
        s_poll_step++;
        return;
    }
    int phase = (s_poll_step - N_INIT) % N_POLL;
    const ifit_cmd_t *c = &POLL_SEQ[phase];
    poll_write(c->d, c->n);
    if (phase == 2) {
        if (s_req_incline != REQ_INCLINE_NONE) {
            float i = s_req_incline < 0 ? 0 : s_req_incline;
            if (i <= 15) ctrl_write(0x02, i);
            s_req_incline = REQ_INCLINE_NONE;
        }
        if (s_req_speed != REQ_SPEED_NONE) {
            /* forceSpeed only takes effect while the belt is already moving; a
             * stopped belt needs the START_SEQ (case 5) first — see below. Keep
             * the request pending until the belt actually starts moving. */
            if (s_req_speed > 0 && s_cur_speed_kmh <= 0) {
                s_req_start = true;
            } else if (s_req_speed >= 0 && s_req_speed <= 22) {
                ctrl_write(0x01, s_req_speed);
                s_req_speed = REQ_SPEED_NONE;
            }
        }
    } else if (phase == 5 && s_req_start) {
        for (int i = 0; i < N_START; i++) poll_write(START_SEQ[i].d, START_SEQ[i].n);
        s_req_start = false;
    }
    s_poll_step++;
}

static void poll_start(void) {
    s_poll_step = 0;
    if (!s_poll_timer) {
        const esp_timer_create_args_t a = { .callback = poll_cb, .name = "ifit_poll" };
        esp_timer_create(&a, &s_poll_timer);
    }
    /* ponytail: 500ms per step — init takes ~9s, poll cycle ~3s; fast enough for
     * a treadmill display. Speed up if live data feels laggy. */
    esp_timer_start_periodic(s_poll_timer, 500000 /* 500 ms */);
}

static void poll_stop(void) {
    if (s_poll_timer) esp_timer_stop(s_poll_timer);
}

/* ---- notifications ------------------------------------------------------ */

static void handle_notify(struct os_mbuf *om) {
    uint8_t buf[NOTIF_BUF_SIZE];
    uint16_t len = OS_MBUF_PKTLEN(om);
    if (len > sizeof buf) len = sizeof buf;
    if (os_mbuf_copydata(om, 0, len, buf) != 0) return;

    float speed_mps, incline_pct;
    bool ok = ifit_parse_data(buf, len, &speed_mps, &incline_pct);
    /* Raw-frame sniff for protocol debugging; silent at default INFO level.
     * Bump this component to DEBUG (esp_log_level_set("machine_ifit", …)) to see
     * whether the treadmill streams at all and whether frames parse. */
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, buf, len, ESP_LOG_DEBUG);
    ESP_LOGD(TAG, "notify len=%u parse=%d", (unsigned)len, ok);
    if (!ok) return;

    int64_t now = esp_timer_get_time();
    if (s_last_rx_us) {
        int64_t gap_us = now - s_last_rx_us;
        if (gap_us > 30000000LL) {
            /* Gap > 30 s: treadmill restarted while BLE stayed up — reset odometer. */
            s_distance_m = 0;
            ESP_LOGI(TAG, "gap %.1f s — distance reset", gap_us / 1e6f);
        } else {
            s_distance_m += speed_mps * (gap_us / 1e6f);
        }
    }
    s_last_rx_us = now;
    s_cur_speed_kmh = speed_mps * 3.6f;

    treadmill_state_t st = {
        .speed_mps = speed_mps,
        .distance_m = s_distance_m,
        .incline_pct = incline_pct,
        .elapsed_s = 0,
    };
    if (s_cb) s_cb(&st);
}

/* ---- discovery callbacks ------------------------------------------------ */

static int dsc_disc_cb(uint16_t conn, const struct ble_gatt_error *err,
                       uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg) {
    (void)chr_val_handle; (void)arg;
    if (err->status != 0) return 0;
    if (ble_uuid_u16(&dsc->uuid.u) == BLE_GATT_DSC_CLT_CFG_UUID16) {
        uint8_t v[2] = {0x01, 0x00};
        int rc = ble_gattc_write_flat(conn, dsc->handle, v, sizeof v, NULL, NULL);
        ESP_LOGI(TAG, "iFit CCCD write @%u rc=%d; notify_handle=%u; starting poll",
                 dsc->handle, rc, s_notify_handle);
        poll_start();
    }
    return 0;
}

static int chr_disc_cb(uint16_t conn, const struct ble_gatt_error *err,
                       const struct ble_gatt_chr *chr, void *arg) {
    (void)arg;
    if (err->status != 0) return 0;
    /* chr discovery returns both notify + write chars; grab handles by uuid. */
    if (ble_uuid_cmp(&chr->uuid.u, &IFIT_NOTIFY.u) == 0) {
        s_notify_handle = chr->val_handle;
        ble_gattc_disc_all_dscs(conn, chr->val_handle, s_svc_end, dsc_disc_cb, NULL);
    } else if (ble_uuid_cmp(&chr->uuid.u, &IFIT_WRITE.u) == 0) {
        s_write_handle = chr->val_handle;
    }
    return 0;
}

static int svc_disc_cb(uint16_t conn, const struct ble_gatt_error *err,
                       const struct ble_gatt_svc *svc, void *arg) {
    (void)arg;
    if (err->status != 0 || svc == NULL) return 0;   /* EDONE or no match */
    s_svc_end = svc->end_handle;
    ble_gattc_disc_all_chrs(conn, svc->start_handle, svc->end_handle, chr_disc_cb, NULL);
    return 0;
}

/* ---- GAP events --------------------------------------------------------- */

static int gap_event_cb(struct ble_gap_event *event, void *arg) {
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        if (s_connecting || s_conn_handle != BLE_HS_CONN_HANDLE_NONE) break;
        struct ble_gap_disc_desc *d = &event->disc;
        if (!adv_has_ifit(d->data, d->length_data)) break;
        ftms_device_t dev;
        dev.addr_type = d->addr.type;
        memcpy(dev.addr, d->addr.val, 6);
        dev.rssi = d->rssi;
        adv_name(d->data, d->length_data, dev.name, FTMS_NAME_LEN);
        s_ndev = ftms_devlist_upsert(s_devs, s_ndev, &dev);
        break;
    }
    case BLE_GAP_EVENT_CONNECT:
        s_connecting = false;
        if (event->connect.status != 0) { if (s_evt_cb) s_evt_cb(0); break; }
        s_conn_handle = event->connect.conn_handle;
        s_notify_handle = s_write_handle = s_svc_end = 0;
        s_distance_m = 0; s_last_rx_us = 0; s_cur_speed_kmh = 0;
        s_req_speed = REQ_SPEED_NONE; s_req_incline = REQ_INCLINE_NONE; s_req_start = false;
        if (s_evt_cb) s_evt_cb(1);
        ble_gattc_disc_svc_by_uuid(s_conn_handle, &IFIT_SVC.u, svc_disc_cb, NULL);
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        poll_stop();
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_connecting = false;
        if (s_evt_cb) s_evt_cb(0);
        break;
    case BLE_GAP_EVENT_NOTIFY_RX:
        if (event->notify_rx.conn_handle == s_conn_handle &&
            event->notify_rx.attr_handle == s_notify_handle)
            handle_notify(event->notify_rx.om);
        break;
    default: break;
    }
    return 0;
}

/* ---- public API --------------------------------------------------------- */

static void start_scan(void) {
    if (s_connecting) { ble_gap_conn_cancel(); s_connecting = false; }
    s_ndev = 0;
    struct ble_gap_disc_params p = { .filter_duplicates = 0, .passive = 0,
        .filter_policy = BLE_HCI_SCAN_FILT_NO_WL };
    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &p, gap_event_cb, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY)
        ESP_LOGE(TAG, "ble_gap_disc failed: %d", rc);
    else
        ESP_LOGI(TAG, "scanning for iFit treadmills…");
}

void machine_ifit_set_addr_type(uint8_t t) { s_own_addr_type = t; }
void machine_ifit_set_data_cb(machine_state_cb cb) { s_cb = cb; }
void machine_ifit_start_scan(void) { start_scan(); }

int machine_ifit_get_devices(ftms_device_t *out, int max) {
    int n = s_ndev < max ? s_ndev : max;
    memcpy(out, s_devs, (size_t)n * sizeof(ftms_device_t));
    ftms_devlist_sort(out, n);
    return n;
}

void machine_ifit_connect(const ftms_device_t *dev) {
    s_connecting = true;
    ble_gap_disc_cancel();
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE)
        ble_gap_terminate(s_conn_handle, 0x13 /* remote user term */);
    ble_addr_t a = { .type = dev->addr_type };
    memcpy(a.val, dev->addr, 6);
    int rc = ble_gap_connect(s_own_addr_type, &a, 30000, NULL, gap_event_cb, NULL);
    if (rc != 0) { s_connecting = false; if (s_evt_cb) s_evt_cb(0); }
}

void machine_ifit_disconnect(void) {
    /* Cancel an in-flight connect too (see machine_ftms_disconnect). */
    if (s_connecting) { ble_gap_conn_cancel(); s_connecting = false; }
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE)
        ble_gap_terminate(s_conn_handle, 0x13 /* remote user term */);
}

bool machine_ifit_connected(void) { return s_conn_handle != BLE_HS_CONN_HANDLE_NONE; }
bool machine_ifit_connecting(void) { return s_connecting; }
bool machine_ifit_is_ifit_adv(const uint8_t *data, uint8_t len) { return adv_has_ifit(data, len); }

bool machine_ifit_set_speed(float kmh) {
    if (!machine_ifit_connected()) return false;
    s_req_speed = kmh;
    return true;
}

bool machine_ifit_set_incline(float pct) {
    if (!machine_ifit_connected()) return false;
    s_req_incline = pct;
    return true;
}

bool machine_ifit_stop(void) { return machine_ifit_set_speed(0); }

int8_t machine_ifit_conn_rssi(void) {
    int8_t r = 0;
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) ble_gap_conn_rssi(s_conn_handle, &r);
    return r;
}
