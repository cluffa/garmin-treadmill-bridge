/*
 * serial_ctrl.c — USB-serial command interface for the phone relay path.
 *
 * On chips with native USB Serial/JTAG (ESP32-C6, ESP32-S3 native USB):
 *   uses usb_serial_jtag_driver_install() + VFS so fgets/printf work.
 * On chips with external USB-UART (Heltec V3 / CP210x on S3):
 *   uses uart_driver_install(UART_NUM_0) + VFS.
 *
 * Both paths expose stdin/stdout through IDF VFS, so the read task uses
 * fgets() and the output helpers use printf() — no platform ifdefs in logic.
 */

#include "serial_ctrl.h"
#include "ctrl_dispatch.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "sdkconfig.h"

#ifdef CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#  include "driver/usb_serial_jtag.h"
#  include "driver/usb_serial_jtag_vfs.h"
#else
#  include "driver/uart.h"
#  include "esp_vfs_dev.h"
#endif

#include "esp_timer.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define TAG            "serial_ctrl"
#define BUF_SIZE       256
#define STATE_RATE_MS  1000

static SemaphoreHandle_t s_tx_mutex;
static int64_t           s_last_state_us;

static void serial_tx(const char *msg, void *ctx)
{
    (void)ctx;
    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    printf("%s\n", msg);
    fflush(stdout);
    xSemaphoreGive(s_tx_mutex);
}

static void serial_task(void *arg)
{
    (void)arg;
    char line[BUF_SIZE];
    while (1) {
        if (fgets(line, sizeof line, stdin) != NULL) {
            ctrl_dispatch(line, serial_tx, NULL);
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

void serial_ctrl_start(void)
{
    s_tx_mutex = xSemaphoreCreateMutex();

#ifdef CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    usb_serial_jtag_driver_install(&cfg);
    usb_serial_jtag_vfs_use_driver();
    ESP_LOGI(TAG, "serial_ctrl ready on USB Serial/JTAG");
#else
    uart_driver_install(UART_NUM_0, BUF_SIZE * 2, 0, 0, NULL, 0);
    esp_vfs_dev_uart_use_driver(0);
    ESP_LOGI(TAG, "serial_ctrl ready on UART0");
#endif

    xTaskCreate(serial_task, "serial_ctrl", 4096, NULL, 5, NULL);
}

void serial_ctrl_push_state(const treadmill_state_t *s)
{
    int64_t now = esp_timer_get_time();
    if (now - s_last_state_us < STATE_RATE_MS * 1000LL) return;
    s_last_state_us = now;
    char buf[256];
    snprintf(buf, sizeof buf,
             "{\"event\":\"state\",\"speed\":%.2f,\"distance\":%.1f"
             ",\"incline\":%.1f,\"elapsed\":%lu}",
             s->speed_mps * 3.6f, s->distance_m,
             s->incline_pct, (unsigned long)s->elapsed_s);
    serial_tx(buf, NULL);
}

void serial_ctrl_push_event(int connected, const char *name, const char *proto)
{
    char buf[128];
    if (connected) {
        snprintf(buf, sizeof buf,
                 "{\"event\":\"connected\",\"name\":\"%s\",\"proto\":\"%s\"}",
                 name ? name : "", proto ? proto : "");
    } else {
        snprintf(buf, sizeof buf, "{\"event\":\"disconnected\"}");
    }
    serial_tx(buf, NULL);
}
