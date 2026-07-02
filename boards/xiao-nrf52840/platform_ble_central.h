#pragma once
#include <stdbool.h>
#include "model.h"

/*
 * platform_ble_central — S340 GATT client for the treadmill link.
 *
 * Port of the NimBLE machine_ftms.c/machine_ifit.c transport layers: scans
 * for FTMS (0x1826) and iFit (0x1533) treadmills, connects to the first
 * match, subscribes to the data characteristic, and pumps decoded frames
 * (ftms_parse / ifit_parse) into one shared state callback.
 *
 * Architecture invariant carried over from the ESP32 build: ONE machine
 * connection at a time. A single connection slot serves both protocols, so
 * the two notification streams can never clobber each other in the shared
 * state.
 */

typedef void (*central_state_cb)(const treadmill_state_t *s);

typedef enum {
    CENTRAL_PROTO_NONE = 0,
    CENTRAL_PROTO_FTMS,
    CENTRAL_PROTO_IFIT,
} central_proto_t;

/* Register the state callback. Call before the SoftDevice starts scanning. */
void platform_ble_central_init(central_state_cb cb);

/* Scan for treadmills; auto-connects to the first FTMS or iFit match. */
void platform_ble_central_start_scan(void);

bool            platform_ble_central_connected(void);
central_proto_t platform_ble_central_proto(void);

/* Treadmill control. FTMS: immediate Control Point write. iFit: queued and
 * injected at the correct keepalive poll phase (see ifit_poll.h). */
bool platform_ble_central_set_speed(float kmh);
bool platform_ble_central_set_incline(float pct);
bool platform_ble_central_stop(void);
