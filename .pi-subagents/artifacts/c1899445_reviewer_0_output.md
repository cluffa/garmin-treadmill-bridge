All header includes and API signatures check out. Here is the complete review:

---

## Review

### Correct

- **Advertising restructure (ctrl_svc.c, lines 241–252):** Moving the RSC 16-bit UUID (0x1814) from the scan response to the primary advertising packet is the right fix for Garmin watch passive scanning. The combined primary payload (flags 3B + 16-bit UUID 4B + 128-bit UUID 18B = 25B) fits within the 31-byte advertising limit. NimBLE's `ble_hs_adv_fields` explicitly supports simultaneous `uuids16` and `uuids128` (separate AD types 0x03 and 0x07). The scan response, now only carrying the 16-character device name `"garmin-ftms-sync"` (~18 bytes), also fits comfortably.

- **Connection parameter update (ctrl_svc.c, lines 296–310):** All `ble_gap_upd_params` fields are correctly initialized with appropriate BLE units (`BLE_GAP_CONN_ITVL_MS` converts to 1.25ms units; `BLE_GAP_SUPERVISION_TIMEOUT_MS` converts to 10ms units). The values (40–60ms interval, 0 latency, 4s timeout) are reasonable for a sensor peripheral sharing the radio with a central role. Failure is handled gracefully via `ESP_LOGW`. Calling `ble_gap_update_params` from the `BLE_GAP_EVENT_CONNECT` handler is a standard NimBLE pattern — the connection handle is already valid.

- **CONN_UPDATE handler (ctrl_svc.c, lines 334–345):** Correctly checks `event->conn_update.status == 0` (int, 0 = success per the NimBLE struct definition). Log output conversions are accurate: `conn_itvl * 1.25f` (1.25ms units → ms), `conn_latency` (connection events), `supervision_timeout * 10` (10ms units → ms).

- **Battery init change (garmin_rsc.c, line 40):** Changing `s_batt_pct` from `0` to `100` is correct for a USB-powered bridge — avoids reporting a misleading dead battery to the paired watch.

### Note

- **`ble_gap_conn_find` uses `s_conn` rather than `event->conn_update.conn_handle`:** The module-level `s_conn` should always match `event->conn_update.conn_handle` in normal operation, so this is harmless and consistent with the rest of the file. However, using the event's own `conn_handle` would be marginally more robust if NimBLE ever recycled a now-disconnected handle before the CONN_UPDATE event fires. Given NimBLE's sequential host-task dispatch, this cannot happen in practice. Existing code (e.g., `BLE_GAP_EVENT_NOTIFY_TX` at line 351) follows the same `s_conn` pattern.

- **No build artifacts or tests were run** — this is a code-level review against the NimBLE SDK headers only. Build verification against the ESP32-C6 target would be appropriate before merging.

### Residual risks

- **None identified.** The advertising change should strictly improve Garmin discoverability without degrading anything (RSC UUID is now in the primary packet, which is a superset of the scan-response delivery). The connection parameter update is a polite request — Garmin watches typically honor sensor-requested intervals in this range.