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
#include "nrf_sdh_ant.h"
#include "nrf_sdh_ble.h"

#include "platform_ant_sdm.h"
#include "platform_ble_central.h"
#include "platform_ble_ctrl_svc.h"

#define HEARTBEAT_MS 1000

APP_TIMER_DEF(m_heartbeat_timer);

static void heartbeat_cb(void *ctx)
{
    (void)ctx;
    nrf_gpio_pin_toggle(LED_2 /* green: alive */);
    /* Bench status at a glance (LEDs are active low):
     * red = treadmill linked, blue = data field connected. */
    nrf_gpio_pin_write(LED_1, platform_ble_central_connected() ? 0 : 1);
    nrf_gpio_pin_write(LED_3, platform_ble_ctrl_svc_connected() ? 0 : 1);
}

static void on_treadmill_state(const treadmill_state_t *s)
{
    platform_ant_sdm_set_state(s);
    NRF_LOG_DEBUG("tm_state speed=" NRF_LOG_FLOAT_MARKER
                  " incline=" NRF_LOG_FLOAT_MARKER,
                  NRF_LOG_FLOAT(s->speed_mps), NRF_LOG_FLOAT(s->incline_pct));
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

    /* BLE: apply the connection-count config and hand the SoftDevice its RAM
     * base. On a RAM mismatch the call logs the required app RAM start over
     * RTT — copy it into xiao_nrf52840_s340.ld and rebuild. */
    uint32_t ram_start = 0;
    APP_ERROR_CHECK(nrf_sdh_ble_default_cfg_set(1 /* conn_cfg_tag */,
                                                &ram_start));
    APP_ERROR_CHECK(nrf_sdh_ble_enable(&ram_start));
    APP_ERROR_CHECK(nrf_sdh_ant_enable());
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

    platform_ant_sdm_init();

    /* Forward path: treadmill frames → shared state → ANT+ SDM broadcast. */
    platform_ble_central_init(on_treadmill_state);
    platform_ble_central_start_scan();

    /* Reverse path: data field writes "SPEED <kmh>" → ctrl_dispatch →
     * machine_shim → treadmill (FTMS CP or iFit poll-phase injection). */
    platform_ble_ctrl_svc_init();

    for (;;) {
        if (!NRF_LOG_PROCESS()) {
            nrf_pwr_mgmt_run();
        }
    }
}
