package com.cluffa.garmin_ftms_app

import android.content.Context
import android.os.Handler
import android.os.Looper
import android.util.Log
import com.garmin.android.connectiq.ConnectIQ
import com.garmin.android.connectiq.IQApp
import com.garmin.android.connectiq.IQDevice
import com.garmin.android.connectiq.exception.InvalidStateException
import com.garmin.android.connectiq.exception.ServiceUnavailableException
import io.flutter.plugin.common.BinaryMessenger
import io.flutter.plugin.common.MethodCall
import io.flutter.plugin.common.MethodChannel

// Must match the id="..." in garmin_data_field/manifest.xml — Connect IQ filters
// registerForAppEvents/sendMessage by this UUID, so a mismatch here means the
// watch app transmits and the phone never receives it, silently.
private const val APP_UUID = "e0123456-7890-1234-5678-123456789012"
private const val TAG = "GarminCiqPlugin"

class GarminCiqPlugin(
    private val context: Context,
    messenger: BinaryMessenger,
) : MethodChannel.MethodCallHandler {

    private val channel = MethodChannel(messenger, "com.cluffa.garmin_ftms/ciq")
    private val connectIQ = ConnectIQ.getInstance(context, ConnectIQ.IQConnectType.WIRELESS)
    private val handler = Handler(Looper.getMainLooper())
    private var sdkReady = false
    private var devices: List<IQDevice> = emptyList()

    // Retry loading devices every 5s until at least one is found.
    // loadDevices() itself re-schedules this when devices are still empty —
    // do not also reschedule here, or every cycle doubles the number of
    // pending callbacks (1, 2, 4, 8, ...), spiraling into a runaway loop.
    private val retryLoad = Runnable {
        if (sdkReady && devices.isEmpty()) {
            loadDevices()
        }
    }

    init {
        channel.setMethodCallHandler(this)
        connectIQ.initialize(context, true, object : ConnectIQ.ConnectIQListener {
            override fun onInitializeError(err: ConnectIQ.IQSdkErrorStatus) {
                Log.w(TAG, "onInitializeError: $err")
                sdkReady = false
            }
            override fun onSdkReady() {
                Log.d(TAG, "onSdkReady")
                sdkReady = true
                loadDevices()
            }
            override fun onSdkShutDown() {
                Log.d(TAG, "onSdkShutDown")
                sdkReady = false
                handler.removeCallbacks(retryLoad)
            }
        })
    }

    override fun onMethodCall(call: MethodCall, result: MethodChannel.Result) {
        when (call.method) {
            "selectDevice" -> {
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
        Log.d(TAG, "loadDevices: sdkReady=$sdkReady")
        try {
            devices = connectIQ.connectedDevices ?: emptyList()
            Log.d(TAG, "connectedDevices -> ${devices.map { it.friendlyName }}")
            channel.invokeMethod("onDevices", devices.map { mapOf("name" to it.friendlyName) })
            for (device in devices) {
                channel.invokeMethod("onDeviceStatus", mapOf(
                    "connected" to true,
                    "name" to device.friendlyName,
                ))
                val app = IQApp(APP_UUID)
                connectIQ.registerForAppEvents(device, app) { _, _, message, _ ->
                    Log.d(TAG, "onAppEvent from ${device.friendlyName}: $message")
                    (message?.firstOrNull() as? Map<*, *>)?.let { dict ->
                        channel.invokeMethod("onCommand", dict.mapKeys { it.key.toString() })
                    }
                }
                connectIQ.registerForDeviceEvents(device) { dev, status ->
                    Log.d(TAG, "onDeviceEvent ${dev.friendlyName} -> $status")
                    channel.invokeMethod("onDeviceStatus", mapOf(
                        "connected" to (status == IQDevice.IQDeviceStatus.CONNECTED),
                        "name" to dev.friendlyName,
                    ))
                }
            }
            if (devices.isEmpty()) {
                Log.d(TAG, "no connected devices, scheduling retry")
                scheduleRetry()
            }
        } catch (e: InvalidStateException) {
            Log.w(TAG, "loadDevices: InvalidStateException", e)
        } catch (e: ServiceUnavailableException) {
            // Garmin Connect Mobile's AIDL binder call died (RemoteException) — the
            // service is transiently unreachable (GCM backgrounded/killed, BT churn).
            // Without this, the uncaught exception aborts loadDevices() permanently:
            // no devices, no retry, and status never recovers even once GCM comes back.
            Log.w(TAG, "loadDevices: ServiceUnavailableException, scheduling retry", e)
            scheduleRetry()
        }
    }

    // Handler.postDelayed does not dedupe: calling this twice before the first
    // fires (e.g. two rapid selectDevice taps) queues two separate callbacks.
    // removeCallbacks first guarantees at most one retryLoad is ever pending.
    private fun scheduleRetry() {
        handler.removeCallbacks(retryLoad)
        handler.postDelayed(retryLoad, 5000)
    }
}
