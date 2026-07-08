#pragma once
#include "model.h"
#include "host/ble_gap.h"
#include <stdbool.h>
#include <stdint.h>

// Set the own address type inferred by ble_hs_id_infer_auto() in on_host_sync.
void garmin_rsc_set_addr_type(uint8_t addr_type);

// Register the RSC GATT service + device name. MUST be called from the host
// task before nimble_port_run().
void garmin_rsc_register_gatt(void);

// No-op: advertising is now owned by ctrl_svc (single advertiser).
void garmin_rsc_start(void);

void garmin_rsc_update(const treadmill_state_t *s);

// Push battery state-of-charge (0..100 %) to the standard Battery Service.
void garmin_rsc_update_battery(uint8_t pct);

// Forward GAP events from the shared (ctrl_svc) GAP callback.
// garmin_rsc handles CONNECT, DISCONNECT, and SUBSCRIBE for its own handles.
void garmin_rsc_on_gap_event(struct ble_gap_event *event);

bool garmin_rsc_subscribed(void);
bool garmin_rsc_advertising(void); /* always false; advertising owned by ctrl_svc */
