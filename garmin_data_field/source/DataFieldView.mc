import Toybox.Activity;
import Toybox.Graphics;
import Toybox.Lang;
import Toybox.WatchUi;
import Toybox.Communications;
import Toybox.System;

class DataFieldView extends WatchUi.DataField {
    hidden var mConnectionStatus as String;
    hidden var mLastSendTime as Number;
    hidden var mSpeed as Float;

    function initialize() {
        DataField.initialize();
        mConnectionStatus = "Disconnected";
        mLastSendTime = 0;
        mSpeed = 0.0f;
    }

    function setSpeed(speed as Float) as Void {
        mSpeed = speed;
        WatchUi.requestUpdate();
    }

    function onLayout(dc as Dc) as Void {
    }

    function compute(info as Activity.Info) as Void {
        var settings = System.getDeviceSettings();
        var status = false;
        if (settings has :phoneConnected) {
            status = settings.phoneConnected;
        }

        if (status) {
            mConnectionStatus = "Connected";
        } else {
            mConnectionStatus = "Disconnected";
        }

        var now = System.getTimer();
        // Send current workout status (target pace) every 5 seconds
        if (status && (now - mLastSendTime > 5000)) {
            mLastSendTime = now;
            var msg = new Dictionary<String, Any>();
            msg.put("type", "workoutStatus");
            msg.put("targetPace", 0.0); // Simple version
            if (info != null && info.currentSpeed != null) {
                msg.put("currentSpeed", info.currentSpeed as Float);
            }
            try {
                Communications.transmit(msg, null, new CommListener());
            } catch (e) {
                System.println("Transmit error: " + e.getErrorMessage());
            }
        }
    }

    function onUpdate(dc as Dc) as Void {
        var fgColor = Graphics.COLOR_BLACK;
        var bgColor = Graphics.COLOR_WHITE;
        
        if (getBackgroundColor() == Graphics.COLOR_BLACK) {
            fgColor = Graphics.COLOR_WHITE;
            bgColor = Graphics.COLOR_BLACK;
        }

        dc.setColor(fgColor, bgColor);
        dc.clear();

        // Calculate pace
        var paceString = "--:-- /mi";
        if (mSpeed > 0.1) {
            var paceMins = 96.5604 / mSpeed;
            var mins = paceMins.toNumber();
            var secs = ((paceMins - mins) * 60).toNumber();
            paceString = mins.toString() + ":" + secs.format("%02d") + " /mi";
        }

        dc.drawText(
            dc.getWidth() / 2,
            dc.getHeight() / 2 - 10,
            Graphics.FONT_LARGE,
            paceString,
            Graphics.TEXT_JUSTIFY_CENTER | Graphics.TEXT_JUSTIFY_VCENTER
        );

        dc.drawText(
            dc.getWidth() / 2,
            dc.getHeight() / 2 + 20,
            Graphics.FONT_XTINY,
            mConnectionStatus,
            Graphics.TEXT_JUSTIFY_CENTER | Graphics.TEXT_JUSTIFY_VCENTER
        );
    }
}

class CommListener extends Communications.ConnectionListener {
    function initialize() {
        Communications.ConnectionListener.initialize();
    }
    
    function onComplete() as Void {
        System.println("Transmit Complete");
    }
    
    function onError() as Void {
        System.println("Transmit Error");
    }
}
