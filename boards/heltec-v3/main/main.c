#include "esp_log.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "ui.h"
#include "machine.h"
#include "garmin_rsc.h"
#include "nus_ctrl.h"
#include "display.h"
#include "serial_ctrl.h"

static const char *TAG = "app";

static void on_host_sync(void)
{
    ESP_LOGI(TAG, "nimble host synced");

    /* Determine the own address type once, following the blecent/bleprph
     * idiom.  ESP32 has no factory public address, so this will typically
     * resolve to BLE_OWN_ADDR_RANDOM. */
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_util_ensure_addr failed: %d", rc);
        return;
    }

    uint8_t own_addr_type;
    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto failed: %d", rc);
        return;
    }

    machine_set_addr_type(own_addr_type);
    garmin_rsc_set_addr_type(own_addr_type);
    nus_ctrl_set_addr_type(own_addr_type);

    ui_start();
}

static void nimble_host_task(void *param) {
    /* Must be called before nimble_port_run() so these services are
     * registered before the host stack starts advertising. */
    ble_svc_gap_init();
    ble_svc_gatt_init();
    /* Register custom GATT services here — before the host starts — so they
     * land in the attribute table. */
    garmin_rsc_register_gatt();
    nus_ctrl_register_gatt();
    display_init();
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = on_host_sync;
    nimble_port_freertos_init(nimble_host_task);
    ESP_LOGI(TAG, "boot complete");
}
