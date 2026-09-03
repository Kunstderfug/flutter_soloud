import 'package:flutter/material.dart';

import 'exit_app.dart';
import 'tests/idle_engine.dart';

/// One-shot regression runner for idle audio-device teardown.
///
/// Run with:
///   flutter run -d macos -t tests/run_idle_engine_test.dart
void main() async {
  WidgetsFlutterBinding.ensureInitialized();

  try {
    final output = await testIdleEngineAfterMixingBusDispose();
    // ignore: avoid_print
    print('IDLE_ENGINE_TEST_PASSED');
    // ignore: avoid_print
    print(output);
    exitApp(0);
  } catch (error, stackTrace) {
    // ignore: avoid_print
    print('IDLE_ENGINE_TEST_FAILED: $error');
    // ignore: avoid_print
    print(stackTrace);
    exitApp(1);
  }
}
