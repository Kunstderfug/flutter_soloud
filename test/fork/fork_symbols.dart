// The fork's native exports and how the web bindings treat each one. Used by
// test/common/web_bindings_sync_test.dart, which spreads [forkNotBoundOnWeb]
// into its own list and runs [forkWebSymbolContract] with the export list it
// computes. Plain Dart (no Flutter imports), as that test is. See
// FORK_MAINTENANCE.md.

import 'dart:io';

import 'package:test/test.dart';

/// Fork exports that the web build compiles but the web bindings do not
/// call, with the reason (web export names, with the leading `_`).
const forkNotBoundOnWeb = {
  // Native input capture: the web bindings return notImplemented.
  '_cancelCapture': 'native capture only (web: notImplemented)',
  '_freeListCaptureDevices': 'native capture only (web: notImplemented)',
  '_getCaptureClockSnapshot': 'native capture only (web: notImplemented)',
  '_getCaptureLevelSnapshot': 'native capture only (web: notImplemented)',
  '_isCaptureRecording': 'native capture only (web: notImplemented)',
  '_listCaptureDevices': 'native capture only (web: notImplemented)',
  '_startCapture': 'native capture only (web: notImplemented)',
  '_startCaptureAndPlay': 'native capture only (web: notImplemented)',
  '_stopCapture': 'native capture only (web: notImplemented)',
  '_requestAudioDeviceIdleStop': 'the device always runs on the web',
};

/// The fork's FFI symbols (C names, as bound in the ffigen output) other
/// than CLAP, which [forkFfiSymbolPrefixes] covers.
const forkFfiSymbols = {
  // Native input capture.
  'cancelCapture',
  'freeListCaptureDevices',
  'getCaptureClockSnapshot',
  'getCaptureLevelSnapshot',
  'isCaptureRecording',
  'listCaptureDevices',
  'startCapture',
  'startCaptureAndPlay',
  'stopCapture',
  // Device idle stop.
  'requestAudioDeviceIdleStop',
  // Atomic voice-group start (bound on the web).
  'scheduleVoiceGroupStartAt',
};

/// Prefixes of fork FFI symbol families, with the reason none of them is
/// compiled for the web.
const forkFfiSymbolPrefixes = {
  'soloud_clap':
      'CLAP hosting is desktop only (#ifndef __EMSCRIPTEN__ in src/bindings.h)',
};

const _generatedPath = 'lib/src/bindings/flutter_soloud_ffigen.dart';

/// The C symbols of every `@ffi.Native` external function in the ffigen
/// output.
Set<String> _ffigenSymbols(String dart) {
  final out = <String>{};
  for (final chunk in dart.split('@ffi.Native<').skip(1)) {
    final external = RegExp(
      r'external\s+[^;(]*?\b(\w+)\s*\(',
    ).firstMatch(chunk);
    if (external == null) continue;
    final symbol = RegExp(
      r"symbol:\s*'(\w+)'",
    ).firstMatch(chunk.substring(0, external.start));
    out.add(symbol?[1] ?? external[1]!);
  }
  return out;
}

/// `FFI_PLUGIN_EXPORT <ret> name(` in the C [code], comments removed.
Set<String> _cExports(String code) => {
  for (final m in RegExp(r'FFI_PLUGIN_EXPORT\b[^;{(]*?\b(\w+)\s*\(').allMatches(
    code
        .replaceAll(RegExp(r'/\*.*?\*/', dotAll: true), '')
        .replaceAll(RegExp('//.*'), ''),
  ))
    m[1]!,
};

/// The headers under src/fork/ that ffigen.yaml lists as entry points.
List<File> _forkEntryPoints() => [
  for (final m in RegExp(
    r"""^\s*-\s*['"]?(src/fork/[^'"\s]+\.h)['"]?\s*$""",
    multiLine: true,
  ).allMatches(File('ffigen.yaml').readAsStringSync()))
    File(m[1]!),
];

/// Every fork FFI symbol is bound on the web, probed with `wasmHasExport`,
/// listed in [forkNotBoundOnWeb], or in a family of [forkFfiSymbolPrefixes]
/// that the web build does not compile. [allExports], [bound] and [probed]
/// are the web export names (with `_`) computed by web_bindings_sync_test.
void forkWebSymbolContract({
  required Set<String> allExports,
  required Set<String> bound,
  required Set<String> probed,
}) {
  group('fork FFI symbols on the web', () {
    final ffigen = _ffigenSymbols(
      File(_generatedPath).readAsStringSync().replaceAll('\r\n', '\n'),
    );
    final prefixed = {
      for (final s in ffigen)
        if (forkFfiSymbolPrefixes.keys.any(s.startsWith)) s,
    };
    final fork = {...forkFfiSymbols, ...prefixed};

    test('the ffigen output parses', () {
      expect(ffigen.length, greaterThan(100));
      expect(ffigen, contains('scheduleVoiceGroupStartAt'));
    });

    test('every listed fork symbol is bound by ffigen', () {
      expect(
        forkFfiSymbols.difference(ffigen),
        isEmpty,
        reason:
            'Listed in forkFfiSymbols but not in $_generatedPath: renamed or '
            'removed? Update test/fork/fork_symbols.dart.',
      );
      for (final prefix in forkFfiSymbolPrefixes.keys) {
        expect(
          prefixed.where((s) => s.startsWith(prefix)),
          isNotEmpty,
          reason: 'no $prefix* symbol in $_generatedPath',
        );
      }
    });

    test('every fork header export is listed', () {
      final listed = <String>{};
      for (final header in _forkEntryPoints()) {
        listed.addAll(_cExports(header.readAsStringSync()));
      }
      final unlisted =
          listed
              .where((s) => !_inPrefixedFamily(s))
              .toSet()
              .difference(forkFfiSymbols)
              .toList()
            ..sort();
      expect(
        unlisted,
        isEmpty,
        reason:
            'Exported by a src/fork/ header in ffigen.yaml but not in '
            'forkFfiSymbols (test/fork/fork_symbols.dart).',
      );
    });

    test('every fork symbol is bound on the web or excluded with a reason', () {
      final excluded = forkNotBoundOnWeb.keys.toSet();
      final missing = <String>[
        for (final s in fork)
          if (!_inPrefixedFamily(s) &&
              !bound.contains('_$s') &&
              !probed.contains('_$s') &&
              !excluded.contains('_$s'))
            s,
      ]..sort();
      expect(
        missing,
        isEmpty,
        reason:
            'Bind it in lib/src/bindings/js_extension.dart, or add it to '
            'forkNotBoundOnWeb with the reason.',
      );
    });

    test('the desktop-only fork families are not compiled for the web', () {
      final compiled = [
        for (final s in prefixed)
          if (allExports.contains('_$s')) s,
      ]..sort();
      expect(
        compiled,
        isEmpty,
        reason:
            'Now exported for the web: bind it or list it in '
            'forkNotBoundOnWeb, and update forkFfiSymbolPrefixes.',
      );
    });

    test('forkNotBoundOnWeb names fork symbols only', () {
      expect(
        forkNotBoundOnWeb.keys
            .map((n) => n.substring(1))
            .toSet()
            .difference(forkFfiSymbols),
        isEmpty,
      );
    });
  });
}

/// Whether [symbol] belongs to a family of [forkFfiSymbolPrefixes].
bool _inPrefixedFamily(String symbol) =>
    forkFfiSymbolPrefixes.keys.any(symbol.startsWith);
