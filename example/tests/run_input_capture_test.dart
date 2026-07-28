import 'package:flutter/material.dart';

import 'exit_app.dart';
import 'tests/input_capture.dart';

/// One-shot runner for the native microphone capture integration test.
///
/// Run with: `flutter run -d macos -t tests/run_input_capture_test.dart`.
///
/// Android requires a runtime permission grant before launching the test:
/// `adb shell pm grant com.example.flutter_soloud_example
/// android.permission.RECORD_AUDIO`.
/// Then run:
/// `flutter run -d <android-device> -t tests/run_input_capture_test.dart`.
void main() async {
  WidgetsFlutterBinding.ensureInitialized();

  try {
    final output = await testInputCapture();
    // ignore: avoid_print
    print('INPUT_CAPTURE_TEST_PASSED');
    // ignore: avoid_print
    print(output);
    exitApp(0);
  } catch (error, stackTrace) {
    // ignore: avoid_print
    print('INPUT_CAPTURE_TEST_FAILED: $error');
    // ignore: avoid_print
    print(stackTrace);
    exitApp(1);
  }
}
