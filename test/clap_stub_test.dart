import 'dart:ffi';

import 'package:ffi/ffi.dart';
import 'package:flutter_soloud/src/bindings/flutter_soloud_ffigen.dart'
    as native;
import 'package:test/test.dart';

/// Whether this run's native build should have CLAP hosting compiled in:
/// `--dart-define=EXPECT_CLAP=1` when the root package enables `enable_clap`
/// on macOS. Unset, the result only has to be self-consistent.
const _expectClap = String.fromEnvironment('EXPECT_CLAP');

void main() {
  test('CLAP C ABI is exported and agrees with the build', () {
    final supported = native.soloud_clapIsSupported();
    expect(supported, anyOf(0, 1));
    if (_expectClap.isNotEmpty) {
      expect(supported, _expectClap == '1' ? 1 : 0);
    }

    using((arena) {
      final major = arena<UnsignedInt>()..value = 99;
      final minor = arena<UnsignedInt>()..value = 99;
      final revision = arena<UnsignedInt>()..value = 99;
      native.soloud_clapGetHostClapVersion(major, minor, revision);
      if (supported == 1) {
        expect(major.value, 1);
        expect(minor.value, greaterThanOrEqualTo(2));
      } else {
        expect([major.value, minor.value, revision.value], [0, 0, 0]);
      }
    });

    // Null outputs are skipped, and unloading with nothing loaded is a no-op.
    native.soloud_clapGetHostClapVersion(nullptr, nullptr, nullptr);
    native.soloud_clapHostUnloadAll();
  });
}
