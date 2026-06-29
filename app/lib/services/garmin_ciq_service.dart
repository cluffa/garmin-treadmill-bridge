import 'dart:async';
import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';
import '../models/treadmill_state.dart';
import 'bridge_service.dart';

class GarminCiqService extends ChangeNotifier {
  static const _channel = MethodChannel('com.cluffa.garmin_ftms/ciq');

  final BridgeService _bridge;
  StreamSubscription? _stateSub;

  bool _deviceConnected = false;
  String? _deviceName;
  bool get deviceConnected => _deviceConnected;
  String? get deviceName => _deviceName;

  GarminCiqService(this._bridge) {
    _channel.setMethodCallHandler(_onNativeCall);
    _stateSub = _bridge.stateStream.listen(_pushState);
  }

  Future<void> selectDevice() => _channel.invokeMethod('selectDevice');

  Future<dynamic> _onNativeCall(MethodCall call) async {
    final args = call.arguments as Map?;
    switch (call.method) {
      case 'onCommand':
        final type = args?['type'] as String?;
        final value = (args?['value'] as num?)?.toDouble();
        if (type == 'speed' && value != null) await _bridge.setSpeed(value);
        if (type == 'incline' && value != null) await _bridge.setIncline(value);
        if (type == 'stop') await _bridge.stop();
      case 'onDeviceStatus':
        _deviceConnected = args?['connected'] as bool? ?? false;
        _deviceName = args?['name'] as String?;
        notifyListeners();
      case 'onDevices':
        // devices paired — status will follow via onDeviceStatus
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
    super.dispose();
  }
}
