# Task for reviewer

Do a short focused code review of this diff for the branch fix/c6-rsc-discover-and-conn-stability against main. Focus on correctness, edge cases, and potential issues.

Changes in two files:

1. components/bridge_core/ctrl_svc.c:
- Moved RSC 0x1814 UUID from scan response to primary advertising packet
- After BLE connect, requests connection params (40-60ms interval, 0 latency, 4000ms timeout) via ble_gap_update_params
- Added BLE_GAP_EVENT_CONN_UPDATE handler to log negotiated params using ble_gap_conn_find

2. components/bridge_core/garmin_rsc.c:
- Changed s_batt_pct init from 0 to 100

Full diff:
```diff
diff --git a/components/bridge_core/ctrl_svc.c b/components/bridge_core/ctrl_svc.c
--- a/components/bridge_core/ctrl_svc.c
+++ b/components/bridge_core/ctrl_svc.c
@@ -224,10 +224,10 @@ static int ctrl_gap_event_cb(struct ble_gap_event *event, void *arg);
 /* ---- advertising ------------------------------------------------------------ */
 
 /*
- * Single advertisement owned by ctrl_svc. Primary packet carries the A6ED
- * 128-bit UUID (the CIQ data field / ctrl app filter on it). Scan response
- * carries the RSC 16-bit UUID (0x1814) and device name so a Garmin watch's
- * active scan finds the RSC sensor without a separate advertisement.
+ * Single advertisement owned by ctrl_svc. Primary packet carries both the
+ * RSC 16-bit UUID (0x1814 — so Garmin watches find it as a native sensor
+ * even without an active scan) and the A6ED 128-bit UUID (CIQ data field /
+ * ctrl app filter on it). Scan response carries the device name only.
  */
 static void start_advertising(void)
 {
@@ -238,19 +238,19 @@ static void start_advertising(void)
         .itvl_max  = BLE_GAP_ADV_ITVL_MS(200),
     };
 
-    /* Primary: flags + ctrl 128-bit UUID */
+    /* Primary: flags + RSC 16-bit UUID + ctrl 128-bit UUID (25 bytes total) */
+    static ble_uuid16_t rsc_uuid = BLE_UUID16_INIT(0x1814);
     struct ble_hs_adv_fields fields = { 0 };
     fields.flags                = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
+    fields.uuids16              = &rsc_uuid;
+    fields.num_uuids16          = 1;
+    fields.uuids16_is_complete  = 1;
     fields.uuids128             = &CTRL_SVC_UUID;
     fields.num_uuids128         = 1;
     fields.uuids128_is_complete = 1;
 
-    /* Scan response: RSC 16-bit UUID + device name */
-    static ble_uuid16_t rsc_uuid = BLE_UUID16_INIT(0x1814);
+    /* Scan response: device name only */
     struct ble_hs_adv_fields rsp = { 0 };
-    rsp.uuids16             = &rsc_uuid;
-    rsp.num_uuids16         = 1;
-    rsp.uuids16_is_complete = 1;
     const char *name        = ble_svc_gap_device_name();
     rsp.name                = (const uint8_t *)name;
     rsp.name_len            = (uint8_t)strlen(name);
@@ -293,6 +293,23 @@ static int ctrl_gap_event_cb(struct ble_gap_event *event, void *arg)
         s_txq_head = s_txq_tail = 0;
         xSemaphoreGive(s_tx_mutex);
         ESP_LOGI(TAG, "watch connected, conn=%d", s_conn);
+
+        /* Request reasonable connection parameters for a sensor peripheral
+         * that shares the radio with a BLE central (treadmill) role.
+         * Interval 40-60ms, no latency, 4s supervision timeout. */
+        {
+            struct ble_gap_upd_params upd = {
+                .itvl_min             = BLE_GAP_CONN_ITVL_MS(40),
+                .itvl_max             = BLE_GAP_CONN_ITVL_MS(60),
+                .latency              = 0,
+                .supervision_timeout  = BLE_GAP_SUPERVISION_TIMEOUT_MS(4000),
+                .min_ce_len           = 0,
+                .max_ce_len           = 0,
+            };
+            int rc = ble_gap_update_params(s_conn, &upd);
+            if (rc != 0)
+                ESP_LOGW(TAG, "update_params request failed: %d", rc);
+        }
         break;
 
     case BLE_GAP_EVENT_DISCONNECT:
@@ -314,6 +331,19 @@ static int ctrl_gap_event_cb(struct ble_gap_event *event, void *arg)
         }
         break;
 
+    case BLE_GAP_EVENT_CONN_UPDATE:
+        if (event->conn_update.status == 0) {
+            struct ble_gap_conn_desc desc;
+            int rc = ble_gap_conn_find(s_conn, &desc);
+            if (rc == 0)
+                ESP_LOGI(TAG, "conn params updated: itvl=%dms latency=%d timeout=%dms",
+                         (int)(desc.conn_itvl * 1.25f),
+                         desc.conn_latency,
+                         desc.supervision_timeout * 10);
+        } else
+            ESP_LOGW(TAG, "conn update failed, status=%d", event->conn_update.status);
+        break;
+
     case BLE_GAP_EVENT_NOTIFY_TX:
         if (event->notify_tx.conn_handle == s_conn) {
             xSemaphoreTake(s_tx_mutex, portMAX_DELAY);

diff --git a/components/bridge_core/garmin_rsc.c b/components/bridge_core/garmin_rsc.c
--- a/components/bridge_core/garmin_rsc.c
+++ b/components/bridge_core/garmin_rsc.c
@@ -37,7 +37,7 @@
 static uint16_t s_conn          = BLE_HS_CONN_HANDLE_NONE;
 static bool     s_subscribed    = false;
 static bool     s_batt_subscribed = false;
-static uint8_t  s_batt_pct      = 0;
+static uint8_t  s_batt_pct      = 100; /* USB-powered — report full charge */
```

## Acceptance Contract
Acceptance level: checked
Completion is not accepted from prose alone. End with a structured acceptance report.

Criteria:
- criterion-1: Implement the requested change without widening scope

Required evidence: changed-files, tests-added, commands-run, residual-risks, no-staged-files

Finish with a fenced JSON block tagged `acceptance-report` in this shape:
Use empty arrays when no items apply; array fields contain strings unless object entries are shown.
```acceptance-report
{
  "criteriaSatisfied": [
    {
      "id": "criterion-1",
      "status": "satisfied",
      "evidence": "specific proof"
    }
  ],
  "changedFiles": [
    "src/file.ts"
  ],
  "testsAddedOrUpdated": [
    "test/file.test.ts"
  ],
  "commandsRun": [
    {
      "command": "command",
      "result": "passed",
      "summary": "short result"
    }
  ],
  "validationOutput": [
    "validation output or concise summary"
  ],
  "residualRisks": [
    "none"
  ],
  "noStagedFiles": true,
  "diffSummary": "short description of the diff",
  "reviewFindings": [
    "blocker: file.ts:12 - issue found, or no blockers"
  ],
  "manualNotes": "anything else the parent should know"
}
```