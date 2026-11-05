// ignore_for_file: public_member_api_docs

import 'package:flutter_soloud/src/filters/filters.dart';
import 'package:flutter_soloud/src/sound_handle.dart';
import 'package:flutter_soloud/src/sound_hash.dart';

/// Classical passage lift controls. Rise/fall are time per 10 dB of gain.
enum ClassicalLeveler {
  wet,
  liftPercent,
  thresholdLufs,
  floorLufs,
  riseTimeS,
  fallTimeMs,
  downwardEnabled,
  downwardThresholdLufs,
  downwardRatio;

  double get min => const <double>[0, 0, -45, -80, 1, 100, 0, -12, 1.1][index];
  double get max => const <double>[1, 100, -15, -50, 20, 2000, 1, 0, 2][index];
  double get def => const <double>[0, 50, -30, -60, 4, 500, 0, -10, 1.3][index];
}

abstract class _ClassicalLevelerInternal extends FilterBase {
  const _ClassicalLevelerInternal(SoundHash? soundHash, int? busId)
    : super(FilterType.classicalLevelerFilter, soundHash, busId);

  FilterParam _parameter(int index, {SoundHandle? soundHandle}) => FilterParam(
    soundHandle, busId, filterType, index,
    index < 9 ? ClassicalLeveler.values[index].min : -200,
    index < 9 ? ClassicalLeveler.values[index].max : 100,
  );

  /// Commits an entire look-ahead plan together. Refresh at least every 100 ms;
  /// stale plans automatically fall back to the real-time loudness detector.
  /// [lufs] contains up to 21 upcoming source-bin loudness values. Arrival
  /// [leadSeconds] are output time relative to this call. No audio is delayed.
  /// Use [snap] after seek, loop, handoff or tempo replacement.
  void setLookAheadPlan({
    required double nowLufs,
    required List<double> lufs,
    required List<double> leadSeconds,
    bool snap = false,
    SoundHandle? soundHandle,
  }) {
    if (lufs.isEmpty || lufs.length > 21 || lufs.length != leadSeconds.length ||
        nowLufs.isNaN || nowLufs == double.infinity ||
        lufs.any((v) => v.isNaN || v == double.infinity) ||
        leadSeconds.any((v) => !v.isFinite || v < 0 || v > 100)) {
      throw ArgumentError('Invalid Classical Leveler look-ahead plan');
    }
    double loudness(double v) => v.clamp(-200, 100).toDouble();
    void put(int index, double value) {
      _parameter(index, soundHandle: soundHandle).value = value;
    }
    put(11, loudness(nowLufs));
    put(12, lufs.length.toDouble());
    put(13, snap ? 1 : 0);
    for (var i = 0; i < lufs.length; ++i) {
      put(14 + i, loudness(lufs[i]));
      put(35 + i, leadSeconds[i]);
    }
    put(56, 1);
  }

  /// Immediately withdraws the prepared plan, keeping detector/gain history.
  void clearLookAheadPlan({SoundHandle? soundHandle}) {
    _parameter(12, soundHandle: soundHandle).value = 0;
    _parameter(56, soundHandle: soundHandle).value = 1;
  }

  ClassicalLeveler get queryWet => ClassicalLeveler.wet;

  ClassicalLeveler get queryLiftPercent => ClassicalLeveler.liftPercent;

  ClassicalLeveler get queryThresholdLufs => ClassicalLeveler.thresholdLufs;

  ClassicalLeveler get queryFloorLufs => ClassicalLeveler.floorLufs;

  ClassicalLeveler get queryRiseTimeS => ClassicalLeveler.riseTimeS;

  ClassicalLeveler get queryFallTimeMs => ClassicalLeveler.fallTimeMs;

  ClassicalLeveler get queryDownwardEnabled => ClassicalLeveler.downwardEnabled;

  ClassicalLeveler get queryDownwardThresholdLufs => ClassicalLeveler.downwardThresholdLufs;

  ClassicalLeveler get queryDownwardRatio => ClassicalLeveler.downwardRatio;
}

class ClassicalLevelerSingle extends _ClassicalLevelerInternal {
  ClassicalLevelerSingle(super.soundHash, super.busId);

  FilterParam wet({SoundHandle? soundHandle}) =>
      _parameter(0, soundHandle: soundHandle);

  FilterParam liftPercent({SoundHandle? soundHandle}) =>
      _parameter(1, soundHandle: soundHandle);

  FilterParam thresholdLufs({SoundHandle? soundHandle}) =>
      _parameter(2, soundHandle: soundHandle);

  FilterParam floorLufs({SoundHandle? soundHandle}) =>
      _parameter(3, soundHandle: soundHandle);

  FilterParam riseTimeS({SoundHandle? soundHandle}) =>
      _parameter(4, soundHandle: soundHandle);

  FilterParam fallTimeMs({SoundHandle? soundHandle}) =>
      _parameter(5, soundHandle: soundHandle);

  FilterParam downwardEnabled({SoundHandle? soundHandle}) =>
      _parameter(6, soundHandle: soundHandle);

  FilterParam downwardThresholdLufs({SoundHandle? soundHandle}) =>
      _parameter(7, soundHandle: soundHandle);

  FilterParam downwardRatio({SoundHandle? soundHandle}) =>
      _parameter(8, soundHandle: soundHandle);

  double appliedGainDb({SoundHandle? soundHandle}) =>
      _parameter(9, soundHandle: soundHandle).value;
  bool lookAheadActive({SoundHandle? soundHandle}) =>
      _parameter(10, soundHandle: soundHandle).value == 1;
}

class ClassicalLevelerGlobal extends _ClassicalLevelerInternal {
  const ClassicalLevelerGlobal() : super(null, null);

  FilterParam get wet => _parameter(0);

  FilterParam get liftPercent => _parameter(1);

  FilterParam get thresholdLufs => _parameter(2);

  FilterParam get floorLufs => _parameter(3);

  FilterParam get riseTimeS => _parameter(4);

  FilterParam get fallTimeMs => _parameter(5);

  FilterParam get downwardEnabled => _parameter(6);

  FilterParam get downwardThresholdLufs => _parameter(7);

  FilterParam get downwardRatio => _parameter(8);

  double get appliedGainDb => _parameter(9).value;
  bool get lookAheadActive => _parameter(10).value == 1;
}
