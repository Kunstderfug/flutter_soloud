// ignore_for_file: experimental_member_use

// Live test of the CLAP Dart wrapper against installed CLAP plugins. The
// calls that run on the platform main thread (scan, load, values, text,
// state) need a running app, so this is an integration test:
//
//   cd example
//   flutter test integration_test/clap_wrapper_test.dart -d macos
//
// It uses ZeroEQ.clap (or CLAP_TEST_BUNDLE) from the standard CLAP folders
// and skips when none is installed. The mock bundle group runs with
// CLAP_MOCK_BUNDLE set to the MockClap.clap that test/run_clap_host_test.sh
// builds (in $TMPDIR/clap_host_test).
import 'dart:ffi' as ffi;
import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:flutter_soloud/flutter_soloud.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';

String? _findBundle(ClapHost host, String name) {
  final env = Platform.environment['CLAP_TEST_BUNDLE'];
  if (env != null && env.isNotEmpty && Directory(env).existsSync()) {
    return env;
  }
  for (final dir in host.standardSearchPaths) {
    final path = '$dir/$name';
    if (Directory(path).existsSync()) {
      return path;
    }
  }
  return null;
}

/// Polls the value of [paramId] until it is within [tolerance] of [expected],
/// draining the events of [plugin] meanwhile, and returns the last value.
Future<double> _waitForValue(
  ClapEffectPlugin plugin,
  int paramId,
  double expected, {
  double tolerance = 1e-6,
  List<ClapHostEvent>? events,
}) async {
  final deadline = DateTime.now().add(const Duration(seconds: 3));
  var value = plugin.getParameterValue(paramId);
  while ((value - expected).abs() > tolerance &&
      DateTime.now().isBefore(deadline)) {
    await Future<void>.delayed(const Duration(milliseconds: 20));
    events?.addAll(plugin.drainEvents());
    value = plugin.getParameterValue(paramId);
  }
  return value;
}

void main() {
  IntegrationTestWidgetsFlutterBinding.ensureInitialized();

  final host = ClapHost();
  final bundle = _findBundle(host, 'ZeroEQ.clap');
  final skip = !Platform.isMacOS
      ? 'CLAP hosting is macOS-only'
      : bundle == null
      ? 'ZeroEQ.clap is not installed'
      : null;

  group('CLAP wrapper', skip: skip, () {
    late AudioSource tone;
    late SoundHandle voice;

    Future<void> start() async {
      await SoLoud.instance.init(sampleRate: 48000, bufferSize: 512);
      // A playing voice keeps the global filters, and so the plugin, running.
      tone = await SoLoud.instance.loadWaveform(WaveForm.sin, false, 1, 0);
      SoLoud.instance.setWaveformFreq(tone, 440);
      voice = SoLoud.instance.play(tone, volume: 0.05);
    }

    Future<void> stop() async {
      await SoLoud.instance.stop(voice);
      await SoLoud.instance.disposeSource(tone);
      SoLoud.instance.deinit();
    }

    /// Whether [plugin] processes audio: a queued value only reaches the
    /// plugin with a processed block.
    Future<bool> processes(ClapEffectPlugin plugin) async {
      final param = plugin.parameters.firstWhere(
        (p) =>
            !p.isStepped &&
            !p.isReadOnly &&
            !p.isHidden &&
            p.maxValue > p.minValue,
      );
      final current = plugin.getParameterValue(param.paramId);
      final mid = (param.minValue + param.maxValue) / 2;
      final target = (current - mid).abs() < 1e-3
          ? param.minValue + (param.maxValue - param.minValue) * 0.3
          : mid;
      plugin.setParameterValue(param.paramId, target);
      final value = await _waitForValue(plugin, param.paramId, target);
      return (value - target).abs() <= 1e-6;
    }

    setUpAll(start);

    tearDownAll(() async {
      host.unloadAll();
      await stop();
    });

    testWidgets('host reports support, version and search paths', (_) async {
      expect(host.isSupported, isTrue);
      final version = host.hostClapVersion;
      expect(version.major, 1);
      expect(version.minor, greaterThanOrEqualTo(2));
      expect(host.standardSearchPaths, contains('/Library/Audio/Plug-Ins/CLAP'));
    });

    testWidgets('scan describes the bundle and rejects a missing one', (
      _,
    ) async {
      final plugins = host.scanBundle(bundle!);
      expect(plugins, isNotEmpty);
      final eq = plugins.first;
      expect(eq.pluginId, isNotEmpty);
      expect(eq.name, isNotEmpty);
      expect(eq.isAudioEffect, isTrue);
      debugPrint('CLAP scan: $plugins');
      expect(
        () => host.scanBundle('/nonexistent/Missing.clap'),
        throwsA(isA<SoLoudCppException>()),
      );
    });

    testWidgets('effect loads, edits, saves state and disposes', (_) async {
      const slot = CustomFilterGlobal(0);
      final plugin = host.loadEffect(bundlePath: bundle!, slot: slot);
      expect(slot.index, greaterThanOrEqualTo(0));
      expect(plugin.isDisposed, isFalse);
      expect(plugin.latencySamples, greaterThanOrEqualTo(0));
      expect(plugin.parameters, isNotEmpty);
      final latency = plugin.latencySamples;
      debugPrint('CLAP params: ${plugin.parameters.length}, latency $latency');

      // A continuous, writable parameter with a real range.
      final param = plugin.parameters.firstWhere(
        (p) =>
            !p.isStepped &&
            !p.isReadOnly &&
            !p.isHidden &&
            p.maxValue > p.minValue,
      );
      final initial = plugin.getParameterValue(param.paramId);
      expect(initial, inInclusiveRange(param.minValue, param.maxValue));
      final text = plugin.valueToText(param.paramId, initial);
      expect(text, isNotEmpty);
      debugPrint('CLAP ${param.name} = $initial ("$text")');

      // Plain values reach the plugin with a processed block.
      final events = <ClapHostEvent>[];
      final target = param.minValue + (param.maxValue - param.minValue) * 0.25;
      plugin.setParameterValue(param.paramId, target);
      expect(
        await _waitForValue(plugin, param.paramId, target, events: events),
        closeTo(target, 1e-6),
      );

      // State round-trip: save at the target, move away, restore.
      final state = plugin.saveState();
      expect(state, isNotEmpty);
      final other = param.minValue + (param.maxValue - param.minValue) * 0.75;
      plugin.setParameterValue(param.paramId, other);
      expect(
        await _waitForValue(plugin, param.paramId, other, events: events),
        closeTo(other, 1e-6),
      );
      plugin.restoreState(state);
      expect(plugin.getParameterValue(param.paramId), closeTo(target, 1e-6));
      events.addAll(plugin.drainEvents());
      expect(
        events.map((e) => e.type),
        contains(ClapHostEventType.paramsRescan),
      );
      debugPrint('CLAP events: ${events.length}');

      // Bounded draining is covered by the mock bundle group: a real plugin
      // queues too few events, and notifications of one kind coalesce.

      // Dispose releases the slot, and twice is fine.
      plugin.dispose();
      expect(plugin.isDisposed, isTrue);
      expect(slot.index, lessThan(0));
      plugin.dispose();
      expect(plugin.drainEvents(), isEmpty);
      expect(plugin.latencySamples, 0);
      expect(
        () => plugin.getParameterValue(param.paramId),
        throwsA(isA<StateError>()),
      );
    });

    testWidgets('unloadAll leaves plugins inert until disposed', (_) async {
      const slot = CustomFilterGlobal(1);
      final plugin = host.loadEffect(
        bundlePath: bundle!,
        slot: slot,
        pluginId: host.scanBundle(bundle).first.pluginId,
      );
      expect(slot.index, greaterThanOrEqualTo(0));
      host.unloadAll();
      expect(plugin.drainEvents(), isEmpty);
      expect(
        () => plugin.getParameterValue(plugin.parameters.first.paramId),
        throwsA(isA<SoLoudCppException>()),
      );
      plugin.dispose();
      expect(slot.index, lessThan(0));
    });

    testWidgets('a stale plugin leaves a reloaded slot alone', (_) async {
      const slot = CustomFilterGlobal(3);
      final stale = host.loadEffect(bundlePath: bundle!, slot: slot);
      host.unloadAll();
      // The slot is still active: the new plugin takes it over.
      final current = host.loadEffect(bundlePath: bundle, slot: slot);
      stale.dispose();
      expect(slot.index, greaterThanOrEqualTo(0));
      expect(await processes(current), isTrue);

      // Loading over a live plugin moves the slot too.
      final next = host.loadEffect(bundlePath: bundle, slot: slot);
      current.dispose();
      expect(slot.index, greaterThanOrEqualTo(0));
      expect(await processes(next), isTrue);
      next.dispose();
      expect(slot.index, lessThan(0));
    });

    testWidgets('slots that alias one native slot share its owner', (
      _,
    ) async {
      // The native side routes by the sound and ignores the bus then.
      final viaSound = CustomFilterSingle(5, tone.soundHash, null);
      final viaSoundAndBus = CustomFilterSingle(5, tone.soundHash, 987654);
      final first = host.loadEffect(bundlePath: bundle!, slot: viaSound);
      final second = host.loadEffect(bundlePath: bundle, slot: viaSoundAndBus);
      expect(viaSoundAndBus.index, viaSound.index);
      first.dispose();
      expect(viaSound.index, greaterThanOrEqualTo(0));
      second.dispose();
      expect(viaSound.index, lessThan(0));
    });

    testWidgets('a failed load leaves the slot as it was', (_) async {
      const slot = CustomFilterGlobal(2);
      expect(
        () => host.loadEffect(
          bundlePath: bundle!,
          slot: slot,
          pluginId: 'no.such.plugin',
        ),
        throwsA(isA<SoLoudCppException>()),
      );
      expect(slot.index, lessThan(0));

      // A slot that cannot be activated (a bus that does not exist): the
      // loaded plugin is unloaded again.
      const missingBus = CustomFilterSingle(0, null, 987654);
      expect(
        () => host.loadEffect(bundlePath: bundle!, slot: missingBus),
        throwsA(isA<SoLoudException>()),
      );
    });

    // Last: it restarts the engine.
    testWidgets('a plugin from before a restart leaves the new slot alone', (
      _,
    ) async {
      const slot = CustomFilterGlobal(4);
      final stale = host.loadEffect(bundlePath: bundle!, slot: slot);
      await stop();
      await start();
      expect(slot.index, lessThan(0));
      final current = host.loadEffect(bundlePath: bundle, slot: slot);
      stale.dispose();
      expect(slot.index, greaterThanOrEqualTo(0));
      expect(await processes(current), isTrue);
      current.dispose();
      expect(slot.index, lessThan(0));
    });
  });

  final mockBundle = Platform.environment['CLAP_MOCK_BUNDLE'];
  final mockSkip = !Platform.isMacOS
      ? 'CLAP hosting is macOS-only'
      : mockBundle == null || !Directory(mockBundle).existsSync()
      ? 'CLAP_MOCK_BUNDLE is not set'
      : null;

  group('CLAP wrapper with the mock bundle', skip: mockSkip, () {
    late AudioSource tone;
    late SoundHandle voice;

    setUpAll(() async {
      await SoLoud.instance.init(sampleRate: 48000, bufferSize: 512);
      tone = await SoLoud.instance.loadWaveform(WaveForm.sin, false, 1, 0);
      voice = SoLoud.instance.play(tone, volume: 0.05);
    });

    tearDownAll(() async {
      host.unloadAll();
      await SoLoud.instance.stop(voice);
      await SoLoud.instance.disposeSource(tone);
      SoLoud.instance.deinit();
    });

    testWidgets('a bounded drain leaves the rest queued, in order', (_) async {
      const slot = CustomFilterGlobal(0);
      final plugin = host.loadEffect(
        bundlePath: mockBundle!,
        slot: slot,
        pluginId: 'mock.ext',
      );
      // The bundle's own image, already loaded by the host.
      final image = ffi.DynamicLibrary.open(
        '$mockBundle/Contents/MacOS/MockClap',
      );
      final emitGestures = image
          .lookupFunction<
            ffi.Void Function(ffi.Int, ffi.Int),
            void Function(int, int)
          >('mock_clap_emit_gestures');
      plugin.drainEvents();

      // One processed block emits BEGIN, 3 x VALUE, END together.
      emitGestures(1, 3);
      final deadline = DateTime.now().add(const Duration(seconds: 3));
      var first = plugin.drainEvents(maxEvents: 2);
      while (first.isEmpty && DateTime.now().isBefore(deadline)) {
        await Future<void>.delayed(const Duration(milliseconds: 20));
        first = plugin.drainEvents(maxEvents: 2);
      }
      final rest = plugin.drainEvents();
      debugPrint('CLAP mock events: $first then $rest');
      expect(first.map((e) => e.type), [
        ClapHostEventType.paramGestureBegin,
        ClapHostEventType.paramValue,
      ]);
      expect(rest.map((e) => e.type), [
        ClapHostEventType.paramValue,
        ClapHostEventType.paramValue,
        ClapHostEventType.paramGestureEnd,
      ]);
      expect(
        [...first, ...rest].map((e) => e.paramId).toSet(),
        {10},
      );
      expect(
        [...first, ...rest]
            .where((e) => e.type == ClapHostEventType.paramValue)
            .map((e) => e.value),
        [closeTo(2 / 3, 1e-9), closeTo(4 / 3, 1e-9), closeTo(2, 1e-9)],
      );
      expect(plugin.drainEvents(), isEmpty);
      plugin.dispose();
      expect(slot.index, lessThan(0));
    });
  });
}
