import Toybox.BluetoothLowEnergy;
import Toybox.Lang;
import Toybox.System;

// BLE client for the nRF52840 bridge's control service ("TMILL-CTRL").
// Send-only: the field writes ctrl_dispatch command lines ("SPEED 8.0") to
// the control characteristic; treadmill speed reaches the watch natively
// over ANT+ (SDM footpod), so nothing is read back.
class CtrlBleDelegate extends BluetoothLowEnergy.BleDelegate {
    // Contract with boards/xiao-nrf52840/platform_ble_ctrl_svc.c
    const CTRL_SVC_UUID  = BluetoothLowEnergy.stringToUuid("A6ED0001-D344-460A-8075-B9E8EC90D71B");
    const CTRL_CHR_UUID  = BluetoothLowEnergy.stringToUuid("A6ED0002-D344-460A-8075-B9E8EC90D71B");

    hidden var mDevice as BluetoothLowEnergy.Device or Null;
    hidden var mProfileRegistered as Boolean;
    hidden var mScanning as Boolean;
    hidden var mWritePending as Boolean;

    function initialize() {
        BleDelegate.initialize();
        mDevice = null;
        mProfileRegistered = false;
        mScanning = false;
        mWritePending = false;
    }

    // Async; results arrive in onProfileRegister. Data fields get a single
    // BLE profile slot — this is it.
    function registerProfile() as Void {
        var profile = {
            :uuid => CTRL_SVC_UUID,
            :characteristics => [{
                :uuid => CTRL_CHR_UUID
            }]
        };
        try {
            BluetoothLowEnergy.registerProfile(profile);
        } catch (e) {
            System.println("BLE registerProfile failed: " + e.getErrorMessage());
        }
    }

    function onProfileRegister(uuid as BluetoothLowEnergy.Uuid,
                               status as BluetoothLowEnergy.Status) as Void {
        mProfileRegistered = (status == BluetoothLowEnergy.STATUS_SUCCESS);
        System.println("BLE profile register status=" + status);
        if (mProfileRegistered) {
            startScan();
        }
    }

    function startScan() as Void {
        if (mDevice != null || mScanning) { return; }
        try {
            BluetoothLowEnergy.setScanState(BluetoothLowEnergy.SCAN_STATE_SCANNING);
        } catch (e) {
            System.println("BLE scan start failed: " + e.getErrorMessage());
        }
    }

    function onScanStateChange(scanState as BluetoothLowEnergy.ScanState,
                               status as BluetoothLowEnergy.Status) as Void {
        mScanning = (scanState == BluetoothLowEnergy.SCAN_STATE_SCANNING);
    }

    hidden function advertisesCtrlSvc(r as BluetoothLowEnergy.ScanResult) as Boolean {
        var uuids = r.getServiceUuids();
        for (var u = uuids.next(); u != null; u = uuids.next()) {
            if (u.equals(CTRL_SVC_UUID)) {
                return true;
            }
        }
        return false;
    }

    function onScanResults(scanResults as BluetoothLowEnergy.Iterator) as Void {
        for (var r = scanResults.next(); r != null; r = scanResults.next()) {
            if (!(r instanceof BluetoothLowEnergy.ScanResult)) { continue; }
            if (advertisesCtrlSvc(r)) {
                System.println("BLE found TMILL-CTRL, pairing");
                BluetoothLowEnergy.setScanState(BluetoothLowEnergy.SCAN_STATE_OFF);
                try {
                    mDevice = BluetoothLowEnergy.pairDevice(r);
                } catch (e) {
                    System.println("BLE pair failed: " + e.getErrorMessage());
                    startScan();
                }
                return;
            }
        }
    }

    function onConnectedStateChanged(device as BluetoothLowEnergy.Device,
                                     state as BluetoothLowEnergy.ConnectionState) as Void {
        if (state == BluetoothLowEnergy.CONNECTION_STATE_CONNECTED) {
            mDevice = device;
            mWritePending = false;
            System.println("BLE bridge connected");
        } else {
            System.println("BLE bridge disconnected — rescanning");
            if (mDevice != null) {
                try {
                    BluetoothLowEnergy.unpairDevice(mDevice);
                } catch (e) {
                    // already gone; nothing to clean up
                }
            }
            mDevice = null;
            mWritePending = false;
            startScan();
        }
    }

    // Release BLE resources when the activity ends.
    function shutdown() as Void {
        try {
            BluetoothLowEnergy.setScanState(BluetoothLowEnergy.SCAN_STATE_OFF);
            if (mDevice != null) {
                BluetoothLowEnergy.unpairDevice(mDevice);
            }
        } catch (e) {
        }
        mDevice = null;
    }

    function isConnected() as Boolean {
        return mDevice != null;
    }

    function isScanning() as Boolean {
        return mScanning;
    }
}
