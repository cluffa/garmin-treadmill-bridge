#pragma once
#include "model.h"
#include <stdbool.h>
#include <stdint.h>

/*
 * nus_ctrl — BLE peripheral: Nordic UART Service (NUS) for phone control.
 *
 * NUS UUIDs:
 *   Service  6E400001-B5A3-F393-E0A9-E50E24DCCA9E
 *   RX char  6E400002-…  (write / write-without-response, phone → ESP32)
 *   TX char  6E400003-…  (notify, ESP32 → phone)
 *
 * The phone writes ASCII command lines to RX; the ESP32 dispatches via
 * ctrl_dispatch() and sends JSON responses back over TX notifications.
 * State-change events are broadcast to all subscribed connections.
 */

/* Must be called from the host task before nimble_port_run(). */
void nus_ctrl_register_gatt(void);

/* Call from on_host_sync() after ble_hs_id_infer_auto(). */
void nus_ctrl_set_addr_type(uint8_t addr_type);

/* Starts NUS advertising (separate from RSC advertising). */
void nus_ctrl_start(void);

/* Push a treadmill state update to all subscribed NUS centrals. */
void nus_ctrl_push_state(const treadmill_state_t *s);

/* Push a connect/disconnect event to all subscribed NUS centrals. */
void nus_ctrl_push_event(int connected, const char *name, const char *proto);
