import 'dart:async';
import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import '../services/bridge_service.dart';
import '../services/garmin_ciq_service.dart';

class HomeScreen extends StatefulWidget {
  const HomeScreen({super.key});
  @override
  State<HomeScreen> createState() => _HomeScreenState();
}

class _HomeScreenState extends State<HomeScreen> {
  int _tab = 0;
  bool _scanning = false;
  bool _bleScanning = false;
  List<ScanResult> _bleResults = [];
  StreamSubscription? _bleScanSub;

  @override
  Widget build(BuildContext context) {
    final bridge = context.watch<BridgeService>();
    final garmin = context.watch<GarminCiqService>();
    return Scaffold(
      appBar: AppBar(title: const Text('FTMS Sync')),
      body: SafeArea(
        child: IndexedStack(
          index: _tab,
          children: [
            _treadmillTab(bridge),
            _garminTab(garmin),
          ],
        ),
      ),
      bottomNavigationBar: BottomNavigationBar(
        currentIndex: _tab,
        onTap: (i) => setState(() => _tab = i),
        items: const [
          BottomNavigationBarItem(
              icon: Icon(Icons.fitness_center), label: 'Treadmill'),
          BottomNavigationBarItem(
              icon: Icon(Icons.watch), label: 'Garmin'),
        ],
      ),
    );
  }

  @override
  void dispose() {
    _bleScanSub?.cancel();
    super.dispose();
  }

  Future<void> _startBleDiscovery(BridgeService bridge) async {
    setState(() { _bleScanning = true; _bleResults = []; });
    _bleScanSub?.cancel();
    _bleScanSub = bridge.scanStream.listen((results) {
      if (mounted) setState(() => _bleResults = results);
    });
    await bridge.startBleScan();
    if (mounted) setState(() => _bleScanning = false);
  }

  // ── Treadmill tab ─────────────────────────────────────────────────────────

  Widget _treadmillTab(BridgeService bridge) =>
      bridge.connectionMode == 'disconnected'
          ? _connectView(bridge)
          : _mainView(bridge);

  Widget _connectView(BridgeService bridge) => Center(
        child: Padding(
          padding: const EdgeInsets.all(24),
          child: Column(mainAxisSize: MainAxisSize.min, children: [
            const Text('Connect to ESP32 bridge',
                style: TextStyle(fontSize: 18, fontWeight: FontWeight.bold)),
            const SizedBox(height: 24),
            if (bridge.canUsb) ...[
              ElevatedButton.icon(
                icon: const Icon(Icons.usb),
                label: const Text('Connect via USB Serial'),
                onPressed: () async {
                  final ok = await bridge.connectUsb();
                  if (!ok && mounted) {
                    ScaffoldMessenger.of(context).showSnackBar(
                      const SnackBar(content: Text('No USB device found')));
                  }
                },
              ),
              const SizedBox(height: 16),
              const Text('— or —', style: TextStyle(color: Colors.grey)),
              const SizedBox(height: 16),
            ],
            ElevatedButton.icon(
              icon: _bleScanning
                  ? const SizedBox(
                      width: 18, height: 18,
                      child: CircularProgressIndicator(
                          strokeWidth: 2, color: Colors.white))
                  : const Icon(Icons.bluetooth_searching),
              label: Text(_bleScanning ? 'Scanning…' : 'Scan for ESP32'),
              onPressed: _bleScanning ? null : () => _startBleDiscovery(bridge),
            ),
            if (_bleResults.isNotEmpty) ...[
              const SizedBox(height: 16),
              ..._bleResults.map((r) => ListTile(
                    leading: const Icon(Icons.bluetooth),
                    title: Text(r.device.platformName.isNotEmpty
                        ? r.device.platformName
                        : r.device.remoteId.str),
                    subtitle: Text('${r.rssi} dBm'),
                    trailing: ElevatedButton(
                      child: const Text('Connect'),
                      onPressed: () async {
                        final ok = await bridge.connectBle(r.device);
                        if (!ok && mounted) {
                          ScaffoldMessenger.of(context).showSnackBar(
                            const SnackBar(
                                content: Text('Could not connect to ESP32')));
                        }
                      },
                    ),
                  )),
            ] else if (!_bleScanning)
              const Padding(
                padding: EdgeInsets.only(top: 12),
                child: Text('Tap scan to find nearby bridges',
                    style: TextStyle(color: Colors.grey)),
              ),
          ]),
        ),
      );

  Widget _mainView(BridgeService bridge) => Column(children: [
        _statusBar(bridge),
        const Divider(height: 1),
        Expanded(child: _treadmillPanel(bridge)),
        const Divider(height: 1),
        _devicePanel(bridge),
      ]);

  Widget _statusBar(BridgeService bridge) => ListTile(
        leading: Icon(
          bridge.connected ? Icons.fitness_center : Icons.link_off,
          color: bridge.connected ? Colors.green : Colors.grey,
        ),
        title: Text(bridge.connectedDevice?.isNotEmpty == true
            ? bridge.connectedDevice!
            : bridge.connected
                ? 'Connected (no treadmill)'
                : 'No treadmill'),
        subtitle: Text('via ${bridge.connectionMode}'),
        trailing: Row(mainAxisSize: MainAxisSize.min, children: [
          IconButton(icon: const Icon(Icons.refresh), onPressed: bridge.status),
          IconButton(
            icon: const Icon(Icons.link_off),
            tooltip: 'Disconnect',
            onPressed: bridge.disconnect,
          ),
        ]),
      );

  Widget _treadmillPanel(BridgeService bridge) {
    final s = bridge.state;
    return SingleChildScrollView(
      padding: const EdgeInsets.all(16),
      child: Column(children: [
        _metric('Speed', '${s.speedKmh.toStringAsFixed(1)} km/h'),
        _metric('Pace', _formatPace(s.speedKmh)),
        _metric('Distance', '${(s.distanceM / 1000).toStringAsFixed(3)} km'),
        _metric('Incline', '${s.inclinePct.toStringAsFixed(1)} %'),
        _metric('Elapsed', _formatElapsed(s.elapsedS)),
        const SizedBox(height: 24),
        _speedControl(bridge),
        const SizedBox(height: 8),
        _inclineControl(bridge),
        const SizedBox(height: 20),
        SizedBox(
          width: double.infinity,
          child: ElevatedButton.icon(
            style: ElevatedButton.styleFrom(
                backgroundColor: Colors.red, foregroundColor: Colors.white),
            icon: const Icon(Icons.stop),
            label: const Text('STOP TREADMILL'),
            onPressed: bridge.stop,
          ),
        ),
      ]),
    );
  }

  Widget _speedControl(BridgeService bridge) => Row(
        mainAxisAlignment: MainAxisAlignment.center,
        children: [
          const SizedBox(
              width: 60,
              child: Text('Speed', textAlign: TextAlign.right)),
          IconButton(
            iconSize: 32,
            icon: const Icon(Icons.remove_circle_outline),
            onPressed: () =>
                bridge.setSpeed((bridge.state.speedKmh - 0.5).clamp(0, 25)),
          ),
          SizedBox(
            width: 64,
            child: Text('${bridge.state.speedKmh.toStringAsFixed(1)}',
                textAlign: TextAlign.center,
                style: const TextStyle(
                    fontSize: 20, fontWeight: FontWeight.bold)),
          ),
          IconButton(
            iconSize: 32,
            icon: const Icon(Icons.add_circle_outline),
            onPressed: () =>
                bridge.setSpeed((bridge.state.speedKmh + 0.5).clamp(0, 25)),
          ),
          const Text('km/h'),
        ],
      );

  Widget _inclineControl(BridgeService bridge) => Row(
        mainAxisAlignment: MainAxisAlignment.center,
        children: [
          const SizedBox(
              width: 60,
              child: Text('Incline', textAlign: TextAlign.right)),
          IconButton(
            iconSize: 32,
            icon: const Icon(Icons.remove_circle_outline),
            onPressed: () =>
                bridge.setIncline((bridge.state.inclinePct - 0.5).clamp(-5, 15)),
          ),
          SizedBox(
            width: 64,
            child: Text('${bridge.state.inclinePct.toStringAsFixed(1)}',
                textAlign: TextAlign.center,
                style: const TextStyle(
                    fontSize: 20, fontWeight: FontWeight.bold)),
          ),
          IconButton(
            iconSize: 32,
            icon: const Icon(Icons.add_circle_outline),
            onPressed: () =>
                bridge.setIncline((bridge.state.inclinePct + 0.5).clamp(-5, 15)),
          ),
          const Text('%'),
        ],
      );

  Widget _devicePanel(BridgeService bridge) => Padding(
        padding: const EdgeInsets.fromLTRB(16, 8, 16, 16),
        child: Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
          Row(children: [
            const Text('Treadmill',
                style: TextStyle(fontWeight: FontWeight.bold, fontSize: 16)),
            const Spacer(),
            TextButton.icon(
              icon: _scanning
                  ? const SizedBox(
                      width: 16,
                      height: 16,
                      child: CircularProgressIndicator(strokeWidth: 2))
                  : const Icon(Icons.search),
              label: const Text('Scan'),
              onPressed: _scanning
                  ? null
                  : () async {
                      setState(() => _scanning = true);
                      await bridge.scan();
                      await Future.delayed(const Duration(seconds: 15));
                      await bridge.fetchList();
                      if (mounted) setState(() => _scanning = false);
                    },
            ),
          ]),
          ...bridge.devices.map((d) => ListTile(
                dense: true,
                title: Text(d.displayName),
                subtitle: Text('${d.proto}  •  ${d.rssi} dBm'),
                trailing: ElevatedButton(
                  child: const Text('Connect'),
                  onPressed: () => bridge.connectDevice(d.idx),
                ),
              )),
          if (bridge.devices.isEmpty && !_scanning)
            const Padding(
              padding: EdgeInsets.only(top: 4),
              child: Text('No devices — tap Scan',
                  style: TextStyle(color: Colors.grey)),
            ),
        ]),
      );

  // ── Garmin tab ────────────────────────────────────────────────────────────

  Widget _garminTab(GarminCiqService garmin) => SingleChildScrollView(
        padding: const EdgeInsets.all(16),
        child: Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
          Card(
            child: ListTile(
              leading: Icon(Icons.watch,
                  color: garmin.deviceConnected ? Colors.blue : Colors.grey),
              title: Text(garmin.deviceConnected
                  ? (garmin.deviceName ?? 'Watch connected')
                  : 'No watch connected'),
              subtitle: Text(garmin.deviceConnected
                  ? (garmin.receivingData
                      ? 'Receiving data from watch app'
                      : 'Paired, but no data from watch app')
                  : 'via Garmin Connect Mobile'),
              trailing: IconButton(
                icon: const Icon(Icons.refresh),
                tooltip: 'Refresh',
                onPressed: garmin.selectDevice,
              ),
            ),
          ),
          const SizedBox(height: 16),
          const Text('From Watch',
              style: TextStyle(fontWeight: FontWeight.bold, fontSize: 16)),
          const SizedBox(height: 8),
          _metric('Target Low',
              garmin.targetSpeedLowKmh != null
                  ? _formatPace(garmin.targetSpeedLowKmh!)
                  : '--'),
          _metric('Target High',
              garmin.targetSpeedHighKmh != null
                  ? _formatPace(garmin.targetSpeedHighKmh!)
                  : '--'),
          _metric('Activity Speed',
              garmin.watchCurrentSpeedKmh != null
                  ? '${garmin.watchCurrentSpeedKmh!.toStringAsFixed(2)} km/h'
                  : '--'),
          const SizedBox(height: 16),
          const Text('Debug: raw last message from watch',
              style: TextStyle(fontWeight: FontWeight.bold, fontSize: 16)),
          const SizedBox(height: 8),
          SelectableText(
            garmin.lastRawCommand ?? '(none received yet)',
            style: const TextStyle(fontFamily: 'monospace', fontSize: 12),
          ),
        ]),
      );

  // ── Shared helpers ────────────────────────────────────────────────────────

  Widget _metric(String label, String value) => Padding(
        padding: const EdgeInsets.symmetric(vertical: 6),
        child: Row(
            mainAxisAlignment: MainAxisAlignment.spaceBetween,
            children: [
              Text(label,
                  style: const TextStyle(color: Colors.grey, fontSize: 16)),
              Text(value,
                  style: const TextStyle(
                      fontSize: 22, fontWeight: FontWeight.bold)),
            ]),
      );

  String _formatElapsed(int s) {
    final h = s ~/ 3600;
    final m = (s % 3600) ~/ 60;
    final sec = s % 60;
    return h > 0
        ? '$h:${m.toString().padLeft(2, '0')}:${sec.toString().padLeft(2, '0')}'
        : '${m.toString().padLeft(2, '0')}:${sec.toString().padLeft(2, '0')}';
  }

  String _formatPace(double speedKmh) {
    if (speedKmh <= 0.1) return '--:-- /mi';
    final paceMins = 96.5604 / speedKmh;
    final mins = paceMins.truncate();
    final secs = ((paceMins - mins) * 60).round();
    return '$mins:${secs.toString().padLeft(2, '0')} /mi';
  }
}
