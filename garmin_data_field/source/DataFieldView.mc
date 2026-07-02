import Toybox.Activity;
import Toybox.Graphics;
import Toybox.Lang;
import Toybox.WatchUi;
import Toybox.System;

// Send-only data field: reads the structured workout's target pace and
// writes it to the nRF52840 bridge over BLE ("SPEED <kmh>"). Actual
// treadmill speed reaches the watch natively via the bridge's ANT+ footpod,
// so this field displays only the target and the control-link state.
class DataFieldView extends WatchUi.DataField {
    hidden var mBle as CtrlBleDelegate or Null;
    hidden var mLastSendTime as Number;
    hidden var mTargetPaceStr as String; // formatted target from workout step
    hidden var mLastSentStr as String;   // last command sent to the bridge

    function initialize(ble as CtrlBleDelegate or Null) {
        DataField.initialize();
        mBle = ble;
        mLastSendTime = 0;
        mTargetPaceStr = "--:-- tgt";
        mLastSentStr = "";
    }

    function onLayout(dc as Dc) as Void {
    }

    hidden function _paceStrFromMs(speedMs as Float) as String {
        var paceMins = 26.8224f / speedMs; // 1609.34/60 = 26.8224 min/mile per m/s
        var mins = paceMins.toNumber();
        var secs = ((paceMins - mins) * 60).toNumber();
        return mins.toString() + ":" + secs.format("%02d");
    }

    // Target pace (m/s) from the current workout step, or 0 when there is no
    // speed-targeted step. Activity.getCurrentWorkoutStep() is the
    // module-level function — distinct from (and more reliable in a
    // DataField's compute() context than) Activity.Info.currentWorkoutStep,
    // which came back null even mid-workout.
    hidden function targetPaceMps() as Float {
        var wStep = Activity.getCurrentWorkoutStep();
        if (wStep == null) { return 0.0f; }
        if ((wStep has :step) && wStep.step != null) {
            wStep = wStep.step;   // intervals nest the active step
        }
        var low = 0.0f;
        var high = 0.0f;
        if ((wStep has :targetType) &&
            wStep.targetType == Activity.WORKOUT_STEP_TARGET_SPEED) {
            if ((wStep has :targetValueLow) && wStep.targetValueLow != null) {
                low = wStep.targetValueLow as Float;
            }
            if ((wStep has :targetValueHigh) && wStep.targetValueHigh != null) {
                high = wStep.targetValueHigh as Float;
            }
        }
        if (low > 0.0f && high > 0.0f) { return (low + high) / 2.0f; }
        if (low > 0.0f) { return low; }
        return high;
    }

    function compute(info as Activity.Info) as Void {
        var targetMps = targetPaceMps();
        mTargetPaceStr = targetMps > 0.05f
            ? (_paceStrFromMs(targetMps) + " tgt")
            : "--:-- tgt";

        // Push the target to the bridge every 5 s while one exists. No
        // currentSpeed fallback (unlike the old phone path): echoing measured
        // speed back as a belt command would chase the footpod reading.
        var now = System.getTimer();
        if (targetMps > 0.05f && mBle != null && mBle.isConnected()
            && (now - mLastSendTime > 5000)) {
            var kmh = targetMps * 3.6f;
            if (mBle.writeSpeedKmh(kmh)) {
                mLastSendTime = now;
                mLastSentStr = "sent " + kmh.format("%.1f") + " km/h";
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

        var w = dc.getWidth();
        var h = dc.getHeight();

        // Workout target pace (top)
        dc.drawText(w / 2, h / 3, Graphics.FONT_MEDIUM, mTargetPaceStr,
            Graphics.TEXT_JUSTIFY_CENTER | Graphics.TEXT_JUSTIFY_VCENTER);

        // Last command sent (middle)
        dc.drawText(w / 2, h * 2 / 3, Graphics.FONT_XTINY, mLastSentStr,
            Graphics.TEXT_JUSTIFY_CENTER | Graphics.TEXT_JUSTIFY_VCENTER);
    }
}
