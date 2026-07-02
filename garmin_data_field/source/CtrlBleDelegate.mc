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
    }

    function isConnected() as Boolean {
        return mDevice != null;
    }

    function isScanning() as Boolean {
        return mScanning;
    }
}
