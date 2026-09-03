import 'package:flutter_soloud/src/bindings/bindings_player_web.dart';
import 'package:flutter_soloud/src/bindings/js_extension.dart';
import 'package:flutter_soloud/src/enums.dart';
import 'package:flutter_soloud/src/sound_handle.dart';
import 'package:test/test.dart';

void registerVoiceGroupStartBindingTests() {
  test('web binding forwards the group commit and maps the result', () {
    jsEval('''
      globalThis.Module_soloud = globalThis.Module_soloud || {};
      globalThis.Module_soloud._scheduleVoiceGroupStartAt =
          (group, main, count, deadline) => {
            globalThis.voiceGroupStartArgs =
                [group, main, count, deadline];
            return 8;
          };
    ''');
    final bindings = FlutterSoLoudWeb();

    final result = bindings.scheduleVoiceGroupStartAt(
      const SoundHandle(0xfffff001),
      const SoundHandle(17),
      3,
      const Duration(microseconds: 1250000),
    );

    expect(result, VoiceGroupStartResult.deadlineReached);
    jsEval('''
      if (globalThis.voiceGroupStartArgs[0] !== 4294963201 ||
          globalThis.voiceGroupStartArgs[1] !== 17 ||
          globalThis.voiceGroupStartArgs[2] !== 3 ||
          globalThis.voiceGroupStartArgs[3] !== 1.25) {
        throw new Error('unexpected atomic group start arguments');
      }
    ''');
  });
}
