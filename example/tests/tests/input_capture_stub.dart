import 'common.dart';

/// Input-device capture is not implemented on web.
Future<OutputBuffer> testInputCapture() async {
  return OutputBuffer()..writeln('InputCapture skipped: unsupported platform');
}
