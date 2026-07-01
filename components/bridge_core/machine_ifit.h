#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "machine_adapter.h"
#include "ftms_devlist.h"

/* iFit / NordicTrack / ProForm BLE treadmill adapter. Same shape as
 * machine_ftms so a selector can choose between them. Unlike FTMS, iFit frames
 * carry no distance, so this adapter integrates distance from speed over time.
 *
 * NOTE: written from the qdomyos-zwift reverse-engineering (proformtreadmill)
 * and HARDWARE-VERIFIED on a live iFit treadmill (decode + T-series poll + full
 * chain to RSC). Wired into the app via the machine.c facade, which auto-detects
 * FTMS vs iFit in one unified scan and dispatches connect to the right adapter. */
void   machine_ifit_set_addr_type(uint8_t addr_type);
void   machine_ifit_set_data_cb(machine_state_cb cb);
void   machine_ifit_set_event_cb(machine_event_cb cb);
bool   machine_ifit_is_ifit_adv(const uint8_t *data, uint8_t len);
void   machine_ifit_start_scan(void);
int    machine_ifit_get_devices(ftms_device_t *out, int max);
void   machine_ifit_connect(const ftms_device_t *dev);
void   machine_ifit_disconnect(void);
bool   machine_ifit_connected(void);
bool   machine_ifit_connecting(void);
int8_t machine_ifit_conn_rssi(void);
bool   machine_ifit_set_speed(float kmh);
bool   machine_ifit_set_incline(float pct);
bool   machine_ifit_stop(void);
