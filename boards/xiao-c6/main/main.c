#include "esp_log.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "machine.h"
#include "garmin_rsc.h"
#include "ctrl_svc.h"
#include "serial_ctrl.h"

static const char *TAG = "app";

static treadmill_state_t s_last_state;

static void on_state(const treadmill_state_t *s)
{
    s_last_state = *s;
    garmin_rsc_update(s);
    serial_ctrl_push_state(s);
}

static void on_link(bool connected)
{
    (void)connected;
    ctrl_svc_notify_status();   /* push 'S' frame to the watch */
}

static void on_host_sync(void)
{
    ESP_LOGI(TAG, "nimble host synced");
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) { ESP_LOGE(TAG, "ensure_addr failed: %d", rc); return; }

    uint8_t own_addr_type;
    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) { ESP_LOGE(TAG, "id_infer_auto failed: %d", rc); return; }

    machine_set_addr_type(own_addr_type);
    garmin_rsc_set_addr_type(own_addr_type);
    ctrl_svc_set_addr_type(own_addr_type);
    machine_set_link_cb(on_link);
    machine_set_data_cb(on_state);
    serial_ctrl_start();
    garmin_rsc_start();
    ctrl_svc_start();
    machine_try_last();
}

static void nimble_host_task(void *param)
{
    ble_svc_gap_init();
    ble_svc_gatt_init();
    garmin_rsc_register_gatt();
    ctrl_svc_register_gatt();
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = on_host_sync;
    nimble_port_freertos_init(nimble_host_task);
    ESP_LOGI(TAG, "boot complete");
}
