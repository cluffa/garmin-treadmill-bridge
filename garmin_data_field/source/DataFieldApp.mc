import Toybox.Application;
import Toybox.BluetoothLowEnergy;
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
}
