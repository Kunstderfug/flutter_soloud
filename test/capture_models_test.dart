import 'package:flutter_soloud/flutter_soloud.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  group('SoLoud capture models', () {
    test('native mirror values decode safely', () {
      expect(
        soLoudCaptureMirrorFormatFromValue(1),
        SoLoudCaptureMirrorFormat.flac,
      );
      expect(
        soLoudCaptureMirrorFormatFromValue(2),
        SoLoudCaptureMirrorFormat.wavPack,
      );
      expect(
        soLoudCaptureMirrorFormatFromValue(-1),
        SoLoudCaptureMirrorFormat.none,
      );
      expect(
        soLoudCaptureMirrorFormatFromValue(99),
        SoLoudCaptureMirrorFormat.none,
      );
    });

    test('clock snapshots project elapsed time onto capture frames', () {
      const snapshot = SoLoudCaptureClockSnapshot(
        hostTimeNanos: 1_250_000_000,
        sessionStartHostTimeNanos: 1_000_000_000,
        sampleRate: 48000,
        inputDeviceFrame: 11980,
      );
      const beforeOrigin = SoLoudCaptureClockSnapshot(
        hostTimeNanos: 999_000_000,
        sessionStartHostTimeNanos: 1_000_000_000,
        sampleRate: 48000,
        inputDeviceFrame: 0,
      );

      expect(snapshot.sessionElapsedNanos, 250_000_000);
      expect(snapshot.estimatedFrame, 12000);
      expect(beforeOrigin.sessionElapsedNanos, 0);
      expect(beforeOrigin.estimatedFrame, 0);
    });

    test('stop results expose comparable session clocks', () {
      const result = SoLoudCaptureStopResult(
        path: '/tmp/input.wav',
        sampleRate: 48000,
        channels: 2,
        frameCount: 48000,
        duration: Duration(seconds: 1),
        sessionStartHostTimeNanos: 2_000_000_000,
        captureStartHostTimeNanos: 2_010_000_000,
        firstInputBufferHostTimeNanos: 2_012_000_000,
        firstInputBufferFrameIndex: 96,
        captureStopHostTimeNanos: 3_010_000_000,
        writerOverflowFrames: 256,
        writerSilenceFrames: 256,
        writerFailed: true,
      );

      expect(result.captureStartClock.inputDeviceFrame, 0);
      expect(result.firstInputBufferClock?.inputDeviceFrame, 96);
      expect(result.captureStopClock.inputDeviceFrame, 48000);
      expect(result.captureStopClock.sessionElapsedNanos, 1_010_000_000);
      expect(result.writerOverflowFrames, 256);
      expect(result.writerSilenceFrames, 256);
      expect(result.writerFailed, isTrue);
    });

    test('coordinated starts expose capture and playback on one clock', () {
      const result = SoLoudCapturePlaybackStartResult(
        path: '/tmp/input.wav',
        handle: 42,
        sampleRate: 48000,
        channels: 2,
        bufferSizeFrames: 256,
        sessionStartHostTimeNanos: 5_000_000_000,
        captureStartHostTimeNanos: 5_010_000_000,
        playbackStartHostTimeNanos: 5_012_000_000,
      );

      expect(result.captureStartClock.sessionElapsedNanos, 10_000_000);
      expect(result.playbackStartClock.sessionElapsedNanos, 12_000_000);
      expect(result.playbackStartClock.estimatedFrame, 576);
    });

    test('level snapshots preserve current and session diagnostics', () {
      const level = SoLoudCaptureLevelSnapshot(
        currentPeak: 0.5,
        currentRms: 0.25,
        peakSinceLastRead: 0.75,
        heldPeak: 0.9,
        frameCount: 9600,
      );

      expect(level.currentPeak, 0.5);
      expect(level.currentRms, 0.25);
      expect(level.peakSinceLastRead, 0.75);
      expect(level.heldPeak, 0.9);
      expect(level.frameCount, 9600);
    });
  });
}
