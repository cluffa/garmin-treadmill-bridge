/*
 * machine_shim.c — machine.h facade over platform_ble_central, so the shared
 * ctrl_dispatch.c links unchanged on the nRF port.
 *
 * SCAN/LIST/CONNECT are fully functional: the central keeps a scan list and
 * the watch (or RTT) can pick a device with CONNECT <n>. The auto-connect
 * policy (last-connected, else closest) runs when nothing is picked.
 */
#include "machine.h"
#include "platform_ble_central.h"

void machine_set_addr_type(uint8_t addr_type) { (void)addr_type; }
void machine_set_data_cb(machine_state_cb cb) { (void)cb; /* wired in main */ }

void machine_start_scan(void)
{
    /* Also legal while connected: refreshes the list for the picker without
     * touching the live link. */
    platform_ble_central_start_scan();
}

int machine_get_devices(ftms_device_t *out, int max)
{
    return platform_ble_central_get_devices(out, max);
}

void machine_connect(const ftms_device_t *dev)
{
    platform_ble_central_connect(dev);
}

void machine_try_last(void)
{
    /* The scan's connect_policy prefers the saved device on sight. */
    machine_start_scan();
}

bool machine_connected(void) { return platform_ble_central_connected(); }

const ftms_device_t *machine_connected_device(void)
{
    return platform_ble_central_device();
}

bool machine_connecting(void) { return platform_ble_central_connecting(); }

int8_t machine_conn_rssi(void)
{
    const ftms_device_t *d = platform_ble_central_device();
    return d ? d->rssi : 0;
}

bool machine_set_speed(float kmh)  { return platform_ble_central_set_speed(kmh); }
bool machine_set_incline(float pct){ return platform_ble_central_set_incline(pct); }
bool machine_stop(void)            { return platform_ble_central_stop(); }
