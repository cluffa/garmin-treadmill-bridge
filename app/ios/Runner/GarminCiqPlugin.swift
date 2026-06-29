import Flutter
import ConnectIQ

// UUID of the CIQ data field app — must match the app-id in your .mc source
private let kAppUUID = UUID(uuidString: "a3421fee-d6d4-4e69-8bcd-42ac52e81013")!
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

    // Called by SceneDelegate when GCM returns to the app via URL scheme
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

    // MARK: - IQDeviceEventDelegate
    func deviceStatusChanged(_ device: IQDevice, status: IQDeviceStatus) {
        channel.invokeMethod("onDeviceStatus", arguments: [
            "connected": status == .connected,
            "name": device.friendlyName as Any
        ])
    }

    // MARK: - IQAppMessageDelegate — watch → phone commands
    func receivedMessage(_ message: Any, from app: IQApp) {
        guard let dict = message as? [String: Any] else { return }
        channel.invokeMethod("onCommand", arguments: dict)
    }
}
