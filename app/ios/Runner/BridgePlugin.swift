import Flutter
import Foundation

/// Stub — the bridge now connects via BLE (flutter_blue_plus) in Dart.
/// This channel is kept registered to avoid a crash on the Android USB path,
/// but on iOS all methods return nil / false gracefully.
class BridgePlugin: NSObject {
    static func register(with messenger: FlutterBinaryMessenger) {
        let ch = FlutterMethodChannel(
            name: "com.cluffa.garmin_ftms/bridge",
            binaryMessenger: messenger)
        ch.setMethodCallHandler { call, result in
            switch call.method {
            case "connectUsb":   result(false)
            case "connectWifi":  result(false)
            case "send":         result(nil)
            case "disconnect":   result(nil)
            default:             result(FlutterMethodNotImplemented)
            }
        }
        // EventChannel stub — no events are pushed from the iOS native side.
        let ev = FlutterEventChannel(
            name: "com.cluffa.garmin_ftms/bridge_events",
            binaryMessenger: messenger)
        ev.setStreamHandler(nil)
    }
}
