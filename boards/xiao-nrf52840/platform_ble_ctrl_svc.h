#pragma once
#include <stdbool.h>

/*
 * platform_ble_ctrl_svc — BLE peripheral control service for the watch's
 * ConnectIQ data field (replaces the ESP32 build's phone-facing nus_ctrl).
 *
 * Vendor GATT service:
 *   Service  A6ED0001-D344-460A-8075-B9E8EC90D71B
 *   Control  A6ED0002-D344-460A-8075-B9E8EC90D71B  (write / write-no-rsp)
 *
 * The data field writes ASCII command lines in the existing ctrl_dispatch
 * grammar — e.g. "SPEED 10.0", "STOP" (uppercase, see ctrl_dispatch.c).
 * Responses are logged over RTT; no notify characteristic (send-only field).
 *
 * Advertises as "TMILL-CTRL" with the 128-bit service UUID; advertising
 * restarts automatically on disconnect.
 */

/* Register the service and start advertising. Call after the BLE stack is
 * enabled (and after platform_ble_central_init, which shares the GAP). */
void platform_ble_ctrl_svc_init(void);

/* True while a central (the data field) is connected. */
bool platform_ble_ctrl_svc_connected(void);
