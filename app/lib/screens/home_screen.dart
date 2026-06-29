import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import '../services/bridge_service.dart';
import '../models/treadmill_state.dart';

class HomeScreen extends StatefulWidget {
  const HomeScreen({super.key});
  @override
  State<HomeScreen> createState() => _HomeScreenState();
}

class _HomeScreenState extends State<HomeScreen> {
  bool _scanning = false;

  @override
  Widget build(BuildContext context) {
    final bridge = context.watch<BridgeService>();
    return Scaffold(
      appBar: AppBar(title: const Text('FTMS Sync')),
      body: bridge.connectionMode == 'disconnected'
          ? _connectView(bridge)
          : _mainView(bridge),
    );
  }

  Widget _connectView(BridgeService bridge) => Center(
        child: Column(mainAxisSize: MainAxisSize.min, children: [
          const Text('No connection', style: TextStyle(fontSize: 18)),
          const SizedBox(height: 16),
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
        ]),
      );

  Widget _mainView(BridgeService bridge) => Column(children: [
        _statusBar(bridge),
        const Divider(),
        Expanded(child: _treadmillPanel(bridge)),
        const Divider(),
        _devicePanel(bridge),
      ]);

  Widget _statusBar(BridgeService bridge) => ListTile(
        leading: Icon(
          bridge.connected ? Icons.fitness_center : Icons.link_off,
          color: bridge.connected ? Colors.green : Colors.grey,
        ),
        title: Text(bridge.connectedDevice ?? 'No treadmill connected'),
        subtitle: Text('via ${bridge.connectionMode}'),
        trailing: IconButton(
          icon: const Icon(Icons.refresh),
          onPressed: bridge.status,
          tooltip: 'Refresh status',
        ),
      );

  Widget _treadmillPanel(BridgeService bridge) {
    final s = bridge.state;
    return Padding(
      padding: const EdgeInsets.all(16),
      child: Column(children: [
        _metric('Speed', '${s.speedKmh.toStringAsFixed(1)} km/h'),
        _metric('Distance', '${(s.distanceM / 1000).toStringAsFixed(2)} km'),
        _metric('Incline', '${s.inclinePct.toStringAsFixed(1)} %'),
        _metric('Elapsed', _formatElapsed(s.elapsedS)),
        const SizedBox(height: 24),
        _speedControl(bridge),
        const SizedBox(height: 12),
        _inclineControl(bridge),
        const SizedBox(height: 12),
        ElevatedButton.icon(
          style: ElevatedButton.styleFrom(backgroundColor: Colors.red),
          icon: const Icon(Icons.stop, color: Colors.white),
          label: const Text('STOP', style: TextStyle(color: Colors.white)),
          onPressed: bridge.stop,
        ),
      ]),
    );
  }

  Widget _metric(String label, String value) => Padding(
        padding: const EdgeInsets.symmetric(vertical: 4),
        child: Row(mainAxisAlignment: MainAxisAlignment.spaceBetween, children: [
          Text(label, style: const TextStyle(color: Colors.grey)),
          Text(value, style: const TextStyle(fontSize: 20, fontWeight: FontWeight.bold)),
        ]),
      );

  Widget _speedControl(BridgeService bridge) {
    final s = bridge.state;
    return Row(children: [
      const Text('Speed'),
      const SizedBox(width: 8),
      IconButton(
        icon: const Icon(Icons.remove),
        onPressed: () => bridge.setSpeed((s.speedKmh - 0.5).clamp(0, 25)),
      ),
      Text('${s.speedKmh.toStringAsFixed(1)}', style: const TextStyle(fontSize: 18)),
      IconButton(
        icon: const Icon(Icons.add),
        onPressed: () => bridge.setSpeed((s.speedKmh + 0.5).clamp(0, 25)),
      ),
      const Text('km/h'),
    ]);
  }

  Widget _inclineControl(BridgeService bridge) {
    final s = bridge.state;
    return Row(children: [
      const Text('Incline'),
      const SizedBox(width: 8),
      IconButton(
        icon: const Icon(Icons.remove),
        onPressed: () => bridge.setIncline((s.inclinePct - 0.5).clamp(-5, 15)),
      ),
      Text('${s.inclinePct.toStringAsFixed(1)}', style: const TextStyle(fontSize: 18)),
      IconButton(
        icon: const Icon(Icons.add),
        onPressed: () => bridge.setIncline((s.inclinePct + 0.5).clamp(-5, 15)),
      ),
      const Text('%'),
    ]);
  }

  Widget _devicePanel(BridgeService bridge) => Padding(
        padding: const EdgeInsets.all(8),
        child: Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
          Row(children: [
            const Text('Treadmill', style: TextStyle(fontWeight: FontWeight.bold)),
            const Spacer(),
            TextButton.icon(
              icon: _scanning
                  ? const SizedBox(width: 16, height: 16,
                      child: CircularProgressIndicator(strokeWidth: 2))
                  : const Icon(Icons.search),
              label: const Text('Scan'),
              onPressed: () async {
                setState(() => _scanning = true);
                await bridge.scan();
                await Future.delayed(const Duration(seconds: 8));
                await bridge.fetchList();
                if (mounted) setState(() => _scanning = false);
              },
            ),
          ]),
          ...bridge.devices.map((d) => ListTile(
                dense: true,
                title: Text(d.displayName),
                subtitle: Text('${d.proto}  ${d.rssi} dBm'),
                trailing: ElevatedButton(
                  child: const Text('Connect'),
                  onPressed: () => bridge.connectDevice(d.idx),
                ),
              )),
          if (bridge.devices.isEmpty && !_scanning)
            const Text('No devices found — tap Scan',
                style: TextStyle(color: Colors.grey)),
        ]),
      );

  String _formatElapsed(int s) {
    final h = s ~/ 3600;
    final m = (s % 3600) ~/ 60;
    final sec = s % 60;
    return h > 0
        ? '$h:${m.toString().padLeft(2,'0')}:${sec.toString().padLeft(2,'0')}'
        : '${m.toString().padLeft(2,'0')}:${sec.toString().padLeft(2,'0')}';
  }
}
