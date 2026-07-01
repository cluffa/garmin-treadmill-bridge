package com.cluffa.garmin_ftms_app

import android.content.Context
import com.garmin.android.connectiq.ConnectIQ
import com.garmin.android.connectiq.IQApp
import com.garmin.android.connectiq.IQDevice
import com.garmin.android.connectiq.exception.InvalidStateException
import com.garmin.android.connectiq.exception.ServiceUnavailableException
import io.flutter.plugin.common.BinaryMessenger
import io.flutter.plugin.common.MethodCall
import io.flutter.plugin.common.MethodChannel

// UUID of the CIQ data field app — must match the app-id in your .mc source
private const val APP_UUID = "a3421fee-d6d4-4e69-8bcd-42ac52e81013"

class GarminCiqPlugin(
    private val context: Context,
    messenger: BinaryMessenger,
) : MethodChannel.MethodCallHandler {

    private val channel = MethodChannel(messenger, "com.cluffa.garmin_ftms/ciq")
    private val connectIQ = ConnectIQ.getInstance(context, ConnectIQ.IQConnectType.WIRELESS)
    private var sdkReady = false
    private var devices: List<IQDevice> = emptyList()

    init {
        channel.setMethodCallHandler(this)
        connectIQ.initialize(context, true, object : ConnectIQ.ConnectIQListener {
            override fun onInitializeError(err: ConnectIQ.IQSdkErrorStatus) { sdkReady = false }
            override fun onSdkReady() { sdkReady = true; loadDevices() }
            override fun onSdkShutDown() { sdkReady = false }
        })
    }

    override fun onMethodCall(call: MethodCall, result: MethodChannel.Result) {
        when (call.method) {
            "selectDevice" -> {
                // On Android the SDK handles device discovery internally via GCM;
                // loadDevices() re-queries known paired devices
                if (sdkReady) loadDevices()
                result.success(null)
            }
            "pushState" -> {
                if (!sdkReady || devices.isEmpty()) { result.success(null); return }
                val args = call.arguments as? Map<*, *> ?: run { result.success(null); return }
                val payload = mapOf(
                    "speed" to (args["speed"] as? Double ?: 0.0),
                    "distance" to (args["distance"] as? Double ?: 0.0),
                    "incline" to (args["incline"] as? Double ?: 0.0),
                    "elapsed" to (args["elapsed"] as? Int ?: 0),
                )
                for (device in devices) {
                    val app = IQApp(APP_UUID)
                    try {
                        connectIQ.sendMessage(device, app, payload) { _, _, _ -> }
                    } catch (_: InvalidStateException) {
                    } catch (_: ServiceUnavailableException) {
                    }
                }
                result.success(null)
            }
            else -> result.notImplemented()
        }
    }

    private fun loadDevices() {
        try {
            devices = connectIQ.connectedDevices ?: emptyList()
            val names = devices.map { mapOf("name" to it.friendlyName) }
            channel.invokeMethod("onDevices", names)
            for (device in devices) {
                // connectedDevices only returns devices that are already connected,
                // so report status immediately rather than waiting for a change event.
                channel.invokeMethod("onDeviceStatus", mapOf(
                    "connected" to true,
                    "name" to device.friendlyName,
                ))
                val app = IQApp(APP_UUID)
                connectIQ.registerForAppEvents(device, app) { dev, _, message, _ ->
                    (message?.firstOrNull() as? Map<*, *>)?.let { dict ->
                        channel.invokeMethod("onCommand", dict.mapKeys { it.key.toString() })
                    }
                }
                connectIQ.registerForDeviceEvents(device) { dev, status ->
                    channel.invokeMethod("onDeviceStatus", mapOf(
                        "connected" to (status == IQDevice.IQDeviceStatus.CONNECTED),
                        "name" to dev.friendlyName,
                    ))
                }
            }
        } catch (_: InvalidStateException) {
        }
    }
}
