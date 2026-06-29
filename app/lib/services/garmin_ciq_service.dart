import 'dart:async';
import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';
import '../models/treadmill_state.dart';
import 'bridge_service.dart';

/// Bridges Garmin Connect IQ ↔ BridgeService via a platform channel.
///
/// The native side (Android: GarminCiqPlugin.kt, iOS: GarminCiqPlugin.swift)
/// wraps the Garmin ConnectIQ SDK. It:
///   - Calls back into Dart when the watch sends a command (speed/incline/stop)
///   - Receives state pushes from Dart to forward to the CIQ data field
///
/// Channel protocol (method names):
///   Dart→Native:  pushState({speed, distance, incline, elapsed})
///   Native→Dart:  onCommand({type: 'speed'|'incline'|'stop', value?: double})
class GarminCiqService {
  static const _channel = MethodChannel('com.cluffa.garmin_ftms/ciq');

  final BridgeService _bridge;
  StreamSubscription? _stateSub;

  GarminCiqService(this._bridge) {
    _channel.setMethodCallHandler(_onNativeCall);
    // Forward live state to the watch
    _stateSub = _bridge.stateStream.listen(_pushState);
  }

  Future<dynamic> _onNativeCall(MethodCall call) async {
    final args = call.arguments as Map?;
    switch (call.method) {
      case 'onCommand':
        final type = args?['type'] as String?;
        final value = (args?['value'] as num?)?.toDouble();
        if (type == 'speed' && value != null) await _bridge.setSpeed(value);
        if (type == 'incline' && value != null) await _bridge.setIncline(value);
        if (type == 'stop') await _bridge.stop();
    }
  }

  void _pushState(TreadmillState s) {
    _channel.invokeMethod('pushState', {
      'speed': s.speedKmh,
      'distance': s.distanceM,
      'incline': s.inclinePct,
      'elapsed': s.elapsedS,
    }).catchError((_) {}); // watch may not be paired yet
  }

  void dispose() {
    _stateSub?.cancel();
  }
}
