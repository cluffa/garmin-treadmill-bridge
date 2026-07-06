#pragma once
#include <stdbool.h>

/*
 * platform_ble_ctrl_svc — BLE peripheral control service for the watch
 * (ConnectIQ data field and the treadmill-picker app).
 *
 * Vendor GATT service:
 *   Service   A6ED0001-D344-460A-8075-B9E8EC90D71B
 *   Control   A6ED0002-D344-460A-8075-B9E8EC90D71B  (write / write-no-rsp)
 *   Response  A6ED0003-D344-460A-8075-B9E8EC90D71B  (notify)
 *
 * The watch writes ASCII command lines in the existing ctrl_dispatch
 * grammar — "SPEED 10.0", "SCAN", "CONNECT 2", … (uppercase). LIST and
 * STATUS answer over the response characteristic in the compact ≤20-byte
 * 'D'/'E'/'S' frames from ctrl_frames.h (CIQ's MTU stays at 23); other
 * commands' JSON replies go to RTT only.
 *
 * Advertises as "TMILL-CTRL" with the 128-bit service UUID; advertising
 * restarts automatically on disconnect.
 */

/* Register the service and start advertising. Call after the BLE stack is
 * enabled (and after platform_ble_central_init, which shares the GAP). */
void platform_ble_ctrl_svc_init(void);

/* True while a central (the watch) is connected. */
bool platform_ble_ctrl_svc_connected(void);

/* Push an unsolicited 'S' status frame to a subscribed watch (call when the
 * treadmill link changes so the picker updates live). No-op otherwise. */
void platform_ble_ctrl_svc_notify_status(void);
