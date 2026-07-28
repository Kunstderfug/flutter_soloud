/// Native capture mirror formats supported by SoLoud capture.
///
/// Encoder availability is build-dependent. Check
/// [SoLoudCaptureStartResult.mirrorActive] after starting capture. In
/// particular, the Apple Swift Package Manager wrapper omits WavPack because
/// it does not compile the vendored WavPack C target; Apple CocoaPods/CMake
/// builds include it.
enum SoLoudCaptureMirrorFormat {
  /// No native sidecar encoder.
  none,

  /// Native FLAC sidecar encoder.
  flac,

  /// Native WavPack sidecar encoder.
  wavPack,
}

/// Resolve a native integer mirror format value.
SoLoudCaptureMirrorFormat soLoudCaptureMirrorFormatFromValue(int value) {
  if (value < 0 || value >= SoLoudCaptureMirrorFormat.values.length) {
    return SoLoudCaptureMirrorFormat.none;
  }
  return SoLoudCaptureMirrorFormat.values[value];
}

/// Result returned after the native miniaudio capture device starts.
final class SoLoudCaptureStartResult {
  /// Create a native capture start result.
  const SoLoudCaptureStartResult({
    required this.path,
    required this.sampleRate,
    required this.channels,
    required this.bufferSizeFrames,
    required this.sessionStartHostTimeNanos,
    required this.captureStartHostTimeNanos,
    this.mirrorPath,
    this.mirrorFormat = SoLoudCaptureMirrorFormat.none,
    this.mirrorBitsPerSample = 0,
    this.mirrorActive = false,
  });

  /// Destination WAV path.
  final String path;

  /// Actual native capture sample rate.
  final int sampleRate;

  /// Actual native capture channel count.
  final int channels;

  /// Requested native period size.
  final int bufferSizeFrames;

  /// Monotonic clock timestamp used as this capture session's origin.
  final int sessionStartHostTimeNanos;

  /// Monotonic clock timestamp captured after the native device started.
  final int captureStartHostTimeNanos;

  /// Native sidecar mirror path, if requested.
  final String? mirrorPath;

  /// Native sidecar mirror format.
  final SoLoudCaptureMirrorFormat mirrorFormat;

  /// Native sidecar mirror bit depth.
  final int mirrorBitsPerSample;

  /// Whether native capture accepted and started the sidecar mirror.
  ///
  /// A requested encoder can be unavailable in a particular platform build;
  /// the WAV safety recording still starts and this value is then `false`.
  final bool mirrorActive;

  /// Capture-start timestamp expressed as a reusable clock snapshot.
  SoLoudCaptureClockSnapshot get captureStartClock =>
      SoLoudCaptureClockSnapshot(
        hostTimeNanos: captureStartHostTimeNanos,
        sessionStartHostTimeNanos: sessionStartHostTimeNanos,
        sampleRate: sampleRate,
        inputDeviceFrame: 0,
      );
}

/// Result returned after native capture and playback start together.
final class SoLoudCapturePlaybackStartResult {
  /// Create a coordinated native capture/playback start result.
  const SoLoudCapturePlaybackStartResult({
    required this.path,
    required this.handle,
    required this.sampleRate,
    required this.channels,
    required this.bufferSizeFrames,
    required this.sessionStartHostTimeNanos,
    required this.captureStartHostTimeNanos,
    required this.playbackStartHostTimeNanos,
    this.mirrorPath,
    this.mirrorFormat = SoLoudCaptureMirrorFormat.none,
    this.mirrorBitsPerSample = 0,
    this.mirrorActive = false,
  });

  /// Destination WAV path.
  final String path;

  /// New SoLoud playback handle.
  final int handle;

  /// Actual native capture sample rate.
  final int sampleRate;

  /// Actual native capture channel count.
  final int channels;

  /// Requested native period size.
  final int bufferSizeFrames;

  /// Monotonic clock timestamp used as this coordinated session's origin.
  final int sessionStartHostTimeNanos;

  /// Monotonic clock timestamp captured after the native capture device starts.
  final int captureStartHostTimeNanos;

  /// Monotonic clock timestamp captured after the SoLoud voice is unpaused.
  final int playbackStartHostTimeNanos;

  /// Native sidecar mirror path, if requested.
  final String? mirrorPath;

  /// Native sidecar mirror format.
  final SoLoudCaptureMirrorFormat mirrorFormat;

  /// Native sidecar mirror bit depth.
  final int mirrorBitsPerSample;

  /// Whether native capture accepted and started the sidecar mirror.
  ///
  /// A requested encoder can be unavailable in a particular platform build;
  /// the WAV safety recording still starts and this value is then `false`.
  final bool mirrorActive;

  /// Capture-start timestamp expressed as a reusable clock snapshot.
  SoLoudCaptureClockSnapshot get captureStartClock =>
      SoLoudCaptureClockSnapshot(
        hostTimeNanos: captureStartHostTimeNanos,
        sessionStartHostTimeNanos: sessionStartHostTimeNanos,
        sampleRate: sampleRate,
        inputDeviceFrame: 0,
      );

  /// Playback-start timestamp expressed as a reusable clock snapshot.
  SoLoudCaptureClockSnapshot get playbackStartClock =>
      SoLoudCaptureClockSnapshot(
        hostTimeNanos: playbackStartHostTimeNanos,
        sessionStartHostTimeNanos: sessionStartHostTimeNanos,
        sampleRate: sampleRate,
        inputDeviceFrame: 0,
      );
}

/// Result returned after the native miniaudio capture device stops.
final class SoLoudCaptureStopResult {
  /// Create a native capture stop result.
  const SoLoudCaptureStopResult({
    required this.path,
    required this.sampleRate,
    required this.channels,
    required this.frameCount,
    required this.duration,
    required this.sessionStartHostTimeNanos,
    required this.captureStartHostTimeNanos,
    required this.captureStopHostTimeNanos,
    this.firstInputBufferHostTimeNanos,
    this.firstInputBufferFrameIndex,
    this.mirrorPath,
    this.mirrorFormat = SoLoudCaptureMirrorFormat.none,
    this.mirrorSucceeded = false,
    this.mirrorFrameCount = 0,
    this.writerOverflowFrames = 0,
    this.writerSilenceFrames = 0,
    this.writerFailed = false,
  });

  /// Destination WAV path.
  final String path;

  /// Actual native capture sample rate.
  final int sampleRate;

  /// Actual native capture channel count.
  final int channels;

  /// Number of captured PCM frames.
  final int frameCount;

  /// Duration resolved from [frameCount] and [sampleRate].
  final Duration duration;

  /// Monotonic clock timestamp used as this capture session's origin.
  final int sessionStartHostTimeNanos;

  /// Monotonic clock timestamp captured after the native device started.
  final int captureStartHostTimeNanos;

  /// Monotonic clock timestamp of the first native input callback, if any.
  final int? firstInputBufferHostTimeNanos;

  /// Captured input frame index at the first native input callback.
  final int? firstInputBufferFrameIndex;

  /// Monotonic clock timestamp captured after the native device stopped.
  final int captureStopHostTimeNanos;

  /// Native sidecar mirror path, if one was requested.
  final String? mirrorPath;

  /// Native sidecar mirror format.
  final SoLoudCaptureMirrorFormat mirrorFormat;

  /// Whether the native sidecar mirror finalized successfully.
  final bool mirrorSucceeded;

  /// Number of frames accepted by the native sidecar mirror.
  final int mirrorFrameCount;

  /// Number of capture frames that could not fit in the writer ring.
  final int writerOverflowFrames;

  /// Number of silence frames inserted to preserve duration after overflow.
  final int writerSilenceFrames;

  /// Whether the asynchronous capture writer reported a WAV write failure.
  final bool writerFailed;

  /// Capture-start timestamp expressed as a reusable clock snapshot.
  SoLoudCaptureClockSnapshot get captureStartClock =>
      SoLoudCaptureClockSnapshot(
        hostTimeNanos: captureStartHostTimeNanos,
        sessionStartHostTimeNanos: sessionStartHostTimeNanos,
        sampleRate: sampleRate,
        inputDeviceFrame: 0,
      );

  /// First-input-buffer timestamp expressed as a reusable clock snapshot.
  SoLoudCaptureClockSnapshot? get firstInputBufferClock {
    final hostTime = firstInputBufferHostTimeNanos;
    if (hostTime == null) {
      return null;
    }
    return SoLoudCaptureClockSnapshot(
      hostTimeNanos: hostTime,
      sessionStartHostTimeNanos: sessionStartHostTimeNanos,
      sampleRate: sampleRate,
      inputDeviceFrame: firstInputBufferFrameIndex ?? 0,
    );
  }

  /// Capture-stop timestamp expressed as a reusable clock snapshot.
  SoLoudCaptureClockSnapshot get captureStopClock => SoLoudCaptureClockSnapshot(
    hostTimeNanos: captureStopHostTimeNanos,
    sessionStartHostTimeNanos: sessionStartHostTimeNanos,
    sampleRate: sampleRate,
    inputDeviceFrame: frameCount,
  );
}

/// A clock snapshot from the active miniaudio capture session.
final class SoLoudCaptureClockSnapshot {
  /// Create a native capture clock snapshot.
  const SoLoudCaptureClockSnapshot({
    required this.hostTimeNanos,
    required this.sessionStartHostTimeNanos,
    required this.sampleRate,
    required this.inputDeviceFrame,
  });

  /// Current monotonic clock timestamp.
  final int hostTimeNanos;

  /// Session origin used to derive [sessionElapsedNanos].
  final int sessionStartHostTimeNanos;

  /// Capture sample rate used for frame estimates.
  final int sampleRate;

  /// Number of captured input frames at [hostTimeNanos].
  final int inputDeviceFrame;

  /// Elapsed nanoseconds since [sessionStartHostTimeNanos].
  int get sessionElapsedNanos {
    final elapsed = hostTimeNanos - sessionStartHostTimeNanos;
    return elapsed < 0 ? 0 : elapsed;
  }

  /// Estimated native frame position at [hostTimeNanos].
  int get estimatedFrame {
    if (sampleRate <= 0) {
      return 0;
    }
    return (sessionElapsedNanos * sampleRate / 1000000000).round();
  }
}

/// A live input-level snapshot from the active miniaudio capture session.
final class SoLoudCaptureLevelSnapshot {
  /// Create a live capture level snapshot.
  const SoLoudCaptureLevelSnapshot({
    required this.currentPeak,
    required this.currentRms,
    required this.peakSinceLastRead,
    required this.heldPeak,
    required this.frameCount,
  });

  /// Peak absolute sample value from the most recent input callback.
  final double currentPeak;

  /// RMS level from the most recent input callback.
  final double currentRms;

  /// Peak absolute sample value seen since the previous snapshot read.
  final double peakSinceLastRead;

  /// Highest absolute sample value seen during this capture session.
  final double heldPeak;

  /// Number of captured input frames at the time of the snapshot.
  final int frameCount;
}
