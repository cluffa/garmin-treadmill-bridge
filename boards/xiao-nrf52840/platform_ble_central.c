#include "platform_ble_central.h"
#include "connect_policy.h"
#include "ftms_parse.h"
#include "ifit_parse.h"
#include "ifit_poll.h"
#include "last_device.h"

#include "app_error.h"
#include "app_timer.h"
#include "ble.h"
#include "ble_gap.h"
#include "ble_gattc.h"
#include "ble_srv_common.h"
#include "nrf_ble_scan.h"
#include "nrf_log.h"
#include "nrf_sdh_ble.h"

#include <string.h>

#define FTMS_SVC_UUID   0x1826
#define TREADMILL_CHR   0x2ACD  /* Treadmill Data (notify) */
#define FTMS_CP_CHR     0x2AD9  /* Fitness Machine Control Point (write) */

#define IFIT_SVC_UUID   0x1533  /* on the vendor base below */
#define IFIT_NOTIFY_CHR 0x1535
#define IFIT_WRITE_CHR  0x1534

#define CONN_CFG_TAG    1       /* must match nrf_sdh_ble_default_cfg_set */

#define IFIT_TICK_MS    500     /* keepalive pace, same as the ESP32 build */
#define IFIT_GAP_RESET_TICKS APP_TIMER_TICKS(30000) /* odometer reset gap */

/* iFit vendor base 00000000-1412-efde-1523-785feabcd123, little-endian.
 * Bytes 12-13 carry the 16-bit uuid (0x1533/34/35). */
static const ble_uuid128_t IFIT_BASE = {{
    0x23,0xd1,0xbc,0xea,0x5f,0x78,0x23,0x15,
    0xde,0xef,0x12,0x14,0x00,0x00,0x00,0x00}};

/* Raw 16-byte iFit service UUID (LE) for matching 128-bit advert lists. */
static const uint8_t IFIT_SVC_RAW[16] = {
    0x23,0xd1,0xbc,0xea,0x5f,0x78,0x23,0x15,
    0xde,0xef,0x12,0x14,0x33,0x15,0x00,0x00};

/* GATT discovery progresses one step per GATTC response event. */
typedef enum {
    DISC_IDLE = 0,
    DISC_SVC,       /* primary service discovery in flight  */
    DISC_CHR,       /* characteristic discovery in flight   */
    DISC_DESC,      /* CCCD descriptor discovery in flight  */
    DISC_CCCD_WR,   /* CCCD write in flight                 */
    DISC_DONE,      /* subscribed, notifications flowing    */
} disc_stage_t;

NRF_BLE_SCAN_DEF(m_scan);
APP_TIMER_DEF(m_ifit_timer);
APP_TIMER_DEF(m_policy_timer);  /* 1 Hz connect_policy tick while scanning */

static central_state_cb s_cb;
static uint16_t         s_conn_handle = BLE_CONN_HANDLE_INVALID;
static central_proto_t  s_proto = CENTRAL_PROTO_NONE;
static uint8_t          s_ifit_uuid_type;   /* from sd_ble_uuid_vs_add */

/* Scan list + auto-connect policy state. */
static ftms_device_t    s_devs[FTMS_MAX_DEVICES];
static int              s_ndev;
static ftms_device_t    s_saved;         /* persisted last-connected      */
static bool             s_have_saved;
static ftms_device_t    s_target;        /* device being connected / live */
static bool             s_connecting;
static ftms_device_t    s_manual;        /* watch-requested override      */
static bool             s_have_manual;
static uint32_t         s_scan_start_ticks;
static disc_stage_t     s_stage = DISC_IDLE;
static uint16_t         s_svc_end;       /* service end handle           */
static uint16_t         s_data_handle;   /* notify char value handle     */
static uint16_t         s_cp_handle;     /* control/write value handle   */
static uint16_t         s_cccd_handle;   /* notify char CCCD             */
static bool             s_write_busy;    /* one WRITE_REQ in flight max  */

/* iFit odometer: the frames carry no distance — integrate from speed. */
static float            s_distance_m;
static uint32_t         s_last_rx_ticks;
static bool             s_have_rx;

/* ---- advert parsing (ported from machine_ftms.c / machine_ifit.c) -------- */

static bool adv_has_uuid16(const uint8_t *data, uint8_t data_len, uint16_t want)
{
    uint8_t i = 0;
    while (i < data_len) {
        uint8_t len = data[i];
        if (len < 1 || (uint16_t)i + 1u + len > data_len) break;
        uint8_t type = data[i + 1];
        if (type == 0x02 || type == 0x03) {
            for (uint8_t j = 2; (uint16_t)j + 1u < (uint16_t)1u + len; j += 2) {
                uint16_t uuid = (uint16_t)(data[i + j]) |
                                ((uint16_t)(data[i + j + 1]) << 8);
                if (uuid == want) return true;
            }
        }
        i += (uint8_t)(len + 1);
    }
    return false;
}

static bool adv_has_ifit(const uint8_t *data, uint8_t len)
{
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

/* Complete/shortened local name (types 0x09/0x08), ported from machine.c. */
static void adv_name(const uint8_t *data, uint8_t len, char *out, int outlen)
{
    out[0] = '\0';
    uint8_t i = 0;
    while (i < len) {
        uint8_t l = data[i];
        if (l < 1 || (uint16_t)i + 1u + l > len) break;
        uint8_t t = data[i + 1];
        if (t == 0x08 || t == 0x09) {
            int nl = l - 1;
            if (nl > outlen - 1) nl = outlen - 1;
            memcpy(out, &data[i + 2], nl);
            out[nl] = '\0';
            return;
        }
        i += (uint8_t)(l + 1);
    }
}

/* ---- discovery steps ------------------------------------------------------ */

static void disc_start(void)
{
    ble_uuid_t svc;
    if (s_proto == CENTRAL_PROTO_IFIT) {
        svc.uuid = IFIT_SVC_UUID;
        svc.type = s_ifit_uuid_type;
    } else {
        svc.uuid = FTMS_SVC_UUID;
        svc.type = BLE_UUID_TYPE_BLE;
    }
    s_stage = DISC_SVC;
    s_svc_end = s_data_handle = s_cp_handle = s_cccd_handle = 0;
    APP_ERROR_CHECK(sd_ble_gattc_primary_services_discover(s_conn_handle,
                                                           0x0001, &svc));
}

static void disc_continue_chrs(uint16_t from)
{
    ble_gattc_handle_range_t r = { .start_handle = from,
                                   .end_handle = s_svc_end };
    s_stage = DISC_CHR;
    APP_ERROR_CHECK(sd_ble_gattc_characteristics_discover(s_conn_handle, &r));
}

static void disc_continue_descs(uint16_t from)
{
    ble_gattc_handle_range_t r = { .start_handle = from,
                                   .end_handle = s_svc_end };
    s_stage = DISC_DESC;
    APP_ERROR_CHECK(sd_ble_gattc_descriptors_discover(s_conn_handle, &r));
}

static void disc_finish_chrs(void)
{
    if (s_data_handle == 0) {
        NRF_LOG_WARNING("central: notify characteristic not found");
        s_stage = DISC_IDLE;
        return;
    }
    if (s_cp_handle == 0) {
        NRF_LOG_WARNING("central: control characteristic missing — writes disabled");
    }
    if (s_data_handle >= s_svc_end) {
        NRF_LOG_WARNING("central: no room for CCCD after notify char");
        s_stage = DISC_IDLE;
        return;
    }
    disc_continue_descs((uint16_t)(s_data_handle + 1));
}

static void cccd_subscribe(void)
{
    static const uint8_t en[2] = {0x01, 0x00};  /* NOTIFY */
    ble_gattc_write_params_t w = {
        .write_op = BLE_GATT_OP_WRITE_REQ,
        .handle   = s_cccd_handle,
        .offset   = 0,
        .len      = sizeof en,
        .p_value  = en,
    };
    s_stage = DISC_CCCD_WR;
    APP_ERROR_CHECK(sd_ble_gattc_write(s_conn_handle, &w));
}

/* ---- iFit keepalive pump --------------------------------------------------- */

/* ifit_poll_tick emits 1..7 frames per tick as fire-and-forget WRITE_CMDs.
 * The SoftDevice queues several; on exhaustion we log and drop, matching the
 * ESP32 build's best-effort write_no_rsp behavior. */
static void ifit_frame_write(const uint8_t *frame, size_t len, void *ctx)
{
    (void)ctx;
    if (s_cp_handle == 0 || s_conn_handle == BLE_CONN_HANDLE_INVALID) return;
    ble_gattc_write_params_t w = {
        .write_op = BLE_GATT_OP_WRITE_CMD,
        .handle   = s_cp_handle,
        .offset   = 0,
        .len      = (uint16_t)len,
        .p_value  = frame,
    };
    uint32_t err = sd_ble_gattc_write(s_conn_handle, &w);
    if (err != NRF_SUCCESS) {
        NRF_LOG_WARNING("central: ifit frame dropped (err %u)", err);
    }
}

static void ifit_timer_cb(void *ctx)
{
    (void)ctx;
    if (s_proto == CENTRAL_PROTO_IFIT && s_stage == DISC_DONE) {
        ifit_poll_tick(ifit_frame_write, NULL);
    }
}

static void subscribed(void)
{
    s_stage = DISC_DONE;
    NRF_LOG_INFO("central: subscribed — notifications active");
    if (s_proto == CENTRAL_PROTO_IFIT) {
        ifit_poll_reset();
        s_distance_m = 0;
        s_have_rx = false;
        APP_ERROR_CHECK(app_timer_start(m_ifit_timer,
                                        APP_TIMER_TICKS(IFIT_TICK_MS), NULL));
        NRF_LOG_INFO("central: iFit init+keepalive started");
    }
}

/* ---- GATTC event handling -------------------------------------------------- */

static void on_svc_disc_rsp(const ble_gattc_evt_t *e)
{
    if (s_stage != DISC_SVC) return;
    if (e->gatt_status != BLE_GATT_STATUS_SUCCESS ||
        e->params.prim_srvc_disc_rsp.count == 0) {
        NRF_LOG_WARNING("central: service not found (status 0x%04X)",
                        e->gatt_status);
        s_stage = DISC_IDLE;
        return;
    }
    const ble_gattc_service_t *svc = &e->params.prim_srvc_disc_rsp.services[0];
    s_svc_end = svc->handle_range.end_handle;
    NRF_LOG_INFO("central: service handles %u-%u",
                 svc->handle_range.start_handle, s_svc_end);
    disc_continue_chrs(svc->handle_range.start_handle);
}

static void on_chr_disc_rsp(const ble_gattc_evt_t *e)
{
    if (s_stage != DISC_CHR) return;
    if (e->gatt_status != BLE_GATT_STATUS_SUCCESS ||
        e->params.char_disc_rsp.count == 0) {
        disc_finish_chrs();     /* range exhausted */
        return;
    }
    uint16_t notify_uuid = (s_proto == CENTRAL_PROTO_IFIT) ? IFIT_NOTIFY_CHR
                                                           : TREADMILL_CHR;
    uint16_t write_uuid  = (s_proto == CENTRAL_PROTO_IFIT) ? IFIT_WRITE_CHR
                                                           : FTMS_CP_CHR;
    uint8_t  uuid_type   = (s_proto == CENTRAL_PROTO_IFIT) ? s_ifit_uuid_type
                                                           : BLE_UUID_TYPE_BLE;
    uint16_t last = 0;
    for (uint16_t i = 0; i < e->params.char_disc_rsp.count; i++) {
        const ble_gattc_char_t *c = &e->params.char_disc_rsp.chars[i];
        last = c->handle_value;
        if (c->uuid.type != uuid_type) continue;
        if (c->uuid.uuid == notify_uuid) {
            s_data_handle = c->handle_value;
            NRF_LOG_INFO("central: notify char @%u", s_data_handle);
        } else if (c->uuid.uuid == write_uuid) {
            s_cp_handle = c->handle_value;
            NRF_LOG_INFO("central: control char @%u", s_cp_handle);
        }
    }
    if (last >= s_svc_end) {
        disc_finish_chrs();
    } else {
        disc_continue_chrs((uint16_t)(last + 1));
    }
}

static void on_desc_disc_rsp(const ble_gattc_evt_t *e)
{
    if (s_stage != DISC_DESC) return;
    uint16_t last = 0;
    if (e->gatt_status == BLE_GATT_STATUS_SUCCESS) {
        for (uint16_t i = 0; i < e->params.desc_disc_rsp.count; i++) {
            const ble_gattc_desc_t *d = &e->params.desc_disc_rsp.descs[i];
            last = d->handle;
            if (d->uuid.type == BLE_UUID_TYPE_BLE &&
                d->uuid.uuid == BLE_UUID_DESCRIPTOR_CLIENT_CHAR_CONFIG) {
                s_cccd_handle = d->handle;
                NRF_LOG_INFO("central: CCCD @%u", s_cccd_handle);
                cccd_subscribe();
                return;
            }
        }
        if (last != 0 && last < s_svc_end) {
            disc_continue_descs((uint16_t)(last + 1));   /* next batch */
            return;
        }
    }
    NRF_LOG_WARNING("central: CCCD not found — no notifications");
    s_stage = DISC_IDLE;
}

static void on_write_rsp(const ble_gattc_evt_t *e)
{
    if (s_stage == DISC_CCCD_WR &&
        e->params.write_rsp.handle == s_cccd_handle) {
        subscribed();
        return;
    }
    s_write_busy = false;
    if (e->gatt_status != BLE_GATT_STATUS_SUCCESS) {
        NRF_LOG_WARNING("central: CP write failed 0x%04X", e->gatt_status);
    }
}

static void on_hvx_ifit(const ble_gattc_evt_hvx_t *h)
{
    float speed_mps, incline_pct;
    if (!ifit_parse_data(h->data, h->len, &speed_mps, &incline_pct)) return;

    /* Integrate distance from speed (frames carry none); >30 s gap means the
     * treadmill restarted while BLE stayed up — reset the odometer. */
    uint32_t now = app_timer_cnt_get();
    if (s_have_rx) {
        uint32_t gap = app_timer_cnt_diff_compute(now, s_last_rx_ticks);
        if (gap > IFIT_GAP_RESET_TICKS) {
            s_distance_m = 0;
            NRF_LOG_INFO("central: rx gap — distance reset");
        } else {
            s_distance_m += speed_mps *
                ((float)gap / (float)APP_TIMER_TICKS(1000));
        }
    }
    s_last_rx_ticks = now;
    s_have_rx = true;

    ifit_poll_note_speed(speed_mps * 3.6f);

    treadmill_state_t st = {
        .speed_mps = speed_mps,
        .distance_m = s_distance_m,
        .incline_pct = incline_pct,
        .elapsed_s = 0,
    };
    if (s_cb) s_cb(&st);
}

static void on_hvx(const ble_gattc_evt_t *e)
{
    const ble_gattc_evt_hvx_t *h = &e->params.hvx;
    if (h->handle != s_data_handle) return;

    if (s_proto == CENTRAL_PROTO_IFIT) {
        on_hvx_ifit(h);
        return;
    }
    treadmill_state_t st;
    if (ftms_parse_treadmill_data(h->data, h->len, &st)) {
        if (s_cb) s_cb(&st);
    } else {
        NRF_LOG_WARNING("central: unparsed treadmill frame len=%u", h->len);
    }
}

/* ---- device list + connect policy ------------------------------------------- */

static uint32_t ms_since_scan_start(void)
{
    uint32_t diff = app_timer_cnt_diff_compute(app_timer_cnt_get(),
                                               s_scan_start_ticks);
    /* 32768 ticks/s → ms; 32-bit safe for the < 9 min the RTC can span. */
    return (uint32_t)(((uint64_t)diff * 1000u) / APP_TIMER_TICKS(1000));
}

static void connect_to(const ftms_device_t *dev)
{
    nrf_ble_scan_stop();
    (void)app_timer_stop(m_policy_timer);

    s_target = *dev;
    s_connecting = true;

    ble_gap_addr_t addr;
    memset(&addr, 0, sizeof addr);
    addr.addr_type = dev->addr_type;
    memcpy(addr.addr, dev->addr, 6);

    static const ble_gap_scan_params_t scan_params = {
        .active        = 0,
        .interval      = NRF_BLE_SCAN_SCAN_INTERVAL,
        .window        = NRF_BLE_SCAN_SCAN_WINDOW,
        .timeout       = 500,   /* 5 s to acquire, else GAP_TIMEOUT_SRC_CONN */
        .scan_phys     = BLE_GAP_PHY_1MBPS,
        .filter_policy = BLE_GAP_SCAN_FP_ACCEPT_ALL,
    };
    static const ble_gap_conn_params_t conn_params = {
        .min_conn_interval = NRF_BLE_SCAN_MIN_CONNECTION_INTERVAL,
        .max_conn_interval = NRF_BLE_SCAN_MAX_CONNECTION_INTERVAL,
        .slave_latency     = NRF_BLE_SCAN_SLAVE_LATENCY,
        .conn_sup_timeout  = NRF_BLE_SCAN_SUPERVISION_TIMEOUT,
    };

    uint32_t err = sd_ble_gap_connect(&addr, &scan_params, &conn_params,
                                      CONN_CFG_TAG);
    if (err != NRF_SUCCESS) {
        NRF_LOG_WARNING("central: connect req err 0x%x — rescanning", err);
        s_connecting = false;
        platform_ble_central_start_scan();
        return;
    }
    NRF_LOG_INFO("central: connecting to \"%s\" (%s)",
                 nrf_log_push((char *)s_target.name),
                 s_target.proto == MACHINE_PROTO_IFIT ? "iFit" : "FTMS");
}

/* Evaluate the auto-connect policy; runs on every advert and each policy
 * tick. Never fires while connected/connecting or a manual pick is staged. */
static void policy_evaluate(void)
{
    if (s_conn_handle != BLE_CONN_HANDLE_INVALID || s_connecting ||
        s_have_manual) {
        return;
    }
    int pick = connect_policy_choose(s_devs, s_ndev,
                                     s_have_saved ? &s_saved : NULL,
                                     ms_since_scan_start());
    if (pick >= 0) {
        connect_to(&s_devs[pick]);
    }
}

static void policy_timer_cb(void *ctx)
{
    (void)ctx;
    policy_evaluate();
}

static void on_adv_report(const ble_gap_evt_adv_report_t *r)
{
    char name[FTMS_NAME_LEN];
    adv_name(r->data.p_data, (uint8_t)r->data.len, name, FTMS_NAME_LEN);

    int proto = -1;
    if (adv_has_ifit(r->data.p_data, (uint8_t)r->data.len)) {
        proto = MACHINE_PROTO_IFIT;
    } else if (adv_has_uuid16(r->data.p_data, (uint8_t)r->data.len,
                              FTMS_SVC_UUID)) {
        proto = MACHINE_PROTO_FTMS;
    }

    if (proto >= 0) {
        ftms_device_t dev;
        memset(&dev, 0, sizeof dev);
        dev.addr_type = r->peer_addr.addr_type;
        memcpy(dev.addr, r->peer_addr.addr, 6);
        dev.rssi = r->rssi;
        dev.proto = (uint8_t)proto;
        memcpy(dev.name, name, FTMS_NAME_LEN);
        int before = s_ndev;
        s_ndev = ftms_devlist_upsert(s_devs, s_ndev, &dev);
        if (s_ndev != before) {
            NRF_LOG_INFO("central: found \"%s\" rssi %d (%s)",
                         nrf_log_push(name), dev.rssi,
                         proto == MACHINE_PROTO_IFIT ? "iFit" : "FTMS");
        }
    } else if (name[0]) {
        /* Name-only scan response: refresh the name of a known device (the
         * UUID often arrives only in the primary advert). */
        for (int i = 0; i < s_ndev; i++) {
            if (memcmp(s_devs[i].addr, r->peer_addr.addr, 6) == 0) {
                memcpy(s_devs[i].name, name, FTMS_NAME_LEN);
                break;
            }
        }
    }

    policy_evaluate();
}

/* ---- GAP + dispatch --------------------------------------------------------- */

static void ble_evt_handler(const ble_evt_t *p_evt, void *p_ctx)
{
    (void)p_ctx;
    const ble_gap_evt_t *gap = &p_evt->evt.gap_evt;

    switch (p_evt->header.evt_id) {
    case BLE_GAP_EVT_CONNECTED:
        if (gap->params.connected.role != BLE_GAP_ROLE_CENTRAL) break;
        /* One machine connection at a time — the single central link slot
         * (NRF_SDH_BLE_CENTRAL_LINK_COUNT=1) enforces the ESP32 invariant
         * structurally; this guard just makes a violation loud. */
        if (s_conn_handle != BLE_CONN_HANDLE_INVALID) {
            NRF_LOG_ERROR("central: INVARIANT VIOLATION — second machine link");
            (void)sd_ble_gap_disconnect(gap->conn_handle,
                     BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
            break;
        }
        s_conn_handle = gap->conn_handle;
        s_connecting = false;
        s_proto = (s_target.proto == MACHINE_PROTO_IFIT) ? CENTRAL_PROTO_IFIT
                                                         : CENTRAL_PROTO_FTMS;
        s_write_busy = false;
        NRF_LOG_INFO("central: connected \"%s\" (%s, handle %u)",
                     nrf_log_push((char *)s_target.name),
                     s_proto == CENTRAL_PROTO_IFIT ? "iFit" : "FTMS",
                     s_conn_handle);
        /* Remember for next power-up (skipped when unchanged). */
        s_saved = s_target;
        s_have_saved = true;
        last_device_save(&s_target);
        disc_start();
        break;

    case BLE_GAP_EVT_DISCONNECTED:
        if (gap->conn_handle != s_conn_handle) break;
        NRF_LOG_INFO("central: disconnected (reason 0x%02X)",
                     gap->params.disconnected.reason);
        (void)app_timer_stop(m_ifit_timer);
        s_conn_handle = BLE_CONN_HANDLE_INVALID;
        s_proto = CENTRAL_PROTO_NONE;
        s_stage = DISC_IDLE;
        if (s_have_manual) {     /* watch-picked switch: connect right away */
            s_have_manual = false;
            connect_to(&s_manual);
        } else {
            platform_ble_central_start_scan();
        }
        break;

    case BLE_GAP_EVT_TIMEOUT:
        if (gap->params.timeout.src == BLE_GAP_TIMEOUT_SRC_CONN) {
            NRF_LOG_WARNING("central: connect timed out — rescanning");
            s_connecting = false;
            s_have_manual = false;   /* picked device gone; policy resumes */
            platform_ble_central_start_scan();
        }
        break;

    case BLE_GATTC_EVT_PRIM_SRVC_DISC_RSP:
        on_svc_disc_rsp(&p_evt->evt.gattc_evt);
        break;
    case BLE_GATTC_EVT_CHAR_DISC_RSP:
        on_chr_disc_rsp(&p_evt->evt.gattc_evt);
        break;
    case BLE_GATTC_EVT_DESC_DISC_RSP:
        on_desc_disc_rsp(&p_evt->evt.gattc_evt);
        break;
    case BLE_GATTC_EVT_WRITE_RSP:
        on_write_rsp(&p_evt->evt.gattc_evt);
        break;
    case BLE_GATTC_EVT_HVX:
        on_hvx(&p_evt->evt.gattc_evt);
        break;
    case BLE_GATTC_EVT_TIMEOUT:
        NRF_LOG_WARNING("central: GATT timeout — disconnecting");
        (void)sd_ble_gap_disconnect(p_evt->evt.gattc_evt.conn_handle,
                                    BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
        break;
    default:
        break;
    }
}

NRF_SDH_BLE_OBSERVER(m_central_obs, 3 /* prio */, ble_evt_handler, NULL);

static void scan_evt_handler(const scan_evt_t *p_evt)
{
    /* No module filters are enabled, so every advert arrives as NOT_FOUND;
     * classification (FTMS/iFit/name-refresh) happens in on_adv_report. */
    switch (p_evt->scan_evt_id) {
    case NRF_BLE_SCAN_EVT_NOT_FOUND:
        on_adv_report(p_evt->params.p_not_found);
        break;
    case NRF_BLE_SCAN_EVT_FILTER_MATCH:
        on_adv_report(p_evt->params.filter_match.p_adv_report);
        break;
    default:
        break;
    }
}

/* ---- public API --------------------------------------------------------------- */

void platform_ble_central_init(central_state_cb cb)
{
    s_cb = cb;

    s_have_saved = last_device_load(&s_saved);
    if (s_have_saved) {
        NRF_LOG_INFO("central: last device \"%s\"",
                     nrf_log_push((char *)s_saved.name));
    }

    APP_ERROR_CHECK(sd_ble_uuid_vs_add(&IFIT_BASE, &s_ifit_uuid_type));

    APP_ERROR_CHECK(app_timer_create(&m_ifit_timer, APP_TIMER_MODE_REPEATED,
                                     ifit_timer_cb));
    APP_ERROR_CHECK(app_timer_create(&m_policy_timer, APP_TIMER_MODE_REPEATED,
                                     policy_timer_cb));

    nrf_ble_scan_init_t init = {
        .connect_if_match = false,   /* connect_policy decides, not the scan */
        .conn_cfg_tag     = CONN_CFG_TAG,
        .p_scan_param     = NULL,   /* NRF_BLE_SCAN_* app_config values */
        .p_conn_param     = NULL,
    };
    APP_ERROR_CHECK(nrf_ble_scan_init(&m_scan, &init, scan_evt_handler));
}

void platform_ble_central_start_scan(void)
{
    s_ndev = 0;
    if (platform_ble_central_connected()) {
        /* A connected treadmill stops advertising; seed it so the watch's
         * picker still lists it (idx 0). */
        s_ndev = ftms_devlist_upsert(s_devs, s_ndev, &s_target);
    }
    s_scan_start_ticks = app_timer_cnt_get();
    APP_ERROR_CHECK(nrf_ble_scan_start(&m_scan));
    (void)app_timer_start(m_policy_timer, APP_TIMER_TICKS(1000), NULL);
    NRF_LOG_INFO("central: scanning for treadmills…");
}

bool platform_ble_central_connected(void)
{
    return s_conn_handle != BLE_CONN_HANDLE_INVALID;
}

bool platform_ble_central_connecting(void)
{
    return s_connecting;
}

central_proto_t platform_ble_central_proto(void)
{
    return platform_ble_central_connected() ? s_proto : CENTRAL_PROTO_NONE;
}

int platform_ble_central_get_devices(ftms_device_t *out, int max)
{
    int n = s_ndev < max ? s_ndev : max;
    memcpy(out, s_devs, (size_t)n * sizeof *out);
    return n;
}

void platform_ble_central_connect(const ftms_device_t *dev)
{
    s_manual = *dev;

    if (s_conn_handle != BLE_CONN_HANDLE_INVALID) {
        if (memcmp(s_target.addr, dev->addr, 6) == 0) return;  /* already on it */
        /* Tear down first; DISCONNECTED sees s_have_manual and connects. */
        s_have_manual = true;
        (void)sd_ble_gap_disconnect(s_conn_handle,
                                    BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
        return;
    }
    if (s_connecting) {
        (void)sd_ble_gap_connect_cancel();
        s_connecting = false;
    } else {
        nrf_ble_scan_stop();
    }
    s_have_manual = false;
    connect_to(&s_manual);
}

const ftms_device_t *platform_ble_central_device(void)
{
    return platform_ble_central_connected() ? &s_target : NULL;
}

/* ---- treadmill control ---------------------------------------------------------- */

#define FTMS_OP_SET_SPEED    0x02   /* uint16, 0.01 km/h */
#define FTMS_OP_SET_INCLINE  0x03   /* int16,  0.1 %     */
#define FTMS_OP_STOP         0x08

/* Parity note: like the ESP32 machine_ftms.c, no Request Control (op 0x00)
 * handshake is sent first. The mock treadmill and tested machines accept
 * writes without it; add the handshake here if a real FTMS belt NAKs. */
static bool cp_write(uint8_t opcode, const uint8_t *param, uint8_t param_len)
{
    if (s_cp_handle == 0 || !platform_ble_central_connected()) return false;
    if (s_stage != DISC_DONE || s_write_busy) return false;

    uint8_t buf[3];
    buf[0] = opcode;
    if (param_len > 0) memcpy(buf + 1, param, param_len);

    ble_gattc_write_params_t w = {
        .write_op = BLE_GATT_OP_WRITE_REQ,
        .handle   = s_cp_handle,
        .offset   = 0,
        .len      = (uint16_t)(1 + param_len),
        .p_value  = buf,
    };
    uint32_t err = sd_ble_gattc_write(s_conn_handle, &w);
    if (err != NRF_SUCCESS) {
        NRF_LOG_WARNING("central: CP write err %u", err);
        return false;
    }
    s_write_busy = true;
    return true;
}

bool platform_ble_central_set_speed(float kmh)
{
    if (s_proto == CENTRAL_PROTO_IFIT) {
        if (!platform_ble_central_connected() || kmh < 0) return false;
        ifit_poll_request_speed(kmh);
        return true;
    }
    /* Clamp before the cast — same rules as machine_ftms.c. */
    if (kmh < 0) kmh = 0;
    if (kmh > 25) kmh = 25;
    uint16_t val = (uint16_t)(kmh * 100.0f + 0.5f);
    uint8_t p[2] = { (uint8_t)(val & 0xFF), (uint8_t)(val >> 8) };
    return cp_write(FTMS_OP_SET_SPEED, p, 2);
}

bool platform_ble_central_set_incline(float pct)
{
    if (s_proto == CENTRAL_PROTO_IFIT) {
        if (!platform_ble_central_connected()) return false;
        ifit_poll_request_incline(pct);
        return true;
    }
    if (pct < -10) pct = -10;
    if (pct > 25) pct = 25;
    int16_t val = (int16_t)(pct * 10.0f);
    uint8_t p[2] = { (uint8_t)((uint16_t)val & 0xFF),
                     (uint8_t)((uint16_t)val >> 8) };
    return cp_write(FTMS_OP_SET_INCLINE, p, 2);
}

bool platform_ble_central_stop(void)
{
    if (s_proto == CENTRAL_PROTO_IFIT) {
        if (!platform_ble_central_connected()) return false;
        ifit_poll_request_stop();
        return true;
    }
    return cp_write(FTMS_OP_STOP, NULL, 0);
}
