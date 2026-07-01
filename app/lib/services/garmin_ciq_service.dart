import 'dart:async';
import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';
import '../models/treadmill_state.dart';
import 'bridge_service.dart';

class GarminCiqService extends ChangeNotifier {
  static const _channel = MethodChannel('com.cluffa.garmin_ftms/ciq');

  final BridgeService _bridge;
  StreamSubscription? _stateSub;
  Timer? _staleTimer;

  bool _deviceConnected = false;
  String? _deviceName;
  double? _targetSpeedLowKmh;
  double? _targetSpeedHighKmh;
  double? _watchCurrentSpeedKmh;
  String? _lastRawCommand;
  DateTime? _lastMessageAt;

  bool get deviceConnected => _deviceConnected;
  String? get deviceName => _deviceName;
  double? get targetSpeedLowKmh => _targetSpeedLowKmh;
  double? get targetSpeedHighKmh => _targetSpeedHighKmh;
  double? get watchCurrentSpeedKmh => _watchCurrentSpeedKmh;
  // ponytail: debug-only, raw unformatted payload from the last onCommand call
  String? get lastRawCommand => _lastRawCommand;
  // True once an actual app message has arrived recently — distinct from
  // deviceConnected, which only reflects the BLE-level pairing and can be
  // true while the watch app UUID mismatches and nothing is really flowing.
  bool get receivingData =>
      _lastMessageAt != null &&
      DateTime.now().difference(_lastMessageAt!) < const Duration(seconds: 10);

  GarminCiqService(this._bridge) {
    _channel.setMethodCallHandler(_onNativeCall);
    _stateSub = _bridge.stateStream.listen(_pushState);
    // receivingData is time-based (staleness), so it needs a tick even when no
    // new message arrives to flip back to "not receiving" after the timeout.
    _staleTimer = Timer.periodic(const Duration(seconds: 3), (_) => notifyListeners());
  }

  Future<void> selectDevice() => _channel.invokeMethod('selectDevice');

  Future<dynamic> _onNativeCall(MethodCall call) async {
    final args = call.arguments as Map?;
    switch (call.method) {
      case 'onCommand':
        _lastRawCommand = args?.toString();
        _lastMessageAt = DateTime.now();
        final type = args?['type'] as String?;
        final value = (args?['value'] as num?)?.toDouble();
        if (type == 'speed' && value != null) await _bridge.setSpeed(value);
        if (type == 'incline' && value != null) await _bridge.setIncline(value);
        if (type == 'stop') await _bridge.stop();
        if (type == 'workoutStatus') {
          final targetPace = (args?['targetPace'] as num?)?.toDouble();
          final rawLow = (args?['targetPaceLow'] as num?)?.toDouble();
          final rawHigh = (args?['targetPaceHigh'] as num?)?.toDouble();
          final rawCurrent = (args?['currentSpeed'] as num?)?.toDouble();
          _targetSpeedLowKmh = rawLow != null ? rawLow * 3.6 : null;
          _targetSpeedHighKmh = rawHigh != null ? rawHigh * 3.6 : null;
          _watchCurrentSpeedKmh = rawCurrent != null ? rawCurrent * 3.6 : null;
          if (targetPace != null && targetPace > 0) {
            await _bridge.setSpeed(targetPace * 3.6);
          }
        }
        notifyListeners();
      case 'onDeviceStatus':
        _deviceConnected = args?['connected'] as bool? ?? false;
        _deviceName = args?['name'] as String?;
        notifyListeners();
      case 'onDevices':
        notifyListeners();
    }
  }

  void _pushState(TreadmillState s) {
    if (!_deviceConnected) return;
    _channel.invokeMethod('pushState', {
      'speed': s.speedKmh,
      'distance': s.distanceM,
      'incline': s.inclinePct,
      'elapsed': s.elapsedS,
    }).catchError((_) {});
  }

  @override
  void dispose() {
    _stateSub?.cancel();
    _staleTimer?.cancel();
    super.dispose();
  }
}
