import 'dart:ffi';

import 'package:ffi/ffi.dart';
import 'package:flutter_soloud/src/bindings/flutter_soloud_ffigen.dart'
    as native;
import 'package:test/test.dart';

/// Whether this run's native build should have CLAP hosting compiled in:
/// `--dart-define=EXPECT_CLAP=1` when the root package enables `enable_clap`
/// on macOS. Unset, the result only has to be self-consistent.
const _expectClap = String.fromEnvironment('EXPECT_CLAP');

// Only calls that return before the host marshals to the main thread: a
// plain `dart test` process need not service the main dispatch queue. Real
// bundles are covered by test/run_clap_host_test.sh.
void main() {
  final supported = native.soloud_clapIsSupported() == 1;
  // What a rejected call returns: invalid arguments when CLAP is compiled
  // in, and notImplemented from the stubs.
  final rejected = supported
      ? native.PlayerErrors.invalidParameter
      : native.PlayerErrors.notImplemented;

  test('CLAP C ABI is exported and agrees with the build', () {
    expect(native.soloud_clapIsSupported(), anyOf(0, 1));
    if (_expectClap.isNotEmpty) {
      expect(supported, _expectClap == '1');
    }

    using((arena) {
      final major = arena<UnsignedInt>()..value = 99;
      final minor = arena<UnsignedInt>()..value = 99;
      final revision = arena<UnsignedInt>()..value = 99;
      native.soloud_clapGetHostClapVersion(major, minor, revision);
      if (supported) {
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

  test('scan clears its outputs and rejects a missing path', () {
    using((arena) {
      final count = arena<Int>()..value = 7;
      final arrays = List.generate(
        5,
        (_) => arena<Pointer<Pointer<Char>>>()
          ..value = Pointer<Pointer<Char>>.fromAddress(0x10),
      );
      final error = native.soloud_clapScanBundle(
        nullptr,
        count,
        arrays[0],
        arrays[1],
        arrays[2],
        arrays[3],
        arrays[4],
      );
      expect(
        error,
        rejected,
      );
      expect(count.value, 0);
      for (final array in arrays) {
        expect(array.value, nullptr);
      }
      // Freeing nothing, or null arrays, is safe.
      native.soloud_clapFreeScanResults(
        0,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
      );
    });
  });

  test('load validates its arguments and zeroes the handle', () {
    using((arena) {
      final handle = arena<Int>()..value = 42;
      final path = '/nonexistent/Mock.clap'.toNativeUtf8(allocator: arena);
      final id = 'mock.stereo'.toNativeUtf8(allocator: arena);
      final error = native.soloud_clapLoadEffect(
        path.cast(),
        id.cast(),
        0, // An invalid sample rate fails before any file access.
        512,
        2,
        handle,
      );
      expect(
        error,
        rejected,
      );
      expect(handle.value, 0);
      if (supported) {
        expect(
          native.soloud_clapLoadEffect(
            path.cast(),
            id.cast(),
            48000,
            512,
            2,
            nullptr,
          ),
          native.PlayerErrors.invalidParameter,
        );
      }
    });
  });

  test('unload of an unknown handle and the DSP seam', () {
    expect(
      native.soloud_clapUnloadPlugin(12345),
      rejected,
    );
    final callback = native.soloud_clapGetFilterCallbackPtr();
    expect(callback == nullptr, !supported);
    expect(native.soloud_clapGetPluginUserData(0), nullptr);
    expect(native.soloud_clapGetPluginUserData(-3), nullptr);
    expect(
      native.soloud_clapGetPluginUserData(1029).address,
      supported ? 1029 : 0,
    );
  });
}
