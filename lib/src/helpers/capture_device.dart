import 'package:flutter_soloud/src/soloud.dart';
import 'package:meta/meta.dart';

/// Capture input device exposed to Dart.
///
/// Used to get a list of available capture devices by calling
/// [SoLoud.listCaptureDevices]. Pass one of these devices to
/// [SoLoud.startCapture] or [SoLoud.startCaptureAndPlay] to record from a
/// specific input instead of the system default input.
final class CaptureDevice {
  /// Constructs a new [CaptureDevice].
  @internal
  // ignore: avoid_positional_boolean_parameters
  const CaptureDevice(this.id, this.isDefault, this.name);

  /// The ID of the device.
  final int id;

  /// Whether this is the default capture device.
  final bool isDefault;

  /// The name of the device.
  final String name;

  @override
  String toString() =>
      '\nCaptureDevice(id: $id, isDefault: $isDefault, name: $name)';
}
