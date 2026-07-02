/*
 * main.c — Seeed XIAO nRF52840 treadmill↔Garmin ANT+ bridge.
 *
 * Skeleton stage (Task A0): SoftDevice up, RTT logging, 1 Hz LED heartbeat.
 * Later tasks wire in platform_ble_central / platform_ant_sdm /
 * platform_ble_ctrl_svc.
 */
#include <stdint.h>

#include "app_error.h"
#include "app_timer.h"
#include "boards.h"
#include "nrf_drv_clock.h"
#include "nrf_gpio.h"
#include "nrf_log.h"
#include "nrf_log_ctrl.h"
#include "nrf_log_default_backends.h"
#include "nrf_pwr_mgmt.h"
#include "nrf_sdh.h"

#define HEARTBEAT_MS 1000

APP_TIMER_DEF(m_heartbeat_timer);

static void heartbeat_cb(void *ctx)
{
    (void)ctx;
    nrf_gpio_pin_toggle(LED_2 /* green */);
}

static void log_init(void)
{
    APP_ERROR_CHECK(NRF_LOG_INIT(NULL));
    NRF_LOG_DEFAULT_BACKENDS_INIT();
}

static void softdevice_init(void)
{
    APP_ERROR_CHECK(nrf_sdh_enable_request());
    ASSERT(nrf_sdh_is_enabled());
}

static void timers_init(void)
{
    APP_ERROR_CHECK(nrf_drv_clock_init());
    nrf_drv_clock_lfclk_request(NULL);
    APP_ERROR_CHECK(app_timer_init());
    APP_ERROR_CHECK(app_timer_create(&m_heartbeat_timer, APP_TIMER_MODE_REPEATED,
                                     heartbeat_cb));
    APP_ERROR_CHECK(app_timer_start(m_heartbeat_timer,
                                    APP_TIMER_TICKS(HEARTBEAT_MS), NULL));
}

static void leds_init(void)
{
    nrf_gpio_cfg_output(LED_1);
    nrf_gpio_cfg_output(LED_2);
    nrf_gpio_cfg_output(LED_3);
    nrf_gpio_pin_set(LED_1);   /* active low: all off */
    nrf_gpio_pin_set(LED_2);
    nrf_gpio_pin_set(LED_3);
}

int main(void)
{
    log_init();
    leds_init();
    timers_init();
    softdevice_init();
    APP_ERROR_CHECK(nrf_pwr_mgmt_init());

    NRF_LOG_INFO("xiao-nrf52840 up, S340 present");

    for (;;) {
        if (!NRF_LOG_PROCESS()) {
            nrf_pwr_mgmt_run();
        }
    }
}
