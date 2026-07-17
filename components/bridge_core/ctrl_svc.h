#pragma once
#include <stdint.h>

/*
 * ctrl_svc — BLE peripheral: the watch-facing control service.
 *
 * Same GATT contract as the nRF52840 bridge's platform_ble_ctrl_svc:
 * service A6ED0001-…, control char A6ED0002-… (write), response char
 * A6ED0003-… (notify, compact ctrl_frames). Owns the single advertisement
 * (RSC 0x1814 + name in the primary packet, A6ED UUID in the scan response
 * where CIQ scans can see it) so the watch can either pair the RSC sensor
 * natively or connect the CIQ data field / ctrl app — one link at a time,
 * chosen on the watch.
 */
void ctrl_svc_set_addr_type(uint8_t addr_type);
void ctrl_svc_register_gatt(void);   /* call before nimble_port_run() */
void ctrl_svc_start(void);           /* starts advertising */
void ctrl_svc_notify_status(void);   /* push 'S' frame if subscribed */
