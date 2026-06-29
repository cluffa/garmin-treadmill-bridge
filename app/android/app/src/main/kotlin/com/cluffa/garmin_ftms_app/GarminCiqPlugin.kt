package com.cluffa.garmin_ftms_app

import android.content.Context
import io.flutter.plugin.common.BinaryMessenger
import io.flutter.plugin.common.MethodCall
import io.flutter.plugin.common.MethodChannel

/**
 * Wraps the Garmin ConnectIQ Android SDK.
 *
 * TODO: Add the ConnectIQ SDK AAR to android/app/libs/ and uncomment the
 * full implementation below. Until then this compiles as a no-op stub so
 * the app builds and the channel is wired up.
 *
 * SDK download: https://developer.garmin.com/connect-iq/sdk/
 * Drop connectiq-android-sdk-*.aar into android/app/libs/ and add to
 * android/app/build.gradle:
 *   implementation fileTree(dir: 'libs', include: ['*.aar'])
 */
class GarminCiqPlugin(
    private val context: Context,
    messenger: BinaryMessenger,
) : MethodChannel.MethodCallHandler {

    private val channel = MethodChannel(messenger, "com.cluffa.garmin_ftms/ciq")

    init {
        channel.setMethodCallHandler(this)
        // TODO: initConnectIQ()
    }

    override fun onMethodCall(call: MethodCall, result: MethodChannel.Result) {
        when (call.method) {
            "pushState" -> {
                // TODO: forward state to the paired CIQ app on the watch
                // val speed = call.argument<Double>("speed") ?: 0.0
                // connectIQ.sendMessage(device, app, mapOf("speed" to speed, ...), ...)
                result.success(null)
            }
            else -> result.notImplemented()
        }
    }

    // Called by the native ConnectIQ SDK when the watch sends a message:
    // channel.invokeMethod("onCommand", mapOf("type" to "speed", "value" to 8.5))
}
