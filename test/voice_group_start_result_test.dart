import 'package:flutter_soloud/flutter_soloud.dart';
import 'package:test/test.dart';

void main() {
  test(
    'VoiceGroupStartResult maps every native value through the public API',
    () {
      for (final result in VoiceGroupStartResult.values) {
        expect(VoiceGroupStartResult.fromValue(result.value), same(result));
      }
    },
  );

  test('VoiceGroupStartResult rejects an unknown native value', () {
    expect(
      () =>
          VoiceGroupStartResult.fromValue(VoiceGroupStartResult.values.length),
      throwsArgumentError,
    );
  });
}
