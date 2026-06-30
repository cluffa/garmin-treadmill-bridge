/*
 * nus_ctrl.c — BLE peripheral: Nordic UART Service (NUS) for phone control.
 *
 * The phone connects as a BLE central, writes command lines to the RX
 * characteristic, and receives JSON responses + state events via TX notify.
 * Up to NUS_MAX_CONNS phones can connect simultaneously; all get the same
 * broadcast state events (ponytail: 2 is plenty for this use case).
 */

#include "nus_ctrl.h"
#include "ctrl_dispatch.h"

#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "services/gap/ble_svc_gap.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <string.h>
#include <stdint.h>

#define TAG          "nus_ctrl"
#define NUS_MAX_CONNS 2
#define NUS_LINE_BUF  256   /* max command line length */
#define NUS_MTU_MIN   20    /* ATT payload when no MTU exchange yet */

/* ---- NUS 128-bit UUIDs (little-endian byte order for NimBLE) ------------ */
/* String: 6E400001-B5A3-F393-E0A9-E50E24DCCA9E */
static const ble_uuid128_t NUS_SVC_UUID = BLE_UUID128_INIT(
    0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,
    0x93,0xf3,0xa3,0xb5,0x01,0x00,0x40,0x6e);
/* RX: …0002… */
static const ble_uuid128_t NUS_RX_UUID = BLE_UUID128_INIT(
    0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,
    0x93,0xf3,0xa3,0xb5,0x02,0x00,0x40,0x6e);
/* TX: …0003… */
static const ble_uuid128_t NUS_TX_UUID = BLE_UUID128_INIT(
    0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,
    0x93,0xf3,0xa3,0xb5,0x03,0x00,0x40,0x6e);

/* ---- per-connection state ------------------------------------------------ */

typedef struct {
    uint16_t conn;
    uint16_t mtu;          /* effective ATT payload = negotiated MTU - 3 */
    bool     subscribed;
    char     rxbuf[NUS_LINE_BUF];
    uint16_t rxlen;
} nus_conn_t;

static nus_conn_t    s_conns[NUS_MAX_CONNS];
static uint16_t      s_tx_handle;
static uint8_t       s_own_addr_type;
static SemaphoreHandle_t s_tx_mutex;

/* ---- helpers ------------------------------------------------------------- */

static nus_conn_t *find_conn(uint16_t conn_handle)
{
    for (int i = 0; i < NUS_MAX_CONNS; i++)
        if (s_conns[i].conn == conn_handle) return &s_conns[i];
    return NULL;
}

static nus_conn_t *alloc_conn(uint16_t conn_handle)
{
    /* prefer a free slot */
    for (int i = 0; i < NUS_MAX_CONNS; i++) {
        if (s_conns[i].conn == BLE_HS_CONN_HANDLE_NONE) {
            s_conns[i].conn       = conn_handle;
            s_conns[i].mtu        = NUS_MTU_MIN;
            s_conns[i].subscribed = false;
            s_conns[i].rxlen      = 0;
            return &s_conns[i];
        }
    }
    return NULL;
}

static void free_conn(uint16_t conn_handle)
{
    nus_conn_t *c = find_conn(conn_handle);
    if (c) { memset(c, 0, sizeof *c); c->conn = BLE_HS_CONN_HANDLE_NONE; }
}

/* Send a string to one connection, chunking if needed for the MTU. */
static void tx_conn(nus_conn_t *c, const char *msg)
{
    if (!c->subscribed) return;
    size_t remaining = strlen(msg);
    size_t offset    = 0;
    /* include the newline terminator in the last chunk */
    while (remaining > 0 || offset == 0) {
        size_t chunk = remaining < c->mtu ? remaining : c->mtu;
        bool   last  = (chunk == remaining);
        /* Build the chunk: payload + '\n' on the last piece */
        size_t nbytes = last ? chunk + 1 : chunk;
        struct os_mbuf *om = ble_hs_mbuf_from_flat(msg + offset, chunk);
        if (!om) { ESP_LOGE(TAG, "tx OOM"); return; }
        if (last) {
            (void)nbytes;  /* informational only */
            uint8_t nl = '\n';
            if (os_mbuf_append(om, &nl, 1) != 0) {
                os_mbuf_free_chain(om);
                ESP_LOGE(TAG, "tx append nl OOM");
                return;
            }
        }
        int rc = ble_gatts_notify_custom(c->conn, s_tx_handle, om);
        if (rc != 0) {
            ESP_LOGW(TAG, "notify failed conn=%d rc=%d", c->conn, rc);
            return;
        }
        offset    += chunk;
        remaining -= chunk;
        if (last) break;
    }
}

/* Broadcast a string to all subscribed connections. */
static void tx_all(const char *msg)
{
    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    for (int i = 0; i < NUS_MAX_CONNS; i++) {
        if (s_conns[i].conn != BLE_HS_CONN_HANDLE_NONE)
            tx_conn(&s_conns[i], msg);
    }
    xSemaphoreGive(s_tx_mutex);
}

/* ctrl_dispatch tx callback: reply to the specific conn that sent the command */
static void nus_reply(const char *msg, void *ctx)
{
    nus_conn_t *c = (nus_conn_t *)ctx;
    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    tx_conn(c, msg);
    xSemaphoreGive(s_tx_mutex);
}

/* ---- GATT callbacks ------------------------------------------------------ */

static int nus_rx_access(uint16_t conn_handle,
                         uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt,
                         void *arg)
{
    (void)attr_handle; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;

    nus_conn_t *c = find_conn(conn_handle);
    if (!c) return BLE_ATT_ERR_UNLIKELY;

    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len == 0) return 0;

    /* append bytes to the per-connection line buffer */
    uint8_t tmp[NUS_LINE_BUF];
    if (len > sizeof tmp) len = sizeof tmp;
    os_mbuf_copydata(ctxt->om, 0, len, tmp);

    for (uint16_t i = 0; i < len; i++) {
        uint8_t b = tmp[i];
        if (b == '\n' || b == '\r') {
            if (c->rxlen > 0) {
                c->rxbuf[c->rxlen] = '\0';
                ctrl_dispatch(c->rxbuf, nus_reply, c);
                c->rxlen = 0;
            }
        } else if (c->rxlen < NUS_LINE_BUF - 1) {
            c->rxbuf[c->rxlen++] = (char)b;
        }
    }
    return 0;
}

static int nus_tx_access(uint16_t conn_handle,
                         uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt,
                         void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)ctxt; (void)arg;
    return BLE_ATT_ERR_READ_NOT_PERMITTED;
}

/* ---- GATT service table -------------------------------------------------- */

static const struct ble_gatt_svc_def s_nus_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &NUS_SVC_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                /* RX: phone writes commands here */
                .uuid      = &NUS_RX_UUID.u,
                .access_cb = nus_rx_access,
                .flags     = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                /* TX: ESP32 notifies responses and events */
                .uuid       = &NUS_TX_UUID.u,
                .access_cb  = nus_tx_access,
                .val_handle = &s_tx_handle,
                .flags      = BLE_GATT_CHR_F_NOTIFY,
            },
            { 0 },
        },
    },
    { 0 },
};

/* ---- forward declaration ------------------------------------------------- */
static int nus_gap_event_cb(struct ble_gap_event *event, void *arg);

/* ---- advertising --------------------------------------------------------- */

static void start_advertising(void)
{
    struct ble_gap_adv_params params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        .itvl_min  = BLE_GAP_ADV_ITVL_MS(200),
        .itvl_max  = BLE_GAP_ADV_ITVL_MS(400),
    };

    struct ble_hs_adv_fields fields = { 0 };
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    /* Advertise NUS service UUID so the phone's BLE scan can filter on it */
    fields.uuids128             = &NUS_SVC_UUID;
    fields.num_uuids128         = 1;
    fields.uuids128_is_complete = 1;

    struct ble_hs_adv_fields rsp = { 0 };
    const char *name = ble_svc_gap_device_name();
    rsp.name             = (const uint8_t *)name;
    rsp.name_len         = (uint8_t)strlen(name);
    rsp.name_is_complete = 1;

    if (ble_gap_adv_set_fields(&fields) != 0 ||
        ble_gap_adv_rsp_set_fields(&rsp) != 0) {
        ESP_LOGE(TAG, "adv set_fields failed");
        return;
    }
    int rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                               &params, nus_gap_event_cb, NULL);
    if (rc != 0)
        ESP_LOGE(TAG, "ble_gap_adv_start failed: %d", rc);
    else
        ESP_LOGI(TAG, "NUS advertising started");
}

/* ---- GAP event handler --------------------------------------------------- */

static int nus_gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            ESP_LOGE(TAG, "connect failed, status=%d", event->connect.status);
            start_advertising();
            break;
        }
        if (alloc_conn(event->connect.conn_handle)) {
            ESP_LOGI(TAG, "NUS central connected, conn=%d",
                     event->connect.conn_handle);
        } else {
            ESP_LOGW(TAG, "NUS conn table full, rejecting conn=%d",
                     event->connect.conn_handle);
            ble_gap_terminate(event->connect.conn_handle, BLE_ERR_CONN_LIMIT);
        }
        start_advertising();  /* keep advertising for more connections */
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "NUS central disconnected, conn=%d reason=%d",
                 event->disconnect.conn.conn_handle, event->disconnect.reason);
        free_conn(event->disconnect.conn.conn_handle);
        start_advertising();
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        {
            nus_conn_t *c = find_conn(event->subscribe.conn_handle);
            if (c && event->subscribe.attr_handle == s_tx_handle) {
                c->subscribed = (event->subscribe.cur_notify != 0);
                ESP_LOGI(TAG, "NUS TX %s, conn=%d",
                         c->subscribed ? "subscribed" : "unsubscribed",
                         event->subscribe.conn_handle);
            }
        }
        break;

    case BLE_GAP_EVENT_MTU:
        {
            nus_conn_t *c = find_conn(event->mtu.conn_handle);
            if (c) {
                /* effective payload = negotiated MTU - 3 ATT header bytes */
                c->mtu = event->mtu.value > 3 ? (uint16_t)(event->mtu.value - 3)
                                               : NUS_MTU_MIN;
                ESP_LOGI(TAG, "NUS MTU=%d (payload=%d), conn=%d",
                         event->mtu.value, c->mtu, event->mtu.conn_handle);
            }
        }
        break;

    default:
        break;
    }
    return 0;
}

/* ---- public API ---------------------------------------------------------- */

void nus_ctrl_set_addr_type(uint8_t addr_type)
{
    s_own_addr_type = addr_type;
}

void nus_ctrl_register_gatt(void)
{
    s_tx_mutex = xSemaphoreCreateMutex();

    /* Initialise conn table */
    for (int i = 0; i < NUS_MAX_CONNS; i++)
        s_conns[i].conn = BLE_HS_CONN_HANDLE_NONE;

    int rc = ble_gatts_count_cfg(s_nus_svcs);
    if (rc != 0) { ESP_LOGE(TAG, "count_cfg failed: %d", rc); return; }
    rc = ble_gatts_add_svcs(s_nus_svcs);
    if (rc != 0) { ESP_LOGE(TAG, "add_svcs failed: %d", rc); return; }
    ESP_LOGI(TAG, "NUS GATT service registered");
}

void nus_ctrl_start(void)
{
    ESP_LOGI(TAG, "NUS ready (tx_handle=%d)", s_tx_handle);
    start_advertising();
}

void nus_ctrl_push_state(const treadmill_state_t *s)
{
    char buf[256];
    snprintf(buf, sizeof buf,
             "{\"event\":\"state\",\"speed\":%.2f,\"distance\":%.1f"
             ",\"incline\":%.1f,\"elapsed\":%lu}",
             s->speed_mps * 3.6f, s->distance_m,
             s->incline_pct, (unsigned long)s->elapsed_s);
    tx_all(buf);
}

void nus_ctrl_push_event(int connected, const char *name, const char *proto)
{
    char buf[128];
    if (connected) {
        snprintf(buf, sizeof buf,
                 "{\"event\":\"connected\",\"name\":\"%s\",\"proto\":\"%s\"}",
                 name ? name : "", proto ? proto : "");
    } else {
        snprintf(buf, sizeof buf, "{\"event\":\"disconnected\"}");
    }
    tx_all(buf);
}
