import 'package:flutter_soloud/src/enums.dart';
import 'package:test/test.dart';

void registerVoiceGroupStartBindingTests() {
  test('maps native voice-group start integers to public results', () {
    expect(
      VoiceGroupStartResult.fromValue(0),
      VoiceGroupStartResult.success,
    );
    expect(
      VoiceGroupStartResult.fromValue(
        VoiceGroupStartResult.deadlineReached.value,
      ),
      VoiceGroupStartResult.deadlineReached,
    );
  });
}
