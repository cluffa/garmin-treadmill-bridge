import Flutter

#if canImport(ConnectIQ)
import ConnectIQ

// Must match the id="..." in garmin_data_field/manifest.xml — Connect IQ filters
// registerForAppMessages/sendMessage by this UUID, so a mismatch here means the
// watch app transmits and the phone never receives it, silently.
private let kAppUUID = UUID(uuidString: "e0123456-7890-1234-5678-123456789012")!
private let kURLScheme = "garmin-ftms-sync"

class GarminCiqPlugin: NSObject, IQDeviceEventDelegate, IQAppMessageDelegate {
    static weak var shared: GarminCiqPlugin?
    private let channel: FlutterMethodChannel
    private var devices: [IQDevice] = []
    private var apps: [IQApp] = []

    static func register(with messenger: FlutterBinaryMessenger) {
        let instance = GarminCiqPlugin(messenger: messenger)
        shared = instance
    }

    init(messenger: FlutterBinaryMessenger) {
        channel = FlutterMethodChannel(name: "com.cluffa.garmin_ftms/ciq",
                                       binaryMessenger: messenger)
        super.init()
        channel.setMethodCallHandler(handle)
        ConnectIQ.sharedInstance().initialize(withUrlScheme: kURLScheme, uiOverrideDelegate: nil)
    }

    func handleOpen(url: URL) {
        guard let parsed = ConnectIQ.sharedInstance().parseDeviceSelectionResponse(from: url) as? [IQDevice] else { return }
        ConnectIQ.sharedInstance().unregister(forAllDeviceEvents: self)
        ConnectIQ.sharedInstance().unregister(forAllAppMessages: self)
        devices = parsed
        apps = parsed.map { device in
            let app = IQApp(uuid: kAppUUID, store: kAppUUID, device: device)!
            ConnectIQ.sharedInstance().register(forDeviceEvents: device, delegate: self)
            ConnectIQ.sharedInstance().register(forAppMessages: app, delegate: self)
            return app
        }
        let names = devices.map { ["name": $0.friendlyName as Any, "model": $0.modelName as Any] }
        channel.invokeMethod("onDevices", arguments: names)
        // getDeviceStatus only reflects state as of registration; deviceStatusChanged
        // only fires on a future transition, so query the current status now or an
        // already-connected watch never reports as connected.
        for device in devices {
            let status = ConnectIQ.sharedInstance().getDeviceStatus(device)
            channel.invokeMethod("onDeviceStatus", arguments: [
                "connected": status == .connected,
                "name": device.friendlyName as Any
            ])
        }
    }

    private func handle(_ call: FlutterMethodCall, result: @escaping FlutterResult) {
        switch call.method {
        case "selectDevice":
            ConnectIQ.sharedInstance().showDeviceSelection()
            result(nil)
        case "pushState":
            guard let args = call.arguments as? [String: Any], !apps.isEmpty else { result(nil); return }
            for app in apps {
                ConnectIQ.sharedInstance().sendMessage(args, to: app,
                    progress: { _, _ in },
                    completion: { _ in })
            }
            result(nil)
        default:
            result(FlutterMethodNotImplemented)
        }
    }

    func deviceStatusChanged(_ device: IQDevice, status: IQDeviceStatus) {
        channel.invokeMethod("onDeviceStatus", arguments: [
            "connected": status == .connected,
            "name": device.friendlyName as Any
        ])
    }

    func receivedMessage(_ message: Any, from app: IQApp) {
        guard let dict = message as? [String: Any] else { return }
        channel.invokeMethod("onCommand", arguments: dict)
    }
}

#else

class GarminCiqPlugin: NSObject {
    static weak var shared: GarminCiqPlugin?
    private let channel: FlutterMethodChannel

    static func register(with messenger: FlutterBinaryMessenger) {
        let instance = GarminCiqPlugin(messenger: messenger)
        shared = instance
    }

    init(messenger: FlutterBinaryMessenger) {
        channel = FlutterMethodChannel(name: "com.cluffa.garmin_ftms/ciq",
                                       binaryMessenger: messenger)
        super.init()
        channel.setMethodCallHandler(handle)
    }

    func handleOpen(url: URL) {}

    private func handle(_ call: FlutterMethodCall, result: @escaping FlutterResult) {
        switch call.method {
        case "selectDevice", "pushState":
            result(nil)  // ponytail: ConnectIQ unavailable (simulator)
        default:
            result(FlutterMethodNotImplemented)
        }
    }
}

#endif
