/*
 * ctrl_svc.c — BLE peripheral: watch-facing control service (NimBLE).
 *
 * Port of boards/xiao-nrf52840/platform_ble_ctrl_svc.c. The Garmin CIQ data
 * field / ctrl app writes uppercase ctrl_dispatch lines ("SPEED 8.0", "SCAN",
 * "CONNECT 2", …) to the control characteristic. LIST and STATUS answer on
 * the response characteristic in the compact ≤20-byte 'D'/'E'/'S' frames from
 * ctrl_frames.h (CIQ pins the ATT MTU at 23); every other command's JSON
 * reply goes to the console log only.
 *
 * Also owns the single advertisement (previously nus_ctrl's): primary packet
 * carries the A6ED 128-bit service UUID (the watch apps filter on it), scan
 * response carries RSC 0x1814 + device name for native sensor pairing. The
 * watch holds at most one link, so RSC mode and control mode are naturally
 * exclusive — whichever connects first wins until it disconnects.
 */

#include "ctrl_svc.h"
#include "ctrl_dispatch.h"
#include "ctrl_frames.h"
#include "garmin_rsc.h"
#include "machine.h"

#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "services/gap/ble_svc_gap.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <string.h>
#include <stdint.h>

#define TAG           "ctrl_svc"
#define CTRL_LINE_MAX 64    /* longest ctrl_dispatch line we accept */

/* ---- A6ED 128-bit UUIDs (little-endian byte order for NimBLE) ----------- */
/* String: A6ED0001-D344-460A-8075-B9E8EC90D71B; chars are …0002/…0003. */
static const ble_uuid128_t CTRL_SVC_UUID = BLE_UUID128_INIT(
    0x1b,0xd7,0x90,0xec,0xe8,0xb9,0x75,0x80,
    0x0a,0x46,0x44,0xd3,0x01,0x00,0xed,0xa6);
static const ble_uuid128_t CTRL_CHR_UUID = BLE_UUID128_INIT(
    0x1b,0xd7,0x90,0xec,0xe8,0xb9,0x75,0x80,
    0x0a,0x46,0x44,0xd3,0x02,0x00,0xed,0xa6);
static const ble_uuid128_t CTRL_RSP_UUID = BLE_UUID128_INIT(
    0x1b,0xd7,0x90,0xec,0xe8,0xb9,0x75,0x80,
    0x0a,0x46,0x44,0xd3,0x03,0x00,0xed,0xa6);

/* ---- state --------------------------------------------------------------- */

static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static bool     s_notify_on;
static uint16_t s_rsp_handle;
static uint8_t  s_own_addr_type;
static SemaphoreHandle_t s_tx_mutex;

/* ---- notification TX queue -----------------------------------------------
 * The host stack buffers only a few notifications at a time; a LIST reply is
 * up to FTMS_MAX_DEVICES+1 frames, so ring-buffer them and drain on
 * BLE_GAP_EVENT_NOTIFY_TX. */
#define TXQ_LEN 12
static struct { uint8_t len; uint8_t data[CTRL_FRAME_MAX]; } s_txq[TXQ_LEN];
static volatile uint8_t s_txq_head, s_txq_tail;

/* caller holds s_tx_mutex */
static void txq_pump(void)
{
    while (s_txq_tail != s_txq_head) {
        if (s_conn == BLE_HS_CONN_HANDLE_NONE || !s_notify_on) {
            s_txq_tail = s_txq_head;   /* drop — nobody is listening */
            return;
        }
        struct os_mbuf *om = ble_hs_mbuf_from_flat(s_txq[s_txq_tail].data,
                                                   s_txq[s_txq_tail].len);
        if (!om) return;               /* OOM — resume on NOTIFY_TX */
        int rc = ble_gatts_notify_custom(s_conn, s_rsp_handle, om);
        if (rc == BLE_HS_ENOMEM || rc == BLE_HS_EBUSY)
            return;                    /* resume on NOTIFY_TX */
        if (rc != 0)
            ESP_LOGW(TAG, "notify rc=%d — dropping frame", rc);
        s_txq_tail = (uint8_t)((s_txq_tail + 1) % TXQ_LEN);
    }
}

/* caller holds s_tx_mutex */
static void txq_push(const uint8_t *data, uint8_t len)
{
    uint8_t next = (uint8_t)((s_txq_head + 1) % TXQ_LEN);
    if (next == s_txq_tail) {
        ESP_LOGW(TAG, "tx queue full — frame dropped");
        return;
    }
    s_txq[s_txq_head].len = len;
    memcpy(s_txq[s_txq_head].data, data, len);
    s_txq_head = next;
    txq_pump();
}

/* ---- watch-facing frames -------------------------------------------------- */

/* caller holds s_tx_mutex */
static void send_status_frame(void)
{
    const ftms_device_t *dev = machine_connected_device();
    uint8_t buf[CTRL_FRAME_MAX];
    int n = ctrl_frame_status(buf, dev != NULL,
                              dev ? dev->proto : 0,
                              dev ? dev->name : NULL);
    txq_push(buf, (uint8_t)n);
}

/* caller holds s_tx_mutex */
static void send_list_frames(void)
{
    ftms_device_t devs[FTMS_MAX_DEVICES];
    int n = machine_get_devices(devs, FTMS_MAX_DEVICES);

    const ftms_device_t *live = machine_connected_device();
    ftms_device_t saved;
    bool have_saved = machine_saved_device(&saved);

    for (int i = 0; i < n; i++) {
        uint8_t flags = 0;
        if (live && memcmp(devs[i].addr, live->addr, 6) == 0)
            flags |= CTRL_DEV_FLAG_CONNECTED;
        if (have_saved && memcmp(devs[i].addr, saved.addr, 6) == 0)
            flags |= CTRL_DEV_FLAG_SAVED;
        uint8_t buf[CTRL_FRAME_MAX];
        int len = ctrl_frame_device(buf, (uint8_t)i, &devs[i], flags);
        txq_push(buf, (uint8_t)len);
    }
    uint8_t buf[CTRL_FRAME_MAX];
    int len = ctrl_frame_list_end(buf, (uint8_t)n);
    txq_push(buf, (uint8_t)len);
}

/* ctrl_dispatch responses go to the console — the watch gets the compact
 * 'D'/'E'/'S' frames instead (JSON exceeds CIQ's 20-byte notifications). */
static void ctrl_log_tx(const char *msg, void *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "ctrl: %s", msg);
}

/* ---- GATT callbacks -------------------------------------------------------- */

static int ctrl_chr_access(uint16_t conn_handle,
                           uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt,
                           void *arg)
{
    (void)attr_handle; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;
    if (conn_handle != s_conn) return BLE_ATT_ERR_UNLIKELY;

    char line[CTRL_LINE_MAX + 1];
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len > CTRL_LINE_MAX) len = CTRL_LINE_MAX;
    os_mbuf_copydata(ctxt->om, 0, len, line);
    line[len] = '\0';
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
        line[--len] = '\0';
    if (len == 0) return 0;
    ESP_LOGI(TAG, "rx \"%s\"", line);

    /* LIST and STATUS answer the watch in compact frames; everything else
     * (SCAN/CONNECT/SPEED/INCLINE/STOP) runs through the shared grammar. */
    if (strcmp(line, "LIST") == 0) {
        xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
        send_list_frames();
        xSemaphoreGive(s_tx_mutex);
        return 0;
    }
    if (strcmp(line, "STATUS") == 0) {
        xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
        send_status_frame();
        xSemaphoreGive(s_tx_mutex);
        return 0;
    }
    ctrl_dispatch(line, ctrl_log_tx, NULL);
    return 0;
}

static int ctrl_rsp_access(uint16_t conn_handle,
                           uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt,
                           void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)ctxt; (void)arg;
    return BLE_ATT_ERR_READ_NOT_PERMITTED;
}

/* ---- GATT service table ----------------------------------------------------- */

static const struct ble_gatt_svc_def s_ctrl_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &CTRL_SVC_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                /* control: watch writes command lines here */
                .uuid      = &CTRL_CHR_UUID.u,
                .access_cb = ctrl_chr_access,
                .flags     = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                /* response: compact 'D'/'E'/'S' frames, notify-only */
                .uuid       = &CTRL_RSP_UUID.u,
                .access_cb  = ctrl_rsp_access,
                .val_handle = &s_rsp_handle,
                .flags      = BLE_GATT_CHR_F_NOTIFY,
            },
            { 0 },
        },
    },
    { 0 },
};

/* ---- forward declaration ---------------------------------------------------- */
static int ctrl_gap_event_cb(struct ble_gap_event *event, void *arg);

/* ---- advertising ------------------------------------------------------------ */

/*
 * Single advertisement owned by ctrl_svc. Primary packet carries the A6ED
 * 128-bit UUID (the CIQ data field / ctrl app filter on it). Scan response
 * carries the RSC 16-bit UUID (0x1814) and device name so a Garmin watch's
 * active scan finds the RSC sensor without a separate advertisement.
 */
static void start_advertising(void)
{
    struct ble_gap_adv_params params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        .itvl_min  = BLE_GAP_ADV_ITVL_MS(100),
        .itvl_max  = BLE_GAP_ADV_ITVL_MS(200),
    };

    /* Primary: flags + ctrl 128-bit UUID */
    struct ble_hs_adv_fields fields = { 0 };
    fields.flags                = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128             = &CTRL_SVC_UUID;
    fields.num_uuids128         = 1;
    fields.uuids128_is_complete = 1;

    /* Scan response: RSC 16-bit UUID + device name */
    static ble_uuid16_t rsc_uuid = BLE_UUID16_INIT(0x1814);
    struct ble_hs_adv_fields rsp = { 0 };
    rsp.uuids16             = &rsc_uuid;
    rsp.num_uuids16         = 1;
    rsp.uuids16_is_complete = 1;
    const char *name        = ble_svc_gap_device_name();
    rsp.name                = (const uint8_t *)name;
    rsp.name_len            = (uint8_t)strlen(name);
    rsp.name_is_complete    = 1;

    if (ble_gap_adv_set_fields(&fields) != 0 ||
        ble_gap_adv_rsp_set_fields(&rsp) != 0) {
        ESP_LOGE(TAG, "adv set_fields failed");
        return;
    }
    int rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                               &params, ctrl_gap_event_cb, NULL);
    if (rc != 0)
        ESP_LOGE(TAG, "ble_gap_adv_start failed: %d", rc);
    else
        ESP_LOGI(TAG, "CTRL+RSC advertising started");
}

/* ---- GAP event handler -------------------------------------------------------- */

static int ctrl_gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    /* Let garmin_rsc track subscriptions on its own handles. */
    garmin_rsc_on_gap_event(event);

    switch (event->type) {

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            ESP_LOGE(TAG, "connect failed, status=%d", event->connect.status);
            start_advertising();
            break;
        }
        /* One watch link at a time (RSC or ctrl) — no re-advertise while
         * connected; the watch can't hold a second link anyway. */
        s_conn = event->connect.conn_handle;
        xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
        s_notify_on = false;
        s_txq_head = s_txq_tail = 0;
        xSemaphoreGive(s_tx_mutex);
        ESP_LOGI(TAG, "watch connected, conn=%d", s_conn);
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "watch disconnected, reason=%d — readvertising",
                 event->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_notify_on = false;
        start_advertising();
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_rsp_handle &&
            event->subscribe.conn_handle == s_conn) {
            xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
            s_notify_on = (event->subscribe.cur_notify != 0);
            if (s_notify_on) send_status_frame();   /* greet with link state */
            xSemaphoreGive(s_tx_mutex);
            ESP_LOGI(TAG, "notifications %s", s_notify_on ? "on" : "off");
        }
        break;

    case BLE_GAP_EVENT_NOTIFY_TX:
        if (event->notify_tx.conn_handle == s_conn) {
            xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
            txq_pump();
            xSemaphoreGive(s_tx_mutex);
        }
        break;

    default:
        break;
    }
    return 0;
}

/* ---- public API ----------------------------------------------------------------- */

void ctrl_svc_set_addr_type(uint8_t addr_type)
{
    s_own_addr_type = addr_type;
}

void ctrl_svc_register_gatt(void)
{
    s_tx_mutex = xSemaphoreCreateMutex();

    int rc = ble_gatts_count_cfg(s_ctrl_svcs);
    if (rc != 0) { ESP_LOGE(TAG, "count_cfg failed: %d", rc); return; }
    rc = ble_gatts_add_svcs(s_ctrl_svcs);
    if (rc != 0) { ESP_LOGE(TAG, "add_svcs failed: %d", rc); return; }
    ESP_LOGI(TAG, "CTRL GATT service registered");
}

void ctrl_svc_start(void)
{
    ESP_LOGI(TAG, "ctrl ready (rsp_handle=%d)", s_rsp_handle);
    start_advertising();
}

void ctrl_svc_notify_status(void)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) return;
    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    if (s_notify_on) send_status_frame();
    xSemaphoreGive(s_tx_mutex);
}
