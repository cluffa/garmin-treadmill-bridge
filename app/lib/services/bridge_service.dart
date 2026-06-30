import 'dart:async';
import 'dart:convert';
import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import '../models/treadmill_state.dart';

/// NUS (Nordic UART Service) UUIDs — matches nus_ctrl.c on the ESP32.
const _nusSvcUuid = '6e400001-b5a3-f393-e0a9-e50e24dcca9e';
const _nusRxUuid  = '6e400002-b5a3-f393-e0a9-e50e24dcca9e'; // phone → ESP32
const _nusTxUuid  = '6e400003-b5a3-f393-e0a9-e50e24dcca9e'; // ESP32 → phone

/// Manages the connection to the ESP32 bridge.
///
/// iOS / Android BLE path:  NUS service on the ESP32 (nus_ctrl.c).
/// Android USB serial path: native MethodChannel (unchanged).
class BridgeService extends ChangeNotifier {
  // USB serial path (Android only) — unchanged.
  static const _ch = MethodChannel('com.cluffa.garmin_ftms/bridge');
  static const _ev = EventChannel('com.cluffa.garmin_ftms/bridge_events');

  // BLE state
  BluetoothDevice? _bleDevice;
  BluetoothCharacteristic? _rxChar; // write to ESP32
  BluetoothCharacteristic? _txChar; // notify from ESP32
  StreamSubscription? _txSub;
  StreamSubscription? _usbEventSub;
  final _rxLineBuf = StringBuffer();

  TreadmillState state = const TreadmillState();
  List<BridgeDevice> devices = [];
  bool connected = false;
  String? connectedDevice;
  String connectionMode = 'disconnected'; // 'usb' | 'ble' | 'disconnected'

  final _stateCtrl = StreamController<TreadmillState>.broadcast();
  Stream<TreadmillState> get stateStream => _stateCtrl.stream;

  // BLE scan results stream for the connect UI.
  final _scanCtrl = StreamController<List<ScanResult>>.broadcast();
  Stream<List<ScanResult>> get scanStream => _scanCtrl.stream;
  StreamSubscription? _scanSub;

  // ── BLE path ─────────────────────────────────────────────────────────────

  Future<void> startBleScan() async {
    final results = <String, ScanResult>{};
    await FlutterBluePlus.stopScan();
    _scanSub?.cancel();
    _scanSub = FlutterBluePlus.scanResults.listen((list) {
      for (final r in list) {
        results[r.device.remoteId.str] = r;
      }
      _scanCtrl.add(results.values.toList());
    });
    await FlutterBluePlus.startScan(
      withServices: [Guid(_nusSvcUuid)],
      timeout: const Duration(seconds: 10),
    );
  }

  Future<bool> connectBle(BluetoothDevice device) async {
    try {
      await FlutterBluePlus.stopScan();
      _scanSub?.cancel();

      await device.connect(timeout: const Duration(seconds: 10));

      // Request a larger MTU so multi-device LIST responses fit in one notify.
      // ponytail: iOS negotiates MTU automatically; Android needs this call.
      if (!defaultTargetPlatform.toString().contains('iOS')) {
        await device.requestMtu(512);
      }

      final services = await device.discoverServices();
      final nusService = services.firstWhere(
        (s) => s.serviceUuid.toString().toLowerCase() == _nusSvcUuid,
      );

      _rxChar = nusService.characteristics.firstWhere(
        (c) => c.characteristicUuid.toString().toLowerCase() == _nusRxUuid,
      );
      _txChar = nusService.characteristics.firstWhere(
        (c) => c.characteristicUuid.toString().toLowerCase() == _nusTxUuid,
      );

      await _txChar!.setNotifyValue(true);
      _txSub = _txChar!.onValueReceived.listen(_onBleData);

      _bleDevice = device;
      _bleDevice!.connectionState.listen((s) {
        if (s == BluetoothConnectionState.disconnected) _onBleDisconnected();
      });

      _onConnected('ble');
      return true;
    } catch (e) {
      await device.disconnect().catchError((_) {});
      return false;
    }
  }

  void _onBleDisconnected() {
    _txSub?.cancel();
    _txSub = null;
    _bleDevice = null;
    _rxChar = null;
    _txChar = null;
    connected = false;
    connectedDevice = null;
    connectionMode = 'disconnected';
    notifyListeners();
  }

  void _onBleData(List<int> bytes) {
    _rxLineBuf.write(utf8.decode(bytes, allowMalformed: true));
    final buf = _rxLineBuf.toString();
    final lines = buf.split('\n');
    _rxLineBuf.clear();
    if (lines.length > 1) _rxLineBuf.write(lines.last);
    for (final line in lines.sublist(0, lines.length - 1)) {
      _handleLine(line.trim());
    }
  }

  // ── USB serial path (Android only) ───────────────────────────────────────

  bool get canUsb => defaultTargetPlatform == TargetPlatform.android;

  Future<bool> connectUsb() async {
    try {
      final ok = await _ch.invokeMethod<bool>('connectUsb') ?? false;
      if (ok) _onConnected('usb');
      return ok;
    } on PlatformException {
      return false;
    }
  }

  // ── shared ────────────────────────────────────────────────────────────────

  void _onConnected(String mode) {
    connectionMode = mode;
    connected = true;
    if (mode == 'usb') {
      _usbEventSub = _ev.receiveBroadcastStream().listen(_onUsbEvent);
    }
    notifyListeners();
  }

  void _onUsbEvent(dynamic data) {
    if (data is String) _onBleData(utf8.encode(data));
  }

  void _handleLine(String line) {
    if (line.isEmpty || !line.startsWith('{')) return;
    try {
      final j = jsonDecode(line) as Map<String, dynamic>;
      final event = j['event'] as String?;
      final cmd = j['cmd'] as String?;
      if (event == 'state') {
        state = TreadmillState.fromJson(j);
        _stateCtrl.add(state);
        notifyListeners();
      } else if (event == 'connected') {
        connectedDevice = j['name'] as String?;
        connected = true;
        notifyListeners();
      } else if (event == 'disconnected') {
        connectedDevice = null;
        notifyListeners();
      } else if (cmd == 'list') {
        final raw = j['devices'] as List<dynamic>? ?? [];
        devices = raw
            .map((d) => BridgeDevice.fromJson(d as Map<String, dynamic>))
            .toList();
        notifyListeners();
      }
    } catch (_) {}
  }

  Future<void> _send(String cmd) async {
    if (connectionMode == 'ble' && _rxChar != null) {
      final bytes = utf8.encode('$cmd\n');
      await _rxChar!.write(bytes, withoutResponse: true);
    } else if (connectionMode == 'usb') {
      await _ch.invokeMethod('send', {'cmd': cmd});
    }
  }

  Future<void> scan() => _send('SCAN');
  Future<void> fetchList() => _send('LIST');
  Future<void> connectDevice(int idx) => _send('CONNECT $idx');
  Future<void> setSpeed(double kmh) => _send('SPEED $kmh');
  Future<void> setIncline(double pct) => _send('INCLINE $pct');
  Future<void> stop() => _send('STOP');
  Future<void> status() => _send('STATUS');

  void disconnect() {
    _txSub?.cancel();
    _usbEventSub?.cancel();
    _bleDevice?.disconnect().catchError((_) {});
    _ch.invokeMethod('disconnect').catchError((_) {});
    _bleDevice = null;
    _rxChar = null;
    _txChar = null;
    connected = false;
    connectedDevice = null;
    connectionMode = 'disconnected';
    notifyListeners();
  }

  @override
  void dispose() {
    disconnect();
    _stateCtrl.close();
    _scanCtrl.close();
    _scanSub?.cancel();
    super.dispose();
  }
}
