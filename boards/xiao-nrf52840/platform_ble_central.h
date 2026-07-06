#pragma once
#include <stdbool.h>
#include "model.h"
#include "ftms_devlist.h"

/*
 * platform_ble_central — S340 GATT client for the treadmill link.
 *
 * Port of the NimBLE machine_ftms.c/machine_ifit.c transport layers: scans
 * for FTMS (0x1826) and iFit (0x1533) treadmills into a device list,
 * connects per connect_policy (saved last device wins on sight, else the
 * strongest RSSI after a pick window), subscribes to the data
 * characteristic, and pumps decoded frames (ftms_parse / ifit_parse) into
 * one shared state callback. The watch can override the choice at any time
 * via platform_ble_central_connect() (ctrl-svc "CONNECT <n>").
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

/* Register the state callback. Call after last_device_init(), before the
 * SoftDevice starts scanning. */
void platform_ble_central_init(central_state_cb cb);

/* (Re)start scanning and reset the device list. While disconnected the
 * connect_policy picks the target; while connected the scan only refreshes
 * the list (for the watch's picker) and never switches devices on its own. */
void platform_ble_central_start_scan(void);

bool            platform_ble_central_connected(void);
bool            platform_ble_central_connecting(void);
central_proto_t platform_ble_central_proto(void);

/* Snapshot of the scan list (insertion order — indices are stable for
 * CONNECT <n>). When connected, entry 0 is the live device (a connected
 * treadmill stops advertising, so it would otherwise vanish from the list).
 * Returns the count. */
int platform_ble_central_get_devices(ftms_device_t *out, int max);

/* Connect to a specific device (watch override). Tears down any existing
 * treadmill link first; the chosen device is persisted as the new
 * last-connected on success. */
void platform_ble_central_connect(const ftms_device_t *dev);

/* The connected device's identity, or NULL when disconnected. */
const ftms_device_t *platform_ble_central_device(void);

/* Treadmill control. FTMS: immediate Control Point write. iFit: queued and
 * injected at the correct keepalive poll phase (see ifit_poll.h). */
bool platform_ble_central_set_speed(float kmh);
bool platform_ble_central_set_incline(float pct);
bool platform_ble_central_stop(void);
