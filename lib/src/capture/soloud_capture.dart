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
  SoLoudCaptureClockSnapshot get captureStopClock =>
      SoLoudCaptureClockSnapshot(
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
