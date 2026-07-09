import Toybox.Activity;
import Toybox.Graphics;
import Toybox.Lang;
import Toybox.WatchUi;
import Toybox.System;

// Send-only data field. It no longer decides belt speed: it packs the *raw*
// current workout step (target/duration/intensity) plus the activity timer
// state into a small binary frame and writes it to the bridge's telemetry
// characteristic. The bridge (components/bridge_core/workout_ctrl.c) resolves
// the target, dedups, keeps a slow keepalive, and drives the treadmill.
//
// Frame layout must match workout_ctrl.h (little-endian, 15 bytes):
//   [0]      version (1)
//   [1]      timerState
//   [2]      flags (bit0 = a step is present)
//   [3]      intensity
//   [4]      targetType (0 = speed)
//   [5..6]   targetLow  (mm/s for a speed target)
//   [7..8]   targetHigh (mm/s for a speed target)
//   [9]      durationType
//   [10..13] durationValue
//   [14]     repetitionNumber
class DataFieldView extends WatchUi.DataField {
    hidden const FRAME_VERSION = 1;
    hidden const FRAME_LEN = 15;

    hidden var mBle as CtrlBleDelegate or Null;
    hidden var mLastFrame as ByteArray or Null; // last frame actually sent
    hidden var mTargetKmh as Float;             // resolved speed target, for display
    hidden var mHasSpeedTarget as Boolean;
    hidden var mTimerState as Number;

    function initialize(ble as CtrlBleDelegate or Null) {
        DataField.initialize();
        mBle = ble;
        mLastFrame = null;
        mTargetKmh = 0.0f;
        mHasSpeedTarget = false;
        mTimerState = Activity.TIMER_STATE_OFF;
    }

    // Let the App's onStart() patch us up when it runs after getInitialView().
    function setBle(ble as CtrlBleDelegate or Null) as Void {
        mBle = ble;
    }

    function onLayout(dc as Dc) as Void {
    }

    // --- frame encoding helpers ---

    hidden function _u16(f as ByteArray, off as Number, v as Number) as Void {
        f[off]     = v & 0xFF;
        f[off + 1] = (v >> 8) & 0xFF;
    }

    hidden function _u32(f as ByteArray, off as Number, v as Number) as Void {
        f[off]     = v & 0xFF;
        f[off + 1] = (v >> 8) & 0xFF;
        f[off + 2] = (v >> 16) & 0xFF;
        f[off + 3] = (v >> 24) & 0xFF;
    }

    // Encode a target value for the wire. Speed targets are m/s → mm/s; other
    // target types are packed raw (informational — the bridge only acts on
    // speed). Clamped to uint16.
    hidden function _targetToWire(targetType as Number, val) as Number {
        if (val == null) { return 0; }
        var n;
        if (targetType == Activity.WORKOUT_STEP_TARGET_SPEED) {
            n = (val.toFloat() * 1000.0f).toNumber();
        } else {
            n = val.toNumber();
        }
        if (n < 0) { n = 0; }
        if (n > 65535) { n = 65535; }
        return n;
    }

    // Build the 15-byte telemetry frame for the given timer state.  Wrapped
    // so that an error reading the workout step never crashes the data field.
    hidden function _packFrame(timerState as Number) as ByteArray {
        try {
            var f = new [FRAME_LEN]b;
            for (var i = 0; i < FRAME_LEN; i++) { f[i] = 0; }
            f[0] = FRAME_VERSION;
            f[1] = timerState & 0xFF;
            f[3] = 0xFF; // intensity: invalid until known
            f[4] = 0xFF; // targetType: invalid until known
            f[9] = 0xFF; // durationType: invalid until known

            mHasSpeedTarget = false;
            mTargetKmh = 0.0f;

            var info = Activity.getCurrentWorkoutStep();
            if (info == null) { return f; }
            if ((info has :intensity) && info.intensity != null) {
                f[3] = info.intensity & 0xFF;
            }

            var step = (info has :step) ? info.step : null;
            if (step == null) { return f; }

            // Resolve interval work/rest to the current portion; a plain step is
            // itself the portion.
            var portion = step;
            if (step instanceof Activity.WorkoutIntervalStep) {
                var isRest = (info has :intensity) &&
                             info.intensity == Activity.WORKOUT_INTENSITY_REST;
                portion = isRest ? step.restStep : step.activeStep;
                if ((step has :repetitionNumber) && step.repetitionNumber != null) {
                    f[14] = step.repetitionNumber & 0xFF;
                }
            }
            if (portion == null) { return f; }

            f[2] = 0x01; // has step
            var tt = ((portion has :targetType) && portion.targetType != null)
                ? portion.targetType : 0xFF;
            f[4] = tt & 0xFF;
            var low  = _targetToWire(tt, (portion has :targetValueLow)  ? portion.targetValueLow  : null);
            var high = _targetToWire(tt, (portion has :targetValueHigh) ? portion.targetValueHigh : null);
            _u16(f, 5, low);
            _u16(f, 7, high);
            if ((portion has :durationType) && portion.durationType != null) {
                f[9] = portion.durationType & 0xFF;
            }
            var dv = ((portion has :durationValue) && portion.durationValue != null)
                ? portion.durationValue : 0;
            _u32(f, 10, dv);

            // Stash the resolved speed for the display (mm/s midpoint → km/h).
            if (tt == Activity.WORKOUT_STEP_TARGET_SPEED) {
                var mmps = (low > 0 && high > 0) ? ((low + high) / 2) : (low > 0 ? low : high);
                if (mmps > 0) {
                    mHasSpeedTarget = true;
                    mTargetKmh = mmps * 0.0036f;
                }
            }
            return f;
        } catch (e) {
            System.println("DataFieldView _packFrame error: " + e.getErrorMessage());
            mHasSpeedTarget = false;
            mTargetKmh = 0.0f;
        }
        // Fallback: minimal valid frame with no-step sentinel values.
        var fb = new [FRAME_LEN]b;
        for (var j = 0; j < FRAME_LEN; j++) { fb[j] = 0; }
        fb[0] = FRAME_VERSION;
        fb[1] = timerState & 0xFF;
        fb[3] = 0xFF;
        fb[4] = 0xFF;
        fb[9] = 0xFF;
        return fb;
    }

    hidden function _bytesEqual(a as ByteArray, b as ByteArray or Null) as Boolean {
        if (b == null || a.size() != b.size()) { return false; }
        for (var i = 0; i < a.size(); i++) {
            if (a[i] != b[i]) { return false; }
        }
        return true;
    }

    // Send the frame if it changed (or force it, for a timer transition). Only
    // latches it as sent when the write was actually issued.
    hidden function _maybeSend(frame as ByteArray, force as Boolean) as Void {
        if (mBle == null || !mBle.isConnected()) { return; }
        if (!force && _bytesEqual(frame, mLastFrame)) { return; }
        if (mBle.writeWorkoutFrame(frame)) {
            mLastFrame = frame;
        }
    }

    hidden function _forcePush(timerState as Number) as Void {
        mTimerState = timerState;
        _maybeSend(_packFrame(timerState), true);
    }

    function compute(info as Activity.Info) as Void {
        try {
            mTimerState = (info has :timerState) && info.timerState != null
                ? info.timerState : Activity.TIMER_STATE_OFF;
            _maybeSend(_packFrame(mTimerState), false);
        } catch (e) {
            System.println("DataFieldView compute error: " + e.getErrorMessage());
        }
    }

    // Timer transitions push immediately so pause stops the belt and resume
    // re-commands without waiting for the next 1 Hz compute().
    function onTimerStart() as Void  { _forcePush(Activity.TIMER_STATE_ON); }
    function onTimerResume() as Void { _forcePush(Activity.TIMER_STATE_ON); }
    function onTimerPause() as Void  { _forcePush(Activity.TIMER_STATE_PAUSED); }
    function onTimerStop() as Void   { _forcePush(Activity.TIMER_STATE_STOPPED); }

    hidden function _timerStr() as String {
        if (mTimerState == Activity.TIMER_STATE_ON) { return "RUN"; }
        if (mTimerState == Activity.TIMER_STATE_PAUSED) { return "PAUSE"; }
        if (mTimerState == Activity.TIMER_STATE_STOPPED) { return "STOP"; }
        return "OFF";
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

        // Target speed (top)
        var tgt = mHasSpeedTarget ? (mTargetKmh.format("%.1f") + " km/h") : "-- km/h";
        dc.drawText(w / 2, h / 4, Graphics.FONT_MEDIUM, tgt,
            Graphics.TEXT_JUSTIFY_CENTER | Graphics.TEXT_JUSTIFY_VCENTER);

        // Timer state (middle)
        dc.drawText(w / 2, h / 2, Graphics.FONT_XTINY, _timerStr(),
            Graphics.TEXT_JUSTIFY_CENTER | Graphics.TEXT_JUSTIFY_VCENTER);

        // Bridge link state (bottom): CONN / SCAN / --
        var link = "--";
        if (mBle != null) {
            if (mBle.isConnected()) {
                link = "CONN";
            } else if (mBle.isScanning()) {
                link = "SCAN";
            }
        }
        dc.drawText(w / 2, h * 3 / 4, Graphics.FONT_XTINY, link,
            Graphics.TEXT_JUSTIFY_CENTER | Graphics.TEXT_JUSTIFY_VCENTER);
    }
}
