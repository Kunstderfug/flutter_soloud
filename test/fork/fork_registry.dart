// The fork's entries in the upstream registry tests of test/common. Each of
// those tests spreads them in with one line marked `// fork:`, so a merge
// with upstream only ever conflicts on that line. See FORK_MAINTENANCE.md.
//
// The FFI and web symbol lists, which must stay free of Flutter imports, are
// in fork_symbols.dart.

import 'package:flutter_soloud/flutter_soloud.dart';
import 'package:flutter_soloud/src/filters/filters.dart';

// ---------------------------------------------------------------------------
// test/common/api_before_init_test.dart

/// Fork members of SoLoud that throw SoLoudNotInitializedException before
/// init, with a call that reaches the check.
final Map<String, Object? Function(SoLoud s, AudioSource sound, SoundHandle h)>
forkBeforeInitCalls = {
  // Native input capture.
  'startCapture': (s, _, _) => s.startCapture('capture.wav'),
  'startCaptureAndPlay': (s, a, _) => s.startCaptureAndPlay('capture.wav', a),
};

/// Fork members of SoLoud that do not throw before init, with the reason.
const forkBeforeInitExempt = {
  'get engineSession': 'a Dart-side counter, readable at any time',
  'requestAudioDeviceIdleStop': 'documented: false before init',
  'scheduleVoiceGroupStartAt':
      'reports voiceGroupStartBackendNotInitialized in its result',
  'listCaptureDevices': 'the capture device is separate from the engine',
  'get isCaptureRecording': 'false without a capture session',
  'cancelCapture': 'throws its own error without a capture session',
  'stopCapture': 'throws its own error without a capture session',
  'captureClockSnapshot': 'throws its own error without a capture session',
  'captureLevelSnapshot': 'throws its own error without a capture session',
};

// ---------------------------------------------------------------------------
// test/common/filters_api_test.dart

/// Fork filters whose user parameters are fewer than
/// [FilterType.numParameters]: the Classical Leveler's meter and look-ahead
/// plan slots are not user parameters.
const forkFiltersWithHiddenParams = {FilterType.classicalLevelerFilter};

/// Builds a parameter entry of filters_api_test.dart. [other] is a
/// non-default value for a parameter that accepts only some values in its
/// range.
typedef ForkFilterParamFactory<P> =
    P Function(
      String name,
      double min,
      double max,
      double def,
      FilterParam Function() global,
      FilterParam Function(FiltersSingle f, SoundHandle h) on, {
      double? other,
    });

/// Builds a filter entry of filters_api_test.dart.
typedef ForkFilterFactory<F, P> =
    F Function(
      FilterType type,
      FilterBase global,
      FilterBase Function(FiltersSingle filters) single,
      List<P> params,
    );

/// The fork's filters, in the shape of filters_api_test.dart's `_filters`.
List<F> forkFilters<F, P>(
  ForkFilterParamFactory<P> param,
  ForkFilterFactory<F, P> filter,
) {
  final globals = SoLoud.instance.filters;
  return [
    filter(
      FilterType.classicalLevelerFilter,
      globals.classicalLevelerFilter,
      (f) => f.classicalLevelerFilter,
      [
        for (final e in ClassicalLeveler.values)
          param(
            e.name,
            e.min,
            e.max,
            e.def,
            () => switch (e) {
              ClassicalLeveler.wet => globals.classicalLevelerFilter.wet,
              ClassicalLeveler.liftPercent =>
                globals.classicalLevelerFilter.liftPercent,
              ClassicalLeveler.thresholdLufs =>
                globals.classicalLevelerFilter.thresholdLufs,
              ClassicalLeveler.floorLufs =>
                globals.classicalLevelerFilter.floorLufs,
              ClassicalLeveler.riseTimeS =>
                globals.classicalLevelerFilter.riseTimeS,
              ClassicalLeveler.fallTimeMs =>
                globals.classicalLevelerFilter.fallTimeMs,
              ClassicalLeveler.downwardEnabled =>
                globals.classicalLevelerFilter.downwardEnabled,
              ClassicalLeveler.downwardThresholdLufs =>
                globals.classicalLevelerFilter.downwardThresholdLufs,
              ClassicalLeveler.downwardRatio =>
                globals.classicalLevelerFilter.downwardRatio,
            },
            (f, h) => switch (e) {
              ClassicalLeveler.wet => f.classicalLevelerFilter.wet(
                soundHandle: h,
              ),
              ClassicalLeveler.liftPercent =>
                f.classicalLevelerFilter.liftPercent(soundHandle: h),
              ClassicalLeveler.thresholdLufs =>
                f.classicalLevelerFilter.thresholdLufs(soundHandle: h),
              ClassicalLeveler.floorLufs => f.classicalLevelerFilter.floorLufs(
                soundHandle: h,
              ),
              ClassicalLeveler.riseTimeS => f.classicalLevelerFilter.riseTimeS(
                soundHandle: h,
              ),
              ClassicalLeveler.fallTimeMs =>
                f.classicalLevelerFilter.fallTimeMs(soundHandle: h),
              ClassicalLeveler.downwardEnabled =>
                f.classicalLevelerFilter.downwardEnabled(soundHandle: h),
              ClassicalLeveler.downwardThresholdLufs =>
                f.classicalLevelerFilter.downwardThresholdLufs(soundHandle: h),
              ClassicalLeveler.downwardRatio =>
                f.classicalLevelerFilter.downwardRatio(soundHandle: h),
            },
            // downwardEnabled is a switch: the native filter refuses 0.5.
            other: e == ClassicalLeveler.downwardEnabled ? 1 : null,
          ),
      ],
    ),
  ];
}
