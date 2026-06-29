package com.cluffa.garmin_ftms_app

import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.os.Handler
import android.os.Looper
import io.flutter.plugin.common.BinaryMessenger
import io.flutter.plugin.common.EventChannel
import io.flutter.plugin.common.MethodCall
import io.flutter.plugin.common.MethodChannel

/**
 * Handles USB serial communication with the ESP32 bridge.
 *
 * Uses Android's UsbManager directly (no external library needed for CDC ACM).
 * ESP32 native USB (XIAO C6) appears as CDC ACM class (bcdDevice 0x100).
 *
 * Channel: com.cluffa.garmin_ftms/bridge (MethodChannel)
 *   connectUsb()         → Bool
 *   send({cmd: String})  → void
 *   disconnect()         → void
 *
 * EventChannel: com.cluffa.garmin_ftms/bridge_events
 *   Streams raw text lines received from the device.
 */
class BridgePlugin(
    private val context: Context,
    messenger: BinaryMessenger,
) : MethodChannel.MethodCallHandler, EventChannel.StreamHandler {

    private val methodChannel = MethodChannel(messenger, "com.cluffa.garmin_ftms/bridge")
    private val eventChannel = EventChannel(messenger, "com.cluffa.garmin_ftms/bridge_events")
    private val usbManager = context.getSystemService(Context.USB_SERVICE) as UsbManager
    private val handler = Handler(Looper.getMainLooper())

    private var connection: android.hardware.usb.UsbDeviceConnection? = null
    private var endpoint: android.hardware.usb.UsbEndpoint? = null
    private var eventSink: EventChannel.EventSink? = null
    private var rxThread: Thread? = null

    // Vendor IDs of ESP32 USB chips
    private val knownVids = setOf(
        0x10C4, // CP210x (Silicon Labs)
        0x1A86, // CH340/CH341 (WCH)
        0x0403, // FTDI
        0x303A, // Espressif native USB
    )

    init {
        methodChannel.setMethodCallHandler(this)
        eventChannel.setStreamHandler(this)
    }

    override fun onMethodCall(call: MethodCall, result: MethodChannel.Result) {
        when (call.method) {
            "connectUsb" -> connectUsb(result)
            "send" -> {
                val cmd = (call.argument<String>("cmd") ?: "") + "\n"
                sendBytes(cmd.toByteArray(), result)
            }
            "disconnect" -> { disconnect(); result.success(null) }
            else -> result.notImplemented()
        }
    }

    private fun connectUsb(result: MethodChannel.Result) {
        val device = usbManager.deviceList.values
            .firstOrNull { knownVids.contains(it.vendorId) }
        if (device == null) { result.success(false); return }

        if (!usbManager.hasPermission(device)) {
            val intent = PendingIntent.getBroadcast(
                context, 0,
                Intent("com.cluffa.garmin_ftms.USB_PERMISSION"),
                PendingIntent.FLAG_IMMUTABLE
            )
            usbManager.requestPermission(device, intent)
            result.success(false); return
        }

        val intf = device.getInterface(1) // CDC data interface
        val ep = (0 until intf.endpointCount)
            .map { intf.getEndpoint(it) }
            .firstOrNull { it.direction == android.hardware.usb.UsbConstants.USB_DIR_IN }
            ?: run { result.success(false); return }

        val conn = usbManager.openDevice(device)
            ?: run { result.success(false); return }
        conn.claimInterface(intf, true)
        setCdcLineCoding(conn, device)

        connection = conn
        endpoint = ep
        startRxThread()
        result.success(true)
    }

    private fun setCdcLineCoding(conn: android.hardware.usb.UsbDeviceConnection, device: UsbDevice) {
        // 115200 baud, 1 stop bit, no parity, 8 data bits
        val lineCoding = byteArrayOf(0x00, 0xC2.toByte(), 0x01, 0x00, 0x00, 0x00, 0x08)
        conn.controlTransfer(0x21, 0x20, 0, 0, lineCoding, lineCoding.size, 1000)
    }

    private fun sendBytes(data: ByteArray, result: MethodChannel.Result) {
        val conn = connection
        val dev = usbManager.deviceList.values.firstOrNull { knownVids.contains(it.vendorId) }
        if (conn == null || dev == null) { result.success(null); return }
        val ep = dev.getInterface(1).let { intf ->
            (0 until intf.endpointCount).map { intf.getEndpoint(it) }
                .firstOrNull { it.direction == android.hardware.usb.UsbConstants.USB_DIR_OUT }
        } ?: run { result.success(null); return }
        conn.bulkTransfer(ep, data, data.size, 1000)
        result.success(null)
    }

    private fun startRxThread() {
        rxThread = Thread {
            val buf = ByteArray(512)
            val conn = connection ?: return@Thread
            val ep = endpoint ?: return@Thread
            while (!Thread.interrupted()) {
                val n = conn.bulkTransfer(ep, buf, buf.size, 200)
                if (n > 0) {
                    val text = String(buf, 0, n)
                    handler.post { eventSink?.success(text) }
                }
            }
        }.also { it.start() }
    }

    private fun disconnect() {
        rxThread?.interrupt()
        rxThread = null
        connection?.close()
        connection = null
    }

    // EventChannel.StreamHandler
    override fun onListen(args: Any?, sink: EventChannel.EventSink?) { eventSink = sink }
    override fun onCancel(args: Any?) { eventSink = null }
}
