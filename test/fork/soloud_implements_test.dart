// fork: GPhil mocks SoLoud with mocktail (`class MockSoLoud extends Mock
// implements SoLoud`) and stubs fork members on it. This file only compiles
// while the fork members are part of SoLoud's public interface, and while a
// class in another library can implement SoLoud without private members.
// See FORK_MAINTENANCE.md.
import 'package:flutter_soloud/flutter_soloud.dart';
import 'package:test/test.dart';

/// Stands in for a mocktail mock: every member it does not override goes to
/// [noSuchMethod], like `Mock` does.
class _FakeSoLoud implements SoLoud {
  final calls = <String>[];

  @override
  bool requestAudioDeviceIdleStop() {
    calls.add('requestAudioDeviceIdleStop');
    return true;
  }

  @override
  VoiceGroupStartResult scheduleVoiceGroupStartAt(
    SoundHandle voiceGroupHandle,
    SoundHandle requiredMain,
    int expectedMemberCount,
    Duration engineDeadline,
  ) {
    calls.add('scheduleVoiceGroupStartAt');
    return VoiceGroupStartResult.deadlineReached;
  }

  @override
  dynamic noSuchMethod(Invocation invocation) {
    calls.add(invocation.memberName.toString());
    return invocation.isGetter ? false : null;
  }
}

void main() {
  test('a class implementing SoLoud can stub the fork members', () {
    final fake = _FakeSoLoud();
    final SoLoud soloud = fake;

    expect(soloud.requestAudioDeviceIdleStop(), isTrue);
    expect(
      soloud.scheduleVoiceGroupStartAt(
        const SoundHandle(1),
        const SoundHandle(2),
        2,
        const Duration(seconds: 1),
      ),
      VoiceGroupStartResult.deadlineReached,
    );
    expect(fake.calls, [
      'requestAudioDeviceIdleStop',
      'scheduleVoiceGroupStartAt',
    ]);
  });

  test('the other fork members are on the SoLoud interface', () {
    final SoLoud soloud = _FakeSoLoud();

    // Each tear-off compiles only while the member is declared on SoLoud.
    final members = <Object>[
      soloud.listCaptureDevices,
      soloud.startCapture,
      soloud.startCaptureAndPlay,
      soloud.stopCapture,
      soloud.cancelCapture,
      soloud.captureClockSnapshot,
      soloud.captureLevelSnapshot,
    ];
    expect(members, everyElement(isA<Function>()));
    expect(soloud.isCaptureRecording, isFalse);
  });
}
