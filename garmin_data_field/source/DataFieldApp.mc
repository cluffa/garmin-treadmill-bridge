import Toybox.Application;
import Toybox.Communications;
import Toybox.Lang;
import Toybox.WatchUi;
import Toybox.System;

class DataFieldApp extends Application.AppBase {
    var mView as DataFieldView or Null;

    function initialize() {
        AppBase.initialize();
    }

    function onStart(state as Dictionary?) as Void {
        Communications.registerForPhoneAppMessages(method(:onPhone));
    }

    function onStop(state as Dictionary?) as Void {
    }

    function getInitialView() {
        mView = new DataFieldView();
        return [ mView ];
    }

    function onPhone(msg as Communications.Message) as Void {
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
