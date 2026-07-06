#pragma once
/*
 * app_config.h — project overrides applied on top of the nRF5 SDK template
 * sdk_config.h (the Makefile defines USE_APP_CONFIG, which every SDK
 * sdk_config.h template honors by including this file first).
 *
 * BENCH SETUP (one-time): copy the template
 *   $(SDK_ROOT)/examples/multiprotocol/ble_ant_app_hrm/pca10056/s340/config/sdk_config.h
 * into this directory unchanged. Everything project-specific lives here so
 * the huge template stays pristine and diffable against the SDK.
 */

/* ---- SoftDevice handler: BLE central + peripheral + ANT ------------------ */
#define NRF_SDH_ENABLED 1
#define NRF_SDH_BLE_ENABLED 1
#define NRF_SDH_ANT_ENABLED 1
#define NRF_SDH_SOC_ENABLED 1

#define NRF_SDH_BLE_CENTRAL_LINK_COUNT 1     /* treadmill */
#define NRF_SDH_BLE_PERIPHERAL_LINK_COUNT 1  /* watch data field (ctrl svc) */
#define NRF_SDH_BLE_TOTAL_LINK_COUNT 2
#define NRF_SDH_BLE_VS_UUID_COUNT 4          /* iFit svc/chars + ctrl svc/char */
#define NRF_SDH_BLE_GATT_MAX_MTU_SIZE 23     /* both protocols use <=20 B frames */

#define NRF_SDH_ANT_ENQUEUED_EVENT_COUNT 8
#define NRF_SDH_ANT_TOTAL_CHANNELS_ALLOCATED 1   /* SDM master */
#define NRF_SDH_ANT_ENCRYPTED_CHANNELS 0

/* ---- clock: XIAO has a 32.768 kHz crystal -------------------------------- */
#define NRF_SDH_CLOCK_LF_SRC 1        /* XTAL */
#define NRF_SDH_CLOCK_LF_RC_CTIV 0
#define NRF_SDH_CLOCK_LF_RC_TEMP_CTIV 0
#define NRF_SDH_CLOCK_LF_ACCURACY 7   /* 20 ppm */

/* ---- radio timeslot budget -------------------------------------------------
 * Three concurrent radio roles share the schedule (S340 multiplexes by
 * timeslot, ANT has priority windows):
 *   ANT+ SDM master  : fixed 8134-count period (~4.03 Hz), ~150 µs bursts
 *   BLE central      : treadmill link, 30-50 ms conn interval (below) —
 *                      the iFit keepalive is one 20 B write-no-rsp per
 *                      500 ms tick (bursts of ≤7 at phases 2/5), far under
 *                      one packet per interval
 *   BLE peripheral   : TMILL-CTRL advertising at 285 ms / one low-traffic
 *                      connection (a 5 s-cadence 20 B write from the field)
 * Bench gate (Task A6): belt speed change lands within one keepalive cycle
 * while ANT keeps broadcasting and the field stays connected. If the iFit
 * link starves, first lengthen the ctrl-svc conn interval, then shorten the
 * treadmill conn interval toward 30 ms. */

/* ---- BLE modules ---------------------------------------------------------- */
#define NRF_BLE_GATT_ENABLED 1
#define NRF_BLE_SCAN_ENABLED 1
/* The ble_ant_app_hrm template sdk_config.h has no nrf_ble_scan section, so
 * the module's whole config block lives here, not just overrides. */
#define NRF_BLE_SCAN_BUFFER 31
#define NRF_BLE_SCAN_SCAN_PHY 1              /* 1 Mbps */
#define NRF_BLE_SCAN_OBSERVER_PRIO 1
#define NRF_BLE_SCAN_NAME_MAX_LEN 32
#define NRF_BLE_SCAN_SHORT_NAME_MAX_LEN 32
#define NRF_BLE_SCAN_SCAN_INTERVAL 160        /* 100 ms */
#define NRF_BLE_SCAN_SCAN_WINDOW 80           /* 50 ms — leave air time for ANT */
#define NRF_BLE_SCAN_SCAN_DURATION 0          /* forever */
#define NRF_BLE_SCAN_SUPERVISION_TIMEOUT 400  /* 4 s */
#define NRF_BLE_SCAN_MIN_CONNECTION_INTERVAL 24  /* 30 ms — iFit keepalive fits */
#define NRF_BLE_SCAN_MAX_CONNECTION_INTERVAL 40  /* 50 ms */
#define NRF_BLE_SCAN_SLAVE_LATENCY 0
#define NRF_BLE_SCAN_FILTER_ENABLE 1
#define NRF_BLE_SCAN_UUID_CNT 2               /* FTMS 0x1826 + iFit 0x1533 */
#define NRF_BLE_SCAN_NAME_CNT 0
#define NRF_BLE_SCAN_SHORT_NAME_CNT 0
#define NRF_BLE_SCAN_ADDRESS_CNT 0
#define NRF_BLE_SCAN_APPEARANCE_CNT 0

#define BLE_DB_DISCOVERY_ENABLED 1
#define NRF_BLE_GQ_ENABLED 1
#define NRF_BLE_GQ_QUEUE_SIZE 8
#define NRF_BLE_GQ_GATTC_WRITE_MAX_DATA_LEN 20
#define NRF_BLE_GQ_GATTS_HVX_MAX_DATA_LEN 20

#define BLE_ADVERTISING_ENABLED 1
#define NRF_BLE_CONN_PARAMS_ENABLED 1
#define NRF_BLE_CONN_PARAMS_MAX_SLAVE_LATENCY_DEVIATION 499
#define NRF_BLE_CONN_PARAMS_MAX_SUPERVISION_TIMEOUT_DEVIATION 65535

/* ---- clock driver (app_timer LFCLK request; SDH takes over once SD is up) - */
#define NRFX_CLOCK_ENABLED 1
#define NRF_CLOCK_ENABLED 1                  /* legacy nrf_drv_clock alias */
#define NRFX_CLOCK_CONFIG_LF_SRC 1           /* XTAL, matches NRF_SDH_CLOCK_LF_SRC */
#define CLOCK_CONFIG_LF_SRC 1
#define NRFX_CLOCK_CONFIG_IRQ_PRIORITY 6
#define NRFX_CLOCK_CONFIG_LF_CAL_ENABLED 0
#define CLOCK_CONFIG_SOC_OBSERVER_PRIO 0
#define CLOCK_CONFIG_STATE_OBSERVER_PRIO 0
#define CLOCK_CONFIG_LOG_ENABLED 0

/* ---- fds: last-connected treadmill persistence ----------------------------- */
#define FDS_ENABLED 1
#define FDS_VIRTUAL_PAGES 2
#define FDS_VIRTUAL_PAGE_SIZE 1024
#define FDS_BACKGROUND_GC 1
#define FDS_OP_QUEUE_SIZE 4
#define FDS_CRC_CHECK_ON_READ 0
#define FDS_CRC_CHECK_ON_WRITE 0
#define FDS_MAX_USERS 2
#define NRF_FSTORAGE_ENABLED 1
#define NRF_FSTORAGE_SD_QUEUE_SIZE 4
#define NRF_FSTORAGE_SD_MAX_RETRIES 8
#define NRF_FSTORAGE_SD_MAX_WRITE_SIZE 4096

/* ---- libraries ------------------------------------------------------------ */
#define APP_TIMER_ENABLED 1
#define APP_TIMER_CONFIG_RTC_FREQUENCY 0     /* 32768 Hz */
#define APP_TIMER_CONFIG_OP_QUEUE_SIZE 16
#define NRF_PWR_MGMT_ENABLED 1
#define NRF_QUEUE_ENABLED 1
#define NRF_SECTION_ITER_ENABLED 1
#define NRF_SORTLIST_ENABLED 1               /* app_timer2 */

/* ---- logging over SEGGER RTT ---------------------------------------------- */
#define NRF_LOG_ENABLED 1
#define NRF_LOG_DEFAULT_LEVEL 3              /* info */
#define NRF_LOG_DEFERRED 1
#define NRF_LOG_BACKEND_RTT_ENABLED 1
#define NRF_LOG_BACKEND_UART_ENABLED 0
#define NRF_LOG_STR_PUSH_BUFFER_SIZE 128
#define NRF_FPRINTF_ENABLED 1
#define NRF_FPRINTF_FLAG_AUTOMATIC_CR_ON_LF_ENABLED 1
