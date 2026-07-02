#pragma once
#include "model.h"

/*
 * platform_ant_sdm — ANT+ Stride-Based Speed & Distance Monitor (footpod)
 * master channel. Replaces the ESP32 build's BLE RSC peripheral
 * (garmin_rsc.c): the watch pairs this as a native ANT+ foot pod, so no BLE
 * sensor slot is consumed and the data field's CIQ BLE connection can't
 * collide with it.
 *
 * Broadcast content comes from the latched treadmill state; pages are built
 * by the shared, host-tested ant_sdm_encode.
 */

/* Assign the ANT+ network key and open the SDM master channel.
 * Call once after the SoftDevice (BLE + ANT) is enabled. */
void platform_ant_sdm_init(void);

/* Latch the state broadcast on subsequent ANT TX slots. Callable from any
 * SoftDevice event priority. */
void platform_ant_sdm_set_state(const treadmill_state_t *s);
