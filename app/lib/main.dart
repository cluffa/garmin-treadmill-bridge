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

class FtmsSyncApp extends StatelessWidget {
  const FtmsSyncApp({super.key});

  @override
  Widget build(BuildContext context) {
    return ChangeNotifierProxyProvider<BridgeService, GarminCiqService>(
      create: (ctx) => GarminCiqService(ctx.read<BridgeService>()),
      update: (_, bridge, prev) => prev ?? GarminCiqService(bridge),
      child: MaterialApp(
        title: 'FTMS Sync',
        theme: ThemeData(
          colorScheme: ColorScheme.fromSeed(seedColor: Colors.indigo),
          useMaterial3: true,
        ),
        home: const HomeScreen(),
      ),
    );
  }
}
