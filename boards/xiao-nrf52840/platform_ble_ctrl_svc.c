#include "platform_ble_ctrl_svc.h"
#include "ctrl_dispatch.h"

#include "app_error.h"
#include "ble.h"
#include "ble_advdata.h"
#include "ble_gap.h"
#include "ble_gatts.h"
#include "ble_srv_common.h"
#include "nrf_log.h"
#include "nrf_sdh_ble.h"

#include <string.h>

#define DEVICE_NAME     "TMILL-CTRL"
#define CONN_CFG_TAG    1

/* A6ED0001-D344-460A-8075-B9E8EC90D71B, little-endian base with the 16-bit
 * alias bytes (12-13) zeroed; service = 0x0001, control char = 0x0002. */
static const ble_uuid128_t CTRL_BASE = {{
    0x1B,0xD7,0x90,0xEC,0xE8,0xB9,0x75,0x80,
    0x0A,0x46,0x44,0xD3,0x00,0x00,0xED,0xA6}};
#define CTRL_SVC_UUID   0x0001
#define CTRL_CHR_UUID   0x0002

#define CTRL_CHR_MAX_LEN 64     /* longest ctrl_dispatch line we accept */

/* Low-duty connectable advertising: 285 ms interval, no timeout. Leaves air
 * time for the ANT channel and the treadmill connection. */
#define ADV_INTERVAL    456     /* × 0.625 ms */

static uint8_t  s_uuid_type;
static uint16_t s_svc_handle;
static ble_gatts_char_handles_t s_chr_handles;
static uint16_t s_conn_handle = BLE_CONN_HANDLE_INVALID;

static uint8_t s_adv_handle = BLE_GAP_ADV_SET_HANDLE_NOT_SET;
static uint8_t s_adv_buf[BLE_GAP_ADV_SET_DATA_SIZE_MAX];
static uint8_t s_srsp_buf[BLE_GAP_ADV_SET_DATA_SIZE_MAX];
static ble_gap_adv_data_t s_adv_data = {
    .adv_data      = { .p_data = s_adv_buf,  .len = sizeof s_adv_buf  },
    .scan_rsp_data = { .p_data = s_srsp_buf, .len = sizeof s_srsp_buf },
};

/* ctrl_dispatch responses go to RTT — there is no notify char (the data
 * field is send-only) and GATT gives the writer its ACK at the ATT layer. */
static void ctrl_rtt_tx(const char *msg, void *ctx)
{
    (void)ctx;
    NRF_LOG_INFO("ctrl: %s", nrf_log_push((char *)msg));
}

static void advertising_start(void)
{
    uint32_t err = sd_ble_gap_adv_start(s_adv_handle, CONN_CFG_TAG);
    if (err == NRF_SUCCESS) {
        NRF_LOG_INFO("ctrl_svc: advertising as %s", DEVICE_NAME);
    } else if (err != NRF_ERROR_INVALID_STATE) {   /* already advertising */
        APP_ERROR_CHECK(err);
    }
}

static void on_write(const ble_gatts_evt_write_t *w)
{
    if (w->handle != s_chr_handles.value_handle) return;

    char line[CTRL_CHR_MAX_LEN + 1];
    uint16_t n = w->len < CTRL_CHR_MAX_LEN ? w->len : CTRL_CHR_MAX_LEN;
    memcpy(line, w->data, n);
    line[n] = '\0';
    NRF_LOG_INFO("ctrl_svc: rx \"%s\"", nrf_log_push(line));
    ctrl_dispatch(line, ctrl_rtt_tx, NULL);
}

static void ble_evt_handler(const ble_evt_t *p_evt, void *p_ctx)
{
    (void)p_ctx;
    const ble_gap_evt_t *gap = &p_evt->evt.gap_evt;

    switch (p_evt->header.evt_id) {
    case BLE_GAP_EVT_CONNECTED:
        if (gap->params.connected.role != BLE_GAP_ROLE_PERIPH) break;
        s_conn_handle = gap->conn_handle;
        NRF_LOG_INFO("ctrl_svc: data field connected (handle %u)",
                     s_conn_handle);
        break;

    case BLE_GAP_EVT_DISCONNECTED:
        if (gap->conn_handle != s_conn_handle) break;
        s_conn_handle = BLE_CONN_HANDLE_INVALID;
        NRF_LOG_INFO("ctrl_svc: data field disconnected — readvertising");
        advertising_start();
        break;

    case BLE_GATTS_EVT_WRITE:
        if (p_evt->evt.gatts_evt.conn_handle == s_conn_handle) {
            on_write(&p_evt->evt.gatts_evt.params.write);
        }
        break;

    default:
        break;
    }
}

NRF_SDH_BLE_OBSERVER(m_ctrl_svc_obs, 3 /* prio */, ble_evt_handler, NULL);

static void service_init(void)
{
    APP_ERROR_CHECK(sd_ble_uuid_vs_add(&CTRL_BASE, &s_uuid_type));

    ble_uuid_t svc_uuid = { .uuid = CTRL_SVC_UUID, .type = s_uuid_type };
    APP_ERROR_CHECK(sd_ble_gatts_service_add(BLE_GATTS_SRVC_TYPE_PRIMARY,
                                             &svc_uuid, &s_svc_handle));

    ble_gatts_char_md_t char_md = {
        .char_props = { .write = 1, .write_wo_resp = 1 },
    };
    ble_gatts_attr_md_t attr_md = {
        .vloc = BLE_GATTS_VLOC_STACK,
        .vlen = 1,
    };
    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&attr_md.read_perm);
    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&attr_md.write_perm);

    ble_uuid_t chr_uuid = { .uuid = CTRL_CHR_UUID, .type = s_uuid_type };
    ble_gatts_attr_t attr = {
        .p_uuid    = &chr_uuid,
        .p_attr_md = &attr_md,
        .init_len  = 0,
        .max_len   = CTRL_CHR_MAX_LEN,
    };
    APP_ERROR_CHECK(sd_ble_gatts_characteristic_add(s_svc_handle, &char_md,
                                                    &attr, &s_chr_handles));
}

static void advertising_init(void)
{
    ble_gap_conn_sec_mode_t sec;
    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&sec);
    APP_ERROR_CHECK(sd_ble_gap_device_name_set(&sec,
                                               (const uint8_t *)DEVICE_NAME,
                                               strlen(DEVICE_NAME)));

    /* Advert: flags + the 128-bit service UUID (fills most of the 31 B);
     * the name goes in the scan response. */
    ble_uuid_t adv_uuid = { .uuid = CTRL_SVC_UUID, .type = s_uuid_type };
    ble_advdata_t advdata = {
        .flags = BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE,
        .uuids_complete = { .uuid_cnt = 1, .p_uuids = &adv_uuid },
    };
    ble_advdata_t srdata = {
        .name_type = BLE_ADVDATA_FULL_NAME,
    };
    APP_ERROR_CHECK(ble_advdata_encode(&advdata, s_adv_data.adv_data.p_data,
                                       &s_adv_data.adv_data.len));
    APP_ERROR_CHECK(ble_advdata_encode(&srdata,
                                       s_adv_data.scan_rsp_data.p_data,
                                       &s_adv_data.scan_rsp_data.len));

    ble_gap_adv_params_t params = {
        .properties = { .type =
            BLE_GAP_ADV_TYPE_CONNECTABLE_SCANNABLE_UNDIRECTED },
        .interval   = ADV_INTERVAL,
        .duration   = 0,   /* forever */
        .filter_policy = BLE_GAP_ADV_FP_ANY,
        .primary_phy   = BLE_GAP_PHY_1MBPS,
    };
    APP_ERROR_CHECK(sd_ble_gap_adv_set_configure(&s_adv_handle, &s_adv_data,
                                                 &params));
}

void platform_ble_ctrl_svc_init(void)
{
    service_init();
    advertising_init();
    advertising_start();
}

bool platform_ble_ctrl_svc_connected(void)
{
    return s_conn_handle != BLE_CONN_HANDLE_INVALID;
}
