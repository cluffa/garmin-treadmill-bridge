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

/* ---- BLE modules ---------------------------------------------------------- */
#define NRF_BLE_GATT_ENABLED 1
#define NRF_BLE_SCAN_ENABLED 1
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
