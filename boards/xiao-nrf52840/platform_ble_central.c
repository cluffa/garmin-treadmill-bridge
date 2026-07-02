#include "platform_ble_central.h"
#include "ftms_parse.h"

#include "app_error.h"
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

#define CONN_CFG_TAG    1       /* must match nrf_sdh_ble_default_cfg_set */

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

static central_state_cb s_cb;
static uint16_t         s_conn_handle = BLE_CONN_HANDLE_INVALID;
static central_proto_t  s_proto = CENTRAL_PROTO_NONE;
static disc_stage_t     s_stage = DISC_IDLE;
static uint16_t         s_svc_end;       /* service end handle           */
static uint16_t         s_data_handle;   /* Treadmill Data value handle  */
static uint16_t         s_cp_handle;     /* Control Point value handle   */
static uint16_t         s_cccd_handle;   /* Treadmill Data CCCD          */
static uint16_t         s_chr_disc_next; /* next handle for chr disc     */
static bool             s_write_busy;    /* one WRITE_REQ in flight max  */

/* ---- discovery steps ----------------------------------------------------- */

static void disc_start(void)
{
    ble_uuid_t ftms = { .uuid = FTMS_SVC_UUID, .type = BLE_UUID_TYPE_BLE };
    s_stage = DISC_SVC;
    s_svc_end = s_data_handle = s_cp_handle = s_cccd_handle = 0;
    APP_ERROR_CHECK(sd_ble_gattc_primary_services_discover(s_conn_handle,
                                                           0x0001, &ftms));
}

static void disc_continue_chrs(uint16_t from)
{
    ble_gattc_handle_range_t r = { .start_handle = from,
                                   .end_handle = s_svc_end };
    s_stage = DISC_CHR;
    APP_ERROR_CHECK(sd_ble_gattc_characteristics_discover(s_conn_handle, &r));
}

static void disc_finish_chrs(void)
{
    if (s_data_handle == 0) {
        NRF_LOG_WARNING("central: Treadmill Data (0x%04X) not found",
                        TREADMILL_CHR);
        s_stage = DISC_IDLE;
        return;
    }
    if (s_cp_handle == 0) {
        NRF_LOG_WARNING("central: FTMS Control Point missing — writes disabled");
    }
    ble_gattc_handle_range_t r = { .start_handle = (uint16_t)(s_data_handle + 1),
                                   .end_handle = s_svc_end };
    s_stage = DISC_DESC;
    APP_ERROR_CHECK(sd_ble_gattc_descriptors_discover(s_conn_handle, &r));
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

/* ---- GATTC event handling ------------------------------------------------ */

static void on_svc_disc_rsp(const ble_gattc_evt_t *e)
{
    if (s_stage != DISC_SVC) return;
    if (e->gatt_status != BLE_GATT_STATUS_SUCCESS ||
        e->params.prim_srvc_disc_rsp.count == 0) {
        NRF_LOG_WARNING("central: FTMS service not found (status 0x%04X)",
                        e->gatt_status);
        s_stage = DISC_IDLE;
        return;
    }
    const ble_gattc_service_t *svc = &e->params.prim_srvc_disc_rsp.services[0];
    s_svc_end = svc->handle_range.end_handle;
    NRF_LOG_INFO("central: FTMS service handles %u-%u",
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
    uint16_t last = 0;
    for (uint16_t i = 0; i < e->params.char_disc_rsp.count; i++) {
        const ble_gattc_char_t *c = &e->params.char_disc_rsp.chars[i];
        last = c->handle_value;
        if (c->uuid.type == BLE_UUID_TYPE_BLE) {
            if (c->uuid.uuid == TREADMILL_CHR) {
                s_data_handle = c->handle_value;
                NRF_LOG_INFO("central: Treadmill Data @%u", s_data_handle);
            } else if (c->uuid.uuid == FTMS_CP_CHR) {
                s_cp_handle = c->handle_value;
                NRF_LOG_INFO("central: FTMS Control Point @%u", s_cp_handle);
            }
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
    if (e->gatt_status == BLE_GATT_STATUS_SUCCESS) {
        for (uint16_t i = 0; i < e->params.desc_disc_rsp.count; i++) {
            const ble_gattc_desc_t *d = &e->params.desc_disc_rsp.descs[i];
            if (d->uuid.type == BLE_UUID_TYPE_BLE &&
                d->uuid.uuid == BLE_UUID_DESCRIPTOR_CLIENT_CHAR_CONFIG) {
                s_cccd_handle = d->handle;
                NRF_LOG_INFO("central: CCCD @%u", s_cccd_handle);
                cccd_subscribe();
                return;
            }
        }
    }
    NRF_LOG_WARNING("central: CCCD not found — no notifications");
    s_stage = DISC_IDLE;
}

static void on_write_rsp(const ble_gattc_evt_t *e)
{
    if (s_stage == DISC_CCCD_WR &&
        e->params.write_rsp.handle == s_cccd_handle) {
        s_stage = DISC_DONE;
        NRF_LOG_INFO("central: subscribed — notifications active");
        return;
    }
    s_write_busy = false;
    if (e->gatt_status != BLE_GATT_STATUS_SUCCESS) {
        NRF_LOG_WARNING("central: CP write failed 0x%04X", e->gatt_status);
    }
}

static void on_hvx(const ble_gattc_evt_t *e)
{
    const ble_gattc_evt_hvx_t *h = &e->params.hvx;
    if (h->handle != s_data_handle) return;

    treadmill_state_t st;
    if (ftms_parse_treadmill_data(h->data, h->len, &st)) {
        if (s_cb) s_cb(&st);
    } else {
        NRF_LOG_WARNING("central: unparsed treadmill frame len=%u", h->len);
    }
}

/* ---- GAP + dispatch ------------------------------------------------------ */

static void ble_evt_handler(const ble_evt_t *p_evt, void *p_ctx)
{
    (void)p_ctx;
    const ble_gap_evt_t *gap = &p_evt->evt.gap_evt;

    switch (p_evt->header.evt_id) {
    case BLE_GAP_EVT_CONNECTED:
        if (gap->params.connected.role != BLE_GAP_ROLE_CENTRAL) break;
        s_conn_handle = gap->conn_handle;
        s_proto = CENTRAL_PROTO_FTMS;
        s_write_busy = false;
        NRF_LOG_INFO("central: connected FTMS (handle %u)", s_conn_handle);
        disc_start();
        break;

    case BLE_GAP_EVT_DISCONNECTED:
        if (gap->conn_handle != s_conn_handle) break;
        NRF_LOG_INFO("central: disconnected (reason 0x%02X) — rescanning",
                     gap->params.disconnected.reason);
        s_conn_handle = BLE_CONN_HANDLE_INVALID;
        s_proto = CENTRAL_PROTO_NONE;
        s_stage = DISC_IDLE;
        platform_ble_central_start_scan();
        break;

    case BLE_GAP_EVT_TIMEOUT:
        if (gap->params.timeout.src == BLE_GAP_TIMEOUT_SRC_CONN) {
            NRF_LOG_WARNING("central: connect timed out — rescanning");
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
    switch (p_evt->scan_evt_id) {
    case NRF_BLE_SCAN_EVT_CONNECTING_ERROR:
        NRF_LOG_WARNING("central: connect request failed (%u) — rescanning",
                        p_evt->params.connecting_err.err_code);
        platform_ble_central_start_scan();
        break;
    default:
        break;
    }
}

/* ---- public API ----------------------------------------------------------- */

void platform_ble_central_init(central_state_cb cb)
{
    s_cb = cb;

    nrf_ble_scan_init_t init = {
        .connect_if_match = true,
        .conn_cfg_tag     = CONN_CFG_TAG,
        .p_scan_param     = NULL,   /* NRF_BLE_SCAN_* app_config values */
        .p_conn_param     = NULL,
    };
    APP_ERROR_CHECK(nrf_ble_scan_init(&m_scan, &init, scan_evt_handler));

    ble_uuid_t ftms = { .uuid = FTMS_SVC_UUID, .type = BLE_UUID_TYPE_BLE };
    APP_ERROR_CHECK(nrf_ble_scan_filter_set(&m_scan, SCAN_UUID_FILTER, &ftms));
    APP_ERROR_CHECK(nrf_ble_scan_filters_enable(&m_scan,
                                                NRF_BLE_SCAN_UUID_FILTER,
                                                false));
}

void platform_ble_central_start_scan(void)
{
    APP_ERROR_CHECK(nrf_ble_scan_start(&m_scan));
    NRF_LOG_INFO("central: scanning for treadmills…");
}

bool platform_ble_central_connected(void)
{
    return s_conn_handle != BLE_CONN_HANDLE_INVALID;
}

central_proto_t platform_ble_central_proto(void)
{
    return platform_ble_central_connected() ? s_proto : CENTRAL_PROTO_NONE;
}

/* ---- FTMS Control Point writes -------------------------------------------- */

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
    /* Clamp before the cast — same rules as machine_ftms.c. */
    if (kmh < 0) kmh = 0;
    if (kmh > 25) kmh = 25;
    uint16_t val = (uint16_t)(kmh * 100.0f + 0.5f);
    uint8_t p[2] = { (uint8_t)(val & 0xFF), (uint8_t)(val >> 8) };
    return cp_write(FTMS_OP_SET_SPEED, p, 2);
}

bool platform_ble_central_set_incline(float pct)
{
    if (pct < -10) pct = -10;
    if (pct > 25) pct = 25;
    int16_t val = (int16_t)(pct * 10.0f);
    uint8_t p[2] = { (uint8_t)((uint16_t)val & 0xFF),
                     (uint8_t)((uint16_t)val >> 8) };
    return cp_write(FTMS_OP_SET_INCLINE, p, 2);
}

bool platform_ble_central_stop(void)
{
    return cp_write(FTMS_OP_STOP, NULL, 0);
}
