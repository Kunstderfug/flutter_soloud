import 'package:flutter/material.dart';

import 'exit_app.dart';
import 'tests/mixer_output_capture.dart';

/// One-shot runner for the mixer-output capture integration test.
///
/// Run with:
/// `flutter run -d macos -t tests/run_mixer_output_capture_test.dart`.
void main() async {
  WidgetsFlutterBinding.ensureInitialized();

  try {
    final output = await testMixerOutputCapture();
    // ignore: avoid_print
    print('MIXER_OUTPUT_CAPTURE_TEST_PASSED');
    // ignore: avoid_print
    print(output);
    exitApp(0);
  } catch (error, stackTrace) {
    // ignore: avoid_print
    print('MIXER_OUTPUT_CAPTURE_TEST_FAILED: $error');
    // ignore: avoid_print
    print(stackTrace);
    exitApp(1);
  }
}
