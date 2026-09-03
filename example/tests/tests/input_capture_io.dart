// Explicit capture format arguments document and validate the migration seam.
// ignore_for_file: avoid_redundant_argument_values

import 'dart:io';

import 'package:flutter_soloud/flutter_soloud.dart';
import 'package:path_provider/path_provider.dart';

import 'common.dart';

/// Exercise microphone capture against a real native input device.
Future<OutputBuffer> testInputCapture() async {
  final output = OutputBuffer();
  final generatedFiles = <File>[];

  await initialize();

  try {
    final devices = SoLoud.instance.listCaptureDevices();
    assert(devices.isNotEmpty, 'No capture devices were found');
    final device = devices.first;
    output.writeln('Using capture device: ${device.name}');

    final directory = await getTemporaryDirectory();
    final runId = DateTime.now().microsecondsSinceEpoch;

    for (final mirrorFormat in SoLoudCaptureMirrorFormat.values) {
      final stem = '${directory.path}/soloud_input_${runId}_'
          '${mirrorFormat.name}';
      final wavFile = File('$stem.wav');
      final mirrorFile = switch (mirrorFormat) {
        SoLoudCaptureMirrorFormat.none => null,
        SoLoudCaptureMirrorFormat.flac => File('$stem.flac'),
        SoLoudCaptureMirrorFormat.wavPack => File('$stem.wv'),
      };
      generatedFiles
        ..add(wavFile)
        ..addAll([if (mirrorFile != null) mirrorFile]);

      final start = SoLoud.instance.startCapture(
        wavFile.path,
        sampleRate: 48000,
        channels: Channels.stereo,
        bufferSizeFrames: 256,
        inputGainDb: 6,
        device: device,
        mirrorPath: mirrorFile?.path,
        mirrorFormat: mirrorFormat,
        mirrorBitsPerSample: 24,
      );
      assert(start.sampleRate == 48000, 'Capture did not start at 48 kHz');
      assert(start.channels == 2, 'Capture did not start in stereo');
      assert(SoLoud.instance.isCaptureRecording, 'Capture did not start');

      await delay(750);

      final clock = SoLoud.instance.captureClockSnapshot();
      final level = SoLoud.instance.captureLevelSnapshot();
      assert(clock.inputDeviceFrame > 0, 'Capture clock did not advance');
      assert(level.frameCount > 0, 'Level snapshot did not advance');
      assert(level.currentPeak >= 0, 'Invalid current peak');
      assert(level.currentRms >= 0, 'Invalid current RMS');

      final stop = SoLoud.instance.stopCapture();
      assert(stop.frameCount > 0, 'No input frames were captured');
      assert(stop.duration > Duration.zero, 'Capture duration is empty');
      assert(
        stop.firstInputBufferClock != null,
        'First input callback timestamp is missing',
      );
      assert(!stop.writerFailed, 'The asynchronous WAV writer failed');
      assert(
        stop.writerSilenceFrames == stop.writerOverflowFrames,
        'Overflow compensation did not preserve the capture duration',
      );
      assert(
        wavFile.existsSync() && wavFile.lengthSync() > 44,
        'The WAV safety recording is empty',
      );

      if (mirrorFile != null) {
        assert(start.mirrorActive, '$mirrorFormat did not start');
        assert(stop.mirrorSucceeded, '$mirrorFormat did not finalize');
        assert(
          stop.mirrorFrameCount == stop.frameCount,
          '$mirrorFormat frame count differs from the WAV frame count',
        );
        assert(
          mirrorFile.existsSync() && mirrorFile.lengthSync() > 0,
          '$mirrorFormat output is empty',
        );
      }

      output.writeln(
        '$mirrorFormat: ${stop.frameCount} frames, '
        'peak=${level.currentPeak}, rms=${level.currentRms}, '
        'overflow=${stop.writerOverflowFrames}',
      );
    }

    final cancelFile = File('${directory.path}/soloud_cancel_$runId.wav');
    generatedFiles.add(cancelFile);
    SoLoud.instance.startCapture(
      cancelFile.path,
      sampleRate: 48000,
      channels: Channels.stereo,
      device: device,
    );
    await delay(150);
    SoLoud.instance.cancelCapture();
    assert(!SoLoud.instance.isCaptureRecording, 'Capture did not cancel');
    assert(!cancelFile.existsSync(), 'Cancel left a partial WAV file');

    final coordinatedFile =
        File('${directory.path}/soloud_coordinated_$runId.wav');
    generatedFiles.add(coordinatedFile);
    final sound = await SoLoud.instance.loadWaveform(
      WaveForm.square,
      false,
      0.1,
      0,
    );
    final coordinated = SoLoud.instance.startCaptureAndPlay(
      coordinatedFile.path,
      sound,
      sampleRate: 48000,
      channels: Channels.stereo,
      device: device,
    );
    assert(
      coordinated.playbackStartHostTimeNanos >=
          coordinated.captureStartHostTimeNanos,
      'Playback started before capture',
    );
    await delay(300);
    final coordinatedStop = SoLoud.instance.stopCapture();
    assert(coordinatedStop.frameCount > 0, 'Coordinated capture is empty');
    await SoLoud.instance.stop(SoundHandle(coordinated.handle));

    final deinitFile = File('${directory.path}/soloud_deinit_$runId.wav');
    generatedFiles.add(deinitFile);
    SoLoud.instance.startCapture(
      deinitFile.path,
      sampleRate: 48000,
      channels: Channels.stereo,
      device: device,
    );
    await delay(150);
    deinit();
    assert(!deinitFile.existsSync(), 'Deinit left a partial WAV file');
    await initialize();

    output.writeln('Cancel, coordinated start, and active deinit passed');
    return output;
  } finally {
    if (SoLoud.instance.isCaptureRecording) {
      SoLoud.instance.cancelCapture();
    }
    if (SoLoud.instance.isInitialized) {
      deinit();
    }
    for (final file in generatedFiles) {
      if (file.existsSync()) {
        file.deleteSync();
      }
    }
  }
}
