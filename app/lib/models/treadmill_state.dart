class TreadmillState {
  final double speedKmh;
  final double distanceM;
  final double inclinePct;
  final int elapsedS;

  const TreadmillState({
    this.speedKmh = 0,
    this.distanceM = 0,
    this.inclinePct = 0,
    this.elapsedS = 0,
  });

  factory TreadmillState.fromJson(Map<String, dynamic> j) => TreadmillState(
        speedKmh: (j['speed'] as num).toDouble(),
        distanceM: (j['distance'] as num).toDouble(),
        inclinePct: (j['incline'] as num).toDouble(),
        elapsedS: (j['elapsed'] as num).toInt(),
      );
}

class BridgeDevice {
  final int idx;
  final String name;
  final String proto;
  final int rssi;

  const BridgeDevice({
    required this.idx,
    required this.name,
    required this.proto,
    required this.rssi,
  });

  factory BridgeDevice.fromJson(Map<String, dynamic> j) => BridgeDevice(
        idx: j['idx'] as int,
        name: j['name'] as String? ?? '',
        proto: j['proto'] as String? ?? '',
        rssi: j['rssi'] as int? ?? -100,
      );

  String get displayName => name.isNotEmpty ? name : 'Unknown ($proto)';
}
