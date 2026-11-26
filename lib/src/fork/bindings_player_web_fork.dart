// fork: the web side of GPhil's additions to the bindings. Capture and the
// domain idle stop are not available on the web; atomic voice-group start
// calls the WASM export. See FORK_MAINTENANCE.md.
part of '../bindings/bindings_player_web.dart';

/// The fork's overrides of [FlutterSoLoud] for the web.
mixin _FlutterSoLoudWebFork on FlutterSoLoud {
  @override
  bool requestAudioDeviceIdleStop() => false;

  @override
  List<CaptureDevice> listCaptureDevices() {
    return const [];
  }

  @override
  VoiceGroupStartResult scheduleVoiceGroupStartAt(
    SoundHandle voiceGroupHandle,
    SoundHandle requiredMain,
    int expectedMemberCount,
    Duration engineDeadline,
  ) {
    final result = wasmScheduleVoiceGroupStartAt(
      voiceGroupHandle.id,
      requiredMain.id,
      expectedMemberCount,
      engineDeadline.toDouble(),
    );
    return VoiceGroupStartResult.fromValue(result);
  }

  @override
  ({PlayerErrors error, SoLoudCaptureStartResult? result}) startCapture(
    String path,
    int sampleRate,
    int channels,
    int bufferSizeFrames,
    double inputGainDb,
    CaptureDevice? device,
    String? mirrorPath,
    SoLoudCaptureMirrorFormat mirrorFormat,
    int mirrorBitsPerSample,
  ) {
    return (error: PlayerErrors.notImplemented, result: null);
  }

  @override
  ({PlayerErrors error, SoLoudCapturePlaybackStartResult? result})
  startCaptureAndPlay(
    String path,
    SoundHash soundHash, {
    int busId = 0,
    int sampleRate = 48000,
    int channels = 2,
    int bufferSizeFrames = 256,
    double volume = 1,
    double pan = 0,
    Duration startAt = Duration.zero,
    bool looping = false,
    Duration loopingStartAt = Duration.zero,
    double inputGainDb = 0,
    CaptureDevice? device,
    String? mirrorPath,
    SoLoudCaptureMirrorFormat mirrorFormat = SoLoudCaptureMirrorFormat.none,
    int mirrorBitsPerSample = 0,
  }) {
    return (error: PlayerErrors.notImplemented, result: null);
  }

  @override
  ({PlayerErrors error, SoLoudCaptureStopResult? result}) stopCapture() {
    return (error: PlayerErrors.notImplemented, result: null);
  }

  @override
  PlayerErrors cancelCapture() => PlayerErrors.notImplemented;

  @override
  bool isCaptureRecording() => false;

  @override
  ({PlayerErrors error, SoLoudCaptureClockSnapshot? result})
  getCaptureClockSnapshot() {
    return (error: PlayerErrors.notImplemented, result: null);
  }

  @override
  ({PlayerErrors error, SoLoudCaptureLevelSnapshot? result})
  getCaptureLevelSnapshot() {
    return (error: PlayerErrors.notImplemented, result: null);
  }
}
