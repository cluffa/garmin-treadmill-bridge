import Toybox.Application;
import Toybox.BluetoothLowEnergy;
import Toybox.Communications;
import Toybox.Lang;
import Toybox.WatchUi;
import Toybox.System;

class DataFieldApp extends Application.AppBase {
    var mView as DataFieldView or Null;
    var mBle as CtrlBleDelegate or Null;

    function initialize() {
        AppBase.initialize();
    }

    function onStart(state as Dictionary?) as Void {
        // Phone path — removed in Task B3 (BLE control replaces it).
        Communications.registerForPhoneAppMessages(method(:onPhone));

        mBle = new CtrlBleDelegate();
        BluetoothLowEnergy.setDelegate(mBle);
        mBle.registerProfile();   // scan starts from onProfileRegister
    }

    function onStop(state as Dictionary?) as Void {
        if (mBle != null) {
            mBle.shutdown();
        }
    }

    function getInitialView() {
        mView = new DataFieldView(mBle);
        return [ mView ];
    }

    function onPhone(msg as Communications.PhoneAppMessage) as Void {
        System.println("Received msg from phone: " + msg.data.toString());
        var data = msg.data;
        if (data instanceof Dictionary) {
            if (data.hasKey("speed")) {
                var speed = data.get("speed");
                if (speed instanceof Float || speed instanceof Double || speed instanceof Number) {
                    if (mView != null) {
                        mView.setSpeed(speed.toFloat());
                    }
                }
            }
        }
    }
}
