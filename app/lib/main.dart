import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import 'services/bridge_service.dart';
import 'services/garmin_ciq_service.dart';
import 'screens/home_screen.dart';

void main() {
  runApp(
    ChangeNotifierProvider(
      create: (_) => BridgeService(),
      child: const FtmsSyncApp(),
    ),
  );
}

class FtmsSyncApp extends StatefulWidget {
  const FtmsSyncApp({super.key});
  @override
  State<FtmsSyncApp> createState() => _FtmsSyncAppState();
}

class _FtmsSyncAppState extends State<FtmsSyncApp> {
  GarminCiqService? _ciq;

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    _ciq ??= GarminCiqService(context.read<BridgeService>());
  }

  @override
  void dispose() {
    _ciq?.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'FTMS Sync',
      theme: ThemeData(
        colorScheme: ColorScheme.fromSeed(seedColor: Colors.indigo),
        useMaterial3: true,
      ),
      home: const HomeScreen(),
    );
  }
}
