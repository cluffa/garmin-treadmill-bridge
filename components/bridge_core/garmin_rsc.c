/*
 * garmin_rsc.c — BLE GATT server for Running Speed & Cadence (RSC, 0x1814).
 *
 * This module owns only the GATT table and notification logic.
 * Advertising and GAP event handling are owned by nus_ctrl.c (the single
 * peripheral advertiser), which calls garmin_rsc_on_gap_event() so RSC can
 * track subscriptions and connection handles.
 */

#include "garmin_rsc.h"
#include "rsc_encode.h"

#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "esp_log.h"

#include <string.h>
#include <stdint.h>

#define TAG              "garmin_rsc"
#define RSC_SVC_UUID     0x1814
#define RSC_MEAS_UUID    0x2A53
#define RSC_FEATURE_UUID 0x2A54
#define BATT_SVC_UUID    0x180F
#define BATT_LVL_UUID    0x2A19

#define RSC_FEATURE_BITS 0x0006u
#define RSC_MEAS_BUF_LEN 16

static uint8_t  s_own_addr_type = BLE_OWN_ADDR_RANDOM;
static uint16_t s_meas_handle   = 0;
static uint16_t s_batt_handle   = 0;
static uint16_t s_conn          = BLE_HS_CONN_HANDLE_NONE;
static bool     s_subscribed    = false;
static bool     s_batt_subscribed = false;
static uint8_t  s_batt_pct      = 0;

/* ---- GATT characteristic callbacks -------------------------------------- */

static int rsc_meas_access(uint16_t conn_handle,
                            uint16_t attr_handle,
                            struct ble_gatt_access_ctxt *ctxt,
                            void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)ctxt; (void)arg;
    return BLE_ATT_ERR_READ_NOT_PERMITTED;
}

static int rsc_feature_access(uint16_t conn_handle,
                               uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt,
                               void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;
    uint16_t feature = RSC_FEATURE_BITS;
    uint8_t buf[2] = { (uint8_t)(feature & 0xFF), (uint8_t)((feature >> 8) & 0xFF) };
    int rc = os_mbuf_append(ctxt->om, buf, sizeof buf);
    return (rc == 0) ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int batt_lvl_access(uint16_t conn_handle,
                           uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt,
                           void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;
    uint8_t v = s_batt_pct;
    int rc = os_mbuf_append(ctxt->om, &v, 1);
    return (rc == 0) ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

/* ---- GATT service table ------------------------------------------------- */

static const struct ble_gatt_svc_def s_rsc_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(RSC_SVC_UUID),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid       = BLE_UUID16_DECLARE(RSC_MEAS_UUID),
                .access_cb  = rsc_meas_access,
                .val_handle = &s_meas_handle,
                .flags      = BLE_GATT_CHR_F_NOTIFY,
            },
            {
                .uuid      = BLE_UUID16_DECLARE(RSC_FEATURE_UUID),
                .access_cb = rsc_feature_access,
                .flags     = BLE_GATT_CHR_F_READ,
            },
            { 0 },
        },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(BATT_SVC_UUID),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid       = BLE_UUID16_DECLARE(BATT_LVL_UUID),
                .access_cb  = batt_lvl_access,
                .val_handle = &s_batt_handle,
                .flags      = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            },
            { 0 },
        },
    },
    { 0 },
};

/* ---- GAP event handler (called by nus_ctrl's single GAP callback) ------- */

void garmin_rsc_on_gap_event(struct ble_gap_event *event)
{
    switch (event->type) {

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0 && s_conn == BLE_HS_CONN_HANDLE_NONE) {
            s_conn           = event->connect.conn_handle;
            s_subscribed     = false;
            s_batt_subscribed = false;
            ESP_LOGI(TAG, "RSC tracking conn=%d", s_conn);
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        if (event->disconnect.conn.conn_handle == s_conn) {
            s_conn           = BLE_HS_CONN_HANDLE_NONE;
            s_subscribed     = false;
            s_batt_subscribed = false;
        }
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_meas_handle) {
            s_subscribed = (event->subscribe.cur_notify != 0);
            ESP_LOGI(TAG, "RSC Measurement %s",
                     s_subscribed ? "subscribed" : "unsubscribed");
        } else if (event->subscribe.attr_handle == s_batt_handle) {
            s_batt_subscribed = (event->subscribe.cur_notify != 0);
            ESP_LOGI(TAG, "Battery Level %s",
                     s_batt_subscribed ? "subscribed" : "unsubscribed");
        }
        break;

    default:
        break;
    }
}

/* ---- Public API --------------------------------------------------------- */

void garmin_rsc_set_addr_type(uint8_t addr_type)
{
    s_own_addr_type = addr_type;
}

void garmin_rsc_register_gatt(void)
{
    int rc = ble_svc_gap_device_name_set("garmin-ftms-sync");
    if (rc != 0) ESP_LOGE(TAG, "device_name_set failed: %d", rc);

    rc = ble_gatts_count_cfg(s_rsc_svcs);
    if (rc != 0) { ESP_LOGE(TAG, "count_cfg failed: %d", rc); return; }
    rc = ble_gatts_add_svcs(s_rsc_svcs);
    if (rc != 0) { ESP_LOGE(TAG, "add_svcs failed: %d", rc); return; }
    ESP_LOGI(TAG, "RSC GATT service registered");
}

/* garmin_rsc_start() is now a no-op; advertising is owned by nus_ctrl. */
void garmin_rsc_start(void)
{
    ESP_LOGI(TAG, "RSC ready (meas_handle=%d, batt_handle=%d)",
             s_meas_handle, s_batt_handle);
}

void garmin_rsc_update(const treadmill_state_t *s)
{
    if (!s_subscribed || s_conn == BLE_HS_CONN_HANDLE_NONE) return;

    uint8_t buf[RSC_MEAS_BUF_LEN];
    size_t  len = rsc_encode_measurement(s, buf, sizeof buf);
    if (len == 0) { ESP_LOGW(TAG, "rsc_encode_measurement returned 0"); return; }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, (uint16_t)len);
    if (!om) { ESP_LOGE(TAG, "OOM"); return; }
    int rc = ble_gatts_notify_custom(s_conn, s_meas_handle, om);
    if (rc != 0) ESP_LOGE(TAG, "notify failed: %d", rc);
}

void garmin_rsc_update_battery(uint8_t pct)
{
    if (pct > 100) pct = 100;
    if (pct == s_batt_pct) return;
    s_batt_pct = pct;
    if (!s_batt_subscribed || s_conn == BLE_HS_CONN_HANDLE_NONE) return;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(&pct, 1);
    if (!om) { ESP_LOGE(TAG, "battery notify: OOM"); return; }
    int rc = ble_gatts_notify_custom(s_conn, s_batt_handle, om);
    if (rc != 0) ESP_LOGE(TAG, "battery notify failed: %d", rc);
}

bool garmin_rsc_subscribed(void) { return s_subscribed; }
bool garmin_rsc_advertising(void) { return false; } /* owned by nus_ctrl */
