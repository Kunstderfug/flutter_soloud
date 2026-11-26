// fork: the code of GPhil's additions to `SoLoud`: native capture, the
// domain idle stop, atomic voice-group start and the engine session counter.
// The public members, with their docs, are declared in soloud.dart (the block
// between `// fork: begin` and `// fork: end`) and forward here, so upstream
// merges touch only that block and the other `// fork:` hook lines.
// See FORK_MAINTENANCE.md.
part of '../soloud.dart';

/// The state and code behind the fork's members of [SoLoud].
///
/// Library-private, so the package exports no new type, and it declares no
/// public member: the public API stays declared on [SoLoud] itself, which is
/// what `implements SoLoud` (mocks included) and dartdoc see.
mixin _SoLoudForkApi {
  /// The [SoLoud] this mixin is applied to: the fork code reaches the
  /// engine's private state through it.
  SoLoud get _soloud => this as SoLoud;

  /// Counts the engine teardowns of this isolate, including the one an
  /// `init` runs after a hot restart.
  int _engineSession = 0;

  /// Path of the active native miniaudio capture, if any.
  String? _capturePath;
  String? _captureMirrorPath;

  bool _forkRequestAudioDeviceIdleStop() {
    if (!_soloud.isInitialized) {
      return false;
    }
    return _soloud._controller.soLoudFFI.requestAudioDeviceIdleStop();
  }

  List<CaptureDevice> _forkListCaptureDevices() {
    return _soloud._controller.soLoudFFI.listCaptureDevices();
  }

  bool get _forkIsCaptureRecording =>
      _soloud._controller.soLoudFFI.isCaptureRecording();

  SoLoudCaptureStartResult _forkStartCapture(
    String path, {
    required int sampleRate,
    required Channels channels,
    required int bufferSizeFrames,
    required double inputGainDb,
    required CaptureDevice? device,
    required String? mirrorPath,
    required SoLoudCaptureMirrorFormat mirrorFormat,
    required int mirrorBitsPerSample,
  }) {
    if (!_soloud.isInitialized) {
      throw const SoLoudNotInitializedException();
    }
    final ret = _soloud._controller.soLoudFFI.startCapture(
      path,
      sampleRate,
      channels.count,
      bufferSizeFrames,
      inputGainDb,
      device,
      mirrorPath,
      mirrorFormat,
      mirrorBitsPerSample,
    );
    _soloud._logPlayerError(ret.error, from: 'startCapture() result');
    if (ret.error != PlayerErrors.noError || ret.result == null) {
      throw SoLoudCppException.fromPlayerError(ret.error);
    }
    _capturePath = path;
    final result = ret.result!;
    _captureMirrorPath = result.mirrorActive ? mirrorPath : null;
    return result;
  }

  SoLoudCapturePlaybackStartResult _forkStartCaptureAndPlay(
    String path,
    AudioSource sound, {
    required int busId,
    required int sampleRate,
    required Channels channels,
    required int bufferSizeFrames,
    required double volume,
    required double pan,
    required Duration startAt,
    required bool looping,
    required Duration loopingStartAt,
    required double inputGainDb,
    required CaptureDevice? device,
    required String? mirrorPath,
    required SoLoudCaptureMirrorFormat mirrorFormat,
    required int mirrorBitsPerSample,
  }) {
    if (!_soloud.isInitialized) {
      throw const SoLoudNotInitializedException();
    }
    final ret = _soloud._controller.soLoudFFI.startCaptureAndPlay(
      path,
      sound.soundHash,
      busId: busId,
      sampleRate: sampleRate,
      channels: channels.count,
      bufferSizeFrames: bufferSizeFrames,
      volume: volume,
      pan: pan,
      startAt: startAt,
      looping: looping,
      loopingStartAt: loopingStartAt,
      inputGainDb: inputGainDb,
      device: device,
      mirrorPath: mirrorPath,
      mirrorFormat: mirrorFormat,
      mirrorBitsPerSample: mirrorBitsPerSample,
    );
    _soloud._logPlayerError(ret.error, from: 'startCaptureAndPlay() result');
    if (ret.error != PlayerErrors.noError || ret.result == null) {
      throw SoLoudCppException.fromPlayerError(ret.error);
    }

    final result = ret.result!;
    _soloud._registerHandle(
      sound,
      SoundHandle(result.handle),
      from: 'startCaptureAndPlay()',
    );

    _capturePath = path;
    _captureMirrorPath = result.mirrorActive ? mirrorPath : null;
    return result;
  }

  SoLoudCaptureStopResult _forkStopCapture() {
    final ret = _soloud._controller.soLoudFFI.stopCapture();
    _soloud._logPlayerError(ret.error, from: 'stopCapture() result');
    if (ret.error != PlayerErrors.noError || ret.result == null) {
      throw SoLoudCppException.fromPlayerError(ret.error);
    }
    final result = ret.result!;
    final path = _capturePath ?? result.path;
    final mirrorPath = _captureMirrorPath ?? result.mirrorPath;
    _capturePath = null;
    _captureMirrorPath = null;
    return SoLoudCaptureStopResult(
      path: path,
      sampleRate: result.sampleRate,
      channels: result.channels,
      frameCount: result.frameCount,
      duration: result.duration,
      sessionStartHostTimeNanos: result.sessionStartHostTimeNanos,
      captureStartHostTimeNanos: result.captureStartHostTimeNanos,
      firstInputBufferHostTimeNanos: result.firstInputBufferHostTimeNanos,
      firstInputBufferFrameIndex: result.firstInputBufferFrameIndex,
      captureStopHostTimeNanos: result.captureStopHostTimeNanos,
      mirrorPath: mirrorPath,
      mirrorFormat: result.mirrorFormat,
      mirrorSucceeded: result.mirrorSucceeded,
      mirrorFrameCount: result.mirrorFrameCount,
      writerOverflowFrames: result.writerOverflowFrames,
      writerSilenceFrames: result.writerSilenceFrames,
      writerFailed: result.writerFailed,
    );
  }

  void _forkCancelCapture() {
    final ret = _soloud._controller.soLoudFFI.cancelCapture();
    _soloud._logPlayerError(ret, from: 'cancelCapture() result');
    if (ret != PlayerErrors.noError) {
      throw SoLoudCppException.fromPlayerError(ret);
    }
    _capturePath = null;
    _captureMirrorPath = null;
  }

  SoLoudCaptureClockSnapshot _forkCaptureClockSnapshot() {
    final ret = _soloud._controller.soLoudFFI.getCaptureClockSnapshot();
    _soloud._logPlayerError(ret.error, from: 'captureClockSnapshot() result');
    if (ret.error != PlayerErrors.noError || ret.result == null) {
      throw SoLoudCppException.fromPlayerError(ret.error);
    }
    return ret.result!;
  }

  SoLoudCaptureLevelSnapshot _forkCaptureLevelSnapshot() {
    final ret = _soloud._controller.soLoudFFI.getCaptureLevelSnapshot();
    _soloud._logPlayerError(ret.error, from: 'captureLevelSnapshot() result');
    if (ret.error != PlayerErrors.noError || ret.result == null) {
      throw SoLoudCppException.fromPlayerError(ret.error);
    }
    return ret.result!;
  }

  VoiceGroupStartResult _forkScheduleVoiceGroupStartAt(
    SoundHandle voiceGroupHandle,
    SoundHandle requiredMain,
    int expectedMemberCount,
    Duration engineDeadline,
  ) {
    return _soloud._controller.soLoudFFI.scheduleVoiceGroupStartAt(
      voiceGroupHandle,
      requiredMain,
      expectedMemberCount,
      engineDeadline,
    );
  }

  /// Called by `SoLoud._predeinit` before it requests the engine shutdown.
  void _forkBeforeEngineShutdown() {
    _engineSession++;
  }

  /// Called at the end of `SoLoud._predeinit`: a capture session ends with
  /// the engine.
  void _forkAfterPredeinit() {
    if (_soloud._controller.soLoudFFI.isCaptureRecording()) {
      _soloud._controller.soLoudFFI.cancelCapture();
    }
    _capturePath = null;
    _captureMirrorPath = null;
  }
}
