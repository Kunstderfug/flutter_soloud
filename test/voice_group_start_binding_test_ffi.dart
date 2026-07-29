import 'dart:ffi' as ffi;

import 'package:flutter_soloud/src/bindings/bindings_player_ffi.dart';
import 'package:flutter_soloud/src/enums.dart';
import 'package:flutter_soloud/src/sound_handle.dart';
import 'package:test/test.dart';

int _group = 0;
int _main = 0;
int _count = 0;
double _deadline = 0;
int _nativeResult = 0;

int _scheduleVoiceGroupStartAt(
  int group,
  int main,
  int count,
  double deadline,
) {
  _group = group;
  _main = main;
  _count = count;
  _deadline = deadline;
  return _nativeResult;
}

final ffi.Pointer<
  ffi.NativeFunction<
    ffi.Int32 Function(ffi.UnsignedInt, ffi.UnsignedInt, ffi.Int32, ffi.Double)
  >
>
_scheduleVoiceGroupStartAtPointer = ffi.Pointer.fromFunction(
  _scheduleVoiceGroupStartAt,
  2,
);

void registerVoiceGroupStartBindingTests() {
  test('FFI binding forwards group commit arguments and maps the result', () {
    _nativeResult = VoiceGroupStartResult.deadlineReached.value;
    final bindings = FlutterSoLoudFfi.fromLookup(<T extends ffi.NativeType>(
      String symbol,
    ) {
      expect(symbol, 'scheduleVoiceGroupStartAt');
      return _scheduleVoiceGroupStartAtPointer.cast<T>();
    });

    final result = bindings.scheduleVoiceGroupStartAt(
      const SoundHandle(0xfffff001),
      const SoundHandle(17),
      3,
      const Duration(microseconds: 1250000),
    );

    expect(result, VoiceGroupStartResult.deadlineReached);
    expect(_group, 0xfffff001);
    expect(_main, 17);
    expect(_count, 3);
    expect(_deadline, 1.25);
  });
}
