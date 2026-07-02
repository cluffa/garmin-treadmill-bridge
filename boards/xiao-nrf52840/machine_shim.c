/*
 * machine_shim.c — machine.h facade over platform_ble_central, so the shared
 * ctrl_dispatch.c links unchanged on the nRF port.
 *
 * The nRF build auto-connects to the first treadmill match instead of
 * keeping a scan list (no display/menu on this board), so the list/connect
 * commands degrade gracefully: LIST reports an empty list, CONNECT <n> is a
 * no-op that returns "bad index".
 */
#include "machine.h"
#include "platform_ble_central.h"

#include <string.h>

void machine_set_addr_type(uint8_t addr_type) { (void)addr_type; }
void machine_set_data_cb(machine_state_cb cb) { (void)cb; /* wired in main */ }

void machine_start_scan(void)
{
    if (!platform_ble_central_connected()) {
        platform_ble_central_start_scan();
    }
}

int machine_get_devices(ftms_device_t *out, int max)
{
    (void)out; (void)max;
    return 0;   /* auto-connect design: no browsable list */
}

void machine_connect(const ftms_device_t *dev) { (void)dev; }
void machine_try_last(void) { machine_start_scan(); }

bool machine_connected(void) { return platform_ble_central_connected(); }

const ftms_device_t *machine_connected_device(void)
{
    static ftms_device_t dev;
    if (!platform_ble_central_connected()) return NULL;
    memset(&dev, 0, sizeof dev);
    dev.proto = (platform_ble_central_proto() == CENTRAL_PROTO_IFIT)
                    ? MACHINE_PROTO_IFIT : MACHINE_PROTO_FTMS;
    strncpy(dev.name,
            dev.proto == MACHINE_PROTO_IFIT ? "iFit" : "FTMS",
            FTMS_NAME_LEN - 1);
    return &dev;
}

bool machine_connecting(void) { return false; }
int8_t machine_conn_rssi(void) { return 0; }

bool machine_set_speed(float kmh)  { return platform_ble_central_set_speed(kmh); }
bool machine_set_incline(float pct){ return platform_ble_central_set_incline(pct); }
bool machine_stop(void)            { return platform_ble_central_stop(); }
