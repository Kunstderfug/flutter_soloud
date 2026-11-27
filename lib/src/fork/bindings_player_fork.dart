// fork: GPhil's additions to the abstract bindings: native capture, the
// domain idle stop and atomic voice-group start. FlutterSoLoudFfi gets them
// from fork/bindings_ffi_fork.dart and FlutterSoLoudWeb from
// fork/bindings_player_web_fork.dart. See FORK_MAINTENANCE.md.
part of '../bindings/bindings_player.dart';

/// The fork's members of [FlutterSoLoud].
mixin _FlutterSoLoudForkApi {
  /// Queue a domain-idle stop without waiting for native device operations.
  /// Returns false if busy, uninitialized, or the idle policy is indefinite.
  @mustBeOverridden
  bool requestAudioDeviceIdleStop();

  /// List available capture/input devices.
  List<CaptureDevice> listCaptureDevices();

  /// Atomically schedule every prepared member of [voiceGroupHandle] at one
  /// absolute [engineDeadline].
  VoiceGroupStartResult scheduleVoiceGroupStartAt(
    SoundHandle voiceGroupHandle,
    SoundHandle requiredMain,
    int expectedMemberCount,
    Duration engineDeadline,
  );

  /// Start native miniaudio input capture to a WAV file.
  @mustBeOverridden
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
  );

  /// Start native capture and a SoLoud voice from one command.
  @mustBeOverridden
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
  });

  /// Stop the active native miniaudio capture session.
  @mustBeOverridden
  ({PlayerErrors error, SoLoudCaptureStopResult? result}) stopCapture();

  /// Cancel capture and delete the partial WAV file.
  @mustBeOverridden
  PlayerErrors cancelCapture();

  /// Whether native miniaudio capture is currently recording.
  @mustBeOverridden
  bool isCaptureRecording();

  /// Same-clock snapshot from the active capture session.
  @mustBeOverridden
  ({PlayerErrors error, SoLoudCaptureClockSnapshot? result})
  getCaptureClockSnapshot();

  /// Live input-level snapshot from the active capture session.
  @mustBeOverridden
  ({PlayerErrors error, SoLoudCaptureLevelSnapshot? result})
  getCaptureLevelSnapshot();
}
