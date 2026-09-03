part of 'bindings_player_ffi.dart';

// ignore_for_file: omit_local_variable_types

/// Native microphone-capture bindings and their Dart-owned output storage.
mixin _FlutterSoLoudFfiCapture on FlutterSoLoud {
  ffi.Pointer<ffi.Float>? _captureLevelCurrentPeak;
  ffi.Pointer<ffi.Float>? _captureLevelCurrentRms;
  ffi.Pointer<ffi.Float>? _captureLevelPeakSinceLastRead;
  ffi.Pointer<ffi.Float>? _captureLevelHeldPeak;
  ffi.Pointer<ffi.Uint64>? _captureLevelFrameCount;

  @override
  List<CaptureDevice> listCaptureDevices() {
    final ret = <CaptureDevice>[];
    final ffi.Pointer<ffi.Pointer<ffi.Char>> deviceNames = calloc(
      ffi.sizeOf<ffi.Pointer<ffi.Pointer<ffi.Char>>>() * 255,
    );
    final ffi.Pointer<ffi.Pointer<ffi.Int>> deviceIds = calloc(
      ffi.sizeOf<ffi.Pointer<ffi.Pointer<ffi.Int>>>() * 50,
    );
    final ffi.Pointer<ffi.Pointer<ffi.Int>> deviceIsDefault = calloc(
      ffi.sizeOf<ffi.Pointer<ffi.Pointer<ffi.Int>>>() * 50,
    );
    final ffi.Pointer<ffi.Int> nDevices = calloc();

    native.listCaptureDevices(
      deviceNames,
      deviceIds,
      deviceIsDefault,
      nDevices,
    );

    final ndev = nDevices.value;
    for (var i = 0; i < ndev; i++) {
      final s1 = (deviceNames + i).value;
      final s = s1.cast<Utf8>().toDartString();
      final id1 = (deviceIds + i).value;
      final id = id1.value;
      final n1 = (deviceIsDefault + i).value;
      final n = n1.value;
      ret.add(CaptureDevice(id, n == 1, s));
    }

    native.freeListCaptureDevices(
      deviceNames,
      deviceIds,
      deviceIsDefault,
      ndev,
    );

    calloc
      ..free(deviceNames)
      ..free(deviceIds)
      ..free(deviceIsDefault)
      ..free(nDevices);
    return ret;
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
    final pathPtr = path.toNativeUtf8();
    final mirrorPathPtr = (mirrorPath ?? '').toNativeUtf8();
    final actualSampleRate = calloc<ffi.UnsignedInt>();
    final actualChannels = calloc<ffi.UnsignedInt>();
    final sessionStartHostTimeNanos = calloc<ffi.Uint64>();
    final captureStartHostTimeNanos = calloc<ffi.Uint64>();
    final actualMirrorFormat = calloc<ffi.UnsignedInt>();
    final mirrorActive = calloc<ffi.UnsignedInt>();
    final error = native.startCapture(
      pathPtr.cast(),
      sampleRate,
      channels,
      bufferSizeFrames,
      inputGainDb,
      device?.id ?? -1,
      mirrorPathPtr.cast(),
      mirrorFormat.index,
      mirrorBitsPerSample,
      actualSampleRate,
      actualChannels,
      sessionStartHostTimeNanos,
      captureStartHostTimeNanos,
      actualMirrorFormat,
      mirrorActive,
    );
    final result = error == native.PlayerErrors.noError
        ? SoLoudCaptureStartResult(
            path: path,
            sampleRate: actualSampleRate.value,
            channels: actualChannels.value,
            bufferSizeFrames: bufferSizeFrames,
            sessionStartHostTimeNanos: sessionStartHostTimeNanos.value,
            captureStartHostTimeNanos: captureStartHostTimeNanos.value,
            mirrorPath: mirrorPath,
            mirrorFormat: soLoudCaptureMirrorFormatFromValue(
              actualMirrorFormat.value,
            ),
            mirrorBitsPerSample: mirrorBitsPerSample,
            mirrorActive: mirrorActive.value != 0,
          )
        : null;
    calloc
      ..free(pathPtr)
      ..free(mirrorPathPtr)
      ..free(actualSampleRate)
      ..free(actualChannels)
      ..free(sessionStartHostTimeNanos)
      ..free(captureStartHostTimeNanos)
      ..free(actualMirrorFormat)
      ..free(mirrorActive);
    return (error: PlayerErrors.values[error.value], result: result);
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
    final pathPtr = path.toNativeUtf8();
    final mirrorPathPtr = (mirrorPath ?? '').toNativeUtf8();
    final handle = calloc<ffi.UnsignedInt>();
    final actualSampleRate = calloc<ffi.UnsignedInt>();
    final actualChannels = calloc<ffi.UnsignedInt>();
    final sessionStartHostTimeNanos = calloc<ffi.Uint64>();
    final captureStartHostTimeNanos = calloc<ffi.Uint64>();
    final playbackStartHostTimeNanos = calloc<ffi.Uint64>();
    final actualMirrorFormat = calloc<ffi.UnsignedInt>();
    final mirrorActive = calloc<ffi.UnsignedInt>();
    final error = native.startCaptureAndPlay(
      pathPtr.cast(),
      soundHash.hash,
      busId,
      sampleRate,
      channels,
      bufferSizeFrames,
      volume,
      pan,
      startAt.toDouble(),
      looping,
      loopingStartAt.toDouble(),
      inputGainDb,
      device?.id ?? -1,
      mirrorPathPtr.cast(),
      mirrorFormat.index,
      mirrorBitsPerSample,
      handle,
      actualSampleRate,
      actualChannels,
      sessionStartHostTimeNanos,
      captureStartHostTimeNanos,
      playbackStartHostTimeNanos,
      actualMirrorFormat,
      mirrorActive,
    );
    final result = error == native.PlayerErrors.noError
        ? SoLoudCapturePlaybackStartResult(
            path: path,
            handle: handle.value,
            sampleRate: actualSampleRate.value,
            channels: actualChannels.value,
            bufferSizeFrames: bufferSizeFrames,
            sessionStartHostTimeNanos: sessionStartHostTimeNanos.value,
            captureStartHostTimeNanos: captureStartHostTimeNanos.value,
            playbackStartHostTimeNanos: playbackStartHostTimeNanos.value,
            mirrorPath: mirrorPath,
            mirrorFormat: soLoudCaptureMirrorFormatFromValue(
              actualMirrorFormat.value,
            ),
            mirrorBitsPerSample: mirrorBitsPerSample,
            mirrorActive: mirrorActive.value != 0,
          )
        : null;
    calloc
      ..free(pathPtr)
      ..free(mirrorPathPtr)
      ..free(handle)
      ..free(actualSampleRate)
      ..free(actualChannels)
      ..free(sessionStartHostTimeNanos)
      ..free(captureStartHostTimeNanos)
      ..free(playbackStartHostTimeNanos)
      ..free(actualMirrorFormat)
      ..free(mirrorActive);
    return (error: PlayerErrors.values[error.value], result: result);
  }

  @override
  ({PlayerErrors error, SoLoudCaptureStopResult? result}) stopCapture() {
    final sampleRate = calloc<ffi.UnsignedInt>();
    final channels = calloc<ffi.UnsignedInt>();
    final frameCount = calloc<ffi.Uint64>();
    final sessionStartHostTimeNanos = calloc<ffi.Uint64>();
    final captureStartHostTimeNanos = calloc<ffi.Uint64>();
    final firstInputBufferHostTimeNanos = calloc<ffi.Uint64>();
    final firstInputBufferFrameIndex = calloc<ffi.Uint64>();
    final captureStopHostTimeNanos = calloc<ffi.Uint64>();
    final mirrorFormat = calloc<ffi.UnsignedInt>();
    final mirrorSucceeded = calloc<ffi.UnsignedInt>();
    final mirrorFrameCount = calloc<ffi.Uint64>();
    final writerOverflowFrames = calloc<ffi.Uint64>();
    final writerSilenceFrames = calloc<ffi.Uint64>();
    final writerFailed = calloc<ffi.UnsignedInt>();
    final error = native.stopCapture(
      sampleRate,
      channels,
      frameCount,
      sessionStartHostTimeNanos,
      captureStartHostTimeNanos,
      firstInputBufferHostTimeNanos,
      firstInputBufferFrameIndex,
      captureStopHostTimeNanos,
      mirrorFormat,
      mirrorSucceeded,
      mirrorFrameCount,
      writerOverflowFrames,
      writerSilenceFrames,
      writerFailed,
    );
    final result = error == native.PlayerErrors.noError
        ? SoLoudCaptureStopResult(
            path: '',
            sampleRate: sampleRate.value,
            channels: channels.value,
            frameCount: frameCount.value,
            duration: Duration(
              microseconds: sampleRate.value <= 0
                  ? 0
                  : (frameCount.value *
                            Duration.microsecondsPerSecond /
                            sampleRate.value)
                        .round(),
            ),
            sessionStartHostTimeNanos: sessionStartHostTimeNanos.value,
            captureStartHostTimeNanos: captureStartHostTimeNanos.value,
            firstInputBufferHostTimeNanos:
                firstInputBufferHostTimeNanos.value == 0
                ? null
                : firstInputBufferHostTimeNanos.value,
            firstInputBufferFrameIndex: firstInputBufferHostTimeNanos.value == 0
                ? null
                : firstInputBufferFrameIndex.value,
            captureStopHostTimeNanos: captureStopHostTimeNanos.value,
            mirrorFormat: soLoudCaptureMirrorFormatFromValue(
              mirrorFormat.value,
            ),
            mirrorSucceeded: mirrorSucceeded.value != 0,
            mirrorFrameCount: mirrorFrameCount.value,
            writerOverflowFrames: writerOverflowFrames.value,
            writerSilenceFrames: writerSilenceFrames.value,
            writerFailed: writerFailed.value != 0,
          )
        : null;
    calloc
      ..free(sampleRate)
      ..free(channels)
      ..free(frameCount)
      ..free(sessionStartHostTimeNanos)
      ..free(captureStartHostTimeNanos)
      ..free(firstInputBufferHostTimeNanos)
      ..free(firstInputBufferFrameIndex)
      ..free(captureStopHostTimeNanos)
      ..free(mirrorFormat)
      ..free(mirrorSucceeded)
      ..free(mirrorFrameCount)
      ..free(writerOverflowFrames)
      ..free(writerSilenceFrames)
      ..free(writerFailed);
    return (error: PlayerErrors.values[error.value], result: result);
  }

  @override
  PlayerErrors cancelCapture() {
    final error = native.cancelCapture();
    return PlayerErrors.values[error.value];
  }

  @override
  bool isCaptureRecording() => native.isCaptureRecording() == 1;

  @override
  ({PlayerErrors error, SoLoudCaptureClockSnapshot? result})
  getCaptureClockSnapshot() {
    final hostTimeNanos = calloc<ffi.Uint64>();
    final sessionStartHostTimeNanos = calloc<ffi.Uint64>();
    final sampleRate = calloc<ffi.UnsignedInt>();
    final inputDeviceFrame = calloc<ffi.Uint64>();
    final error = native.getCaptureClockSnapshot(
      hostTimeNanos,
      sessionStartHostTimeNanos,
      sampleRate,
      inputDeviceFrame,
    );
    final result = error == native.PlayerErrors.noError
        ? SoLoudCaptureClockSnapshot(
            hostTimeNanos: hostTimeNanos.value,
            sessionStartHostTimeNanos: sessionStartHostTimeNanos.value,
            sampleRate: sampleRate.value,
            inputDeviceFrame: inputDeviceFrame.value,
          )
        : null;
    calloc
      ..free(hostTimeNanos)
      ..free(sessionStartHostTimeNanos)
      ..free(sampleRate)
      ..free(inputDeviceFrame);
    return (error: PlayerErrors.values[error.value], result: result);
  }

  @override
  ({PlayerErrors error, SoLoudCaptureLevelSnapshot? result})
  getCaptureLevelSnapshot() {
    _ensureCaptureLevelPointers();
    final currentPeak = _captureLevelCurrentPeak!;
    final currentRms = _captureLevelCurrentRms!;
    final peakSinceLastRead = _captureLevelPeakSinceLastRead!;
    final heldPeak = _captureLevelHeldPeak!;
    final frameCount = _captureLevelFrameCount!;
    final error = native.getCaptureLevelSnapshot(
      currentPeak,
      currentRms,
      peakSinceLastRead,
      heldPeak,
      frameCount,
    );
    final result = error == native.PlayerErrors.noError
        ? SoLoudCaptureLevelSnapshot(
            currentPeak: currentPeak.value,
            currentRms: currentRms.value,
            peakSinceLastRead: peakSinceLastRead.value,
            heldPeak: heldPeak.value,
            frameCount: frameCount.value,
          )
        : null;
    return (error: PlayerErrors.values[error.value], result: result);
  }

  void _ensureCaptureLevelPointers() {
    _captureLevelCurrentPeak ??= calloc<ffi.Float>();
    _captureLevelCurrentRms ??= calloc<ffi.Float>();
    _captureLevelPeakSinceLastRead ??= calloc<ffi.Float>();
    _captureLevelHeldPeak ??= calloc<ffi.Float>();
    _captureLevelFrameCount ??= calloc<ffi.Uint64>();
  }

  void _disposeCaptureLevelPointers() {
    final currentPeak = _captureLevelCurrentPeak;
    final currentRms = _captureLevelCurrentRms;
    final peakSinceLastRead = _captureLevelPeakSinceLastRead;
    final heldPeak = _captureLevelHeldPeak;
    final frameCount = _captureLevelFrameCount;

    if (currentPeak != null) {
      calloc.free(currentPeak);
      _captureLevelCurrentPeak = null;
    }
    if (currentRms != null) {
      calloc.free(currentRms);
      _captureLevelCurrentRms = null;
    }
    if (peakSinceLastRead != null) {
      calloc.free(peakSinceLastRead);
      _captureLevelPeakSinceLastRead = null;
    }
    if (heldPeak != null) {
      calloc.free(heldPeak);
      _captureLevelHeldPeak = null;
    }
    if (frameCount != null) {
      calloc.free(frameCount);
      _captureLevelFrameCount = null;
    }
  }
}
