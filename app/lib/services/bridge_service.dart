import 'dart:async';
import 'dart:convert';
import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';
import '../models/treadmill_state.dart';

/// Manages the connection to the ESP32 bridge.
/// Transport: USB serial (Android, via platform channel) or WebSocket (WiFi).
class BridgeService extends ChangeNotifier {
  static const _channel = MethodChannel('com.cluffa.garmin_ftms/bridge');
  static const _events = EventChannel('com.cluffa.garmin_ftms/bridge_events');

  StreamSubscription? _eventSub;
  final _lineBuffer = StringBuffer();

  TreadmillState state = const TreadmillState();
  List<BridgeDevice> devices = [];
  bool connected = false;
  String? connectedDevice;
  String connectionMode = 'disconnected'; // 'usb' | 'wifi' | 'disconnected'

  final _stateCtrl = StreamController<TreadmillState>.broadcast();
  Stream<TreadmillState> get stateStream => _stateCtrl.stream;

  Future<bool> connectUsb() async {
    try {
      final ok = await _channel.invokeMethod<bool>('connectUsb') ?? false;
      if (ok) {
        connectionMode = 'usb';
        connected = true;
        _eventSub = _events.receiveBroadcastStream().listen(_onEvent);
        notifyListeners();
      }
      return ok;
    } on PlatformException {
      return false;
    }
  }

  void _onEvent(dynamic data) {
    if (data is! String) return;
    _lineBuffer.write(data);
    final buf = _lineBuffer.toString();
    final lines = buf.split('\n');
    _lineBuffer.clear();
    if (lines.length > 1) _lineBuffer.write(lines.last);
    for (final line in lines.sublist(0, lines.length - 1)) {
      _handleLine(line.trim());
    }
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

  Future<void> _send(String cmd) =>
      _channel.invokeMethod('send', {'cmd': cmd});

  Future<void> scan() => _send('SCAN');
  Future<void> fetchList() => _send('LIST');
  Future<void> connectDevice(int idx) => _send('CONNECT $idx');
  Future<void> setSpeed(double kmh) => _send('SPEED $kmh');
  Future<void> setIncline(double pct) => _send('INCLINE $pct');
  Future<void> stop() => _send('STOP');
  Future<void> status() => _send('STATUS');

  @override
  void dispose() {
    _eventSub?.cancel();
    _stateCtrl.close();
    _channel.invokeMethod('disconnect');
    super.dispose();
  }
}
