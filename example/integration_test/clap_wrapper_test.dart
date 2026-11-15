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

    testWidgets('normalized values and a slot parameter drive the plugin', (
      _,
    ) async {
      const slot = CustomFilterGlobal(0);
      final plugin = host.loadEffect(bundlePath: bundle!, slot: slot);
      final param = plugin.parameters.firstWhere(
        (p) =>
            !p.isStepped &&
            !p.isReadOnly &&
            !p.isHidden &&
            p.maxValue > p.minValue,
      );
      final id = param.paramId;
      expect(plugin.parameterInfo(id), param);

      // Normalized set and get go through the plain range.
      plugin.setParameterNormalized(id, 0.2);
      expect(
        await _waitForValue(plugin, id, param.fromNormalized(0.2)),
        closeTo(param.fromNormalized(0.2), 1e-6),
      );
      expect(plugin.getParameterNormalized(id), closeTo(0.2, 1e-6));

      // A slot parameter drives the plugin parameter once it changes.
      slot.configureParameters(const [
        CustomFilterParamSpec(name: 'Drive', min: 0, max: 1, defaultValue: 0),
      ]);
      final slotParam =
          plugin.mapToSlotParameter(slotParamIndex: 1, paramId: id)
            ..value = 0.6;
      expect(
        await _waitForValue(plugin, id, param.fromNormalized(0.6)),
        closeTo(param.fromNormalized(0.6), 1e-6),
      );
      slotParam.fadeFilterParameter(
        to: 0.9,
        time: const Duration(milliseconds: 150),
      );
      expect(
        await _waitForValue(plugin, id, param.fromNormalized(0.9)),
        closeTo(param.fromNormalized(0.9), 1e-6),
      );

      // Unmapped, the slot no longer moves it.
      plugin.unmapSlotParameter(1);
      slotParam.value = 0.1;
      await Future<void>.delayed(const Duration(milliseconds: 200));
      expect(
        plugin.getParameterValue(id),
        closeTo(param.fromNormalized(0.9), 1e-6),
      );

      expect(
        () => plugin.mapToSlotParameter(slotParamIndex: 0, paramId: id),
        throwsArgumentError,
      );
      expect(
        () => plugin.mapToSlotParameter(slotParamIndex: 8, paramId: id),
        throwsArgumentError,
      );
      final unknown =
          plugin.parameters
              .map((p) => p.paramId)
              .fold(0, (a, b) => a > b ? a : b) +
          1;
      expect(
        () => plugin.mapToSlotParameter(slotParamIndex: 1, paramId: unknown),
        throwsA(isA<SoLoudCppException>()),
      );
      expect(
        () => plugin.getParameterNormalized(unknown),
        throwsA(isA<SoLoudCppException>()),
      );
      plugin.dispose();
      expect(
        () => plugin.mapToSlotParameter(slotParamIndex: 1, paramId: id),
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
      final subscription = plugin.events.listen((_) {});
      expect(plugin.isPollingEvents, isTrue);
      host.unloadAll();
      // The first drain finds the handle gone: polling stops for good, and a
      // new listener does not restart it.
      expect(plugin.drainEvents(), isEmpty);
      expect(plugin.isPollingEvents, isFalse);
      await subscription.cancel();
      final again = plugin.paramEdits.listen((_) {});
      expect(plugin.isPollingEvents, isFalse);
      await again.cancel();
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

    testWidgets('the editor opens, closes and reopens', (_) async {
      const slot = CustomFilterGlobal(6);
      final plugin = host.loadEffect(bundlePath: bundle!, slot: slot);
      debugPrint('CLAP editor: hasEditor ${plugin.hasEditor}');
      expect(plugin.hasEditor, isTrue);
      expect(plugin.isEditorOpen, isFalse);
      plugin.drainEvents();

      expect(plugin.openEditorWindow(title: 'CLAP editor test'), isTrue);
      expect(plugin.isEditorOpen, isTrue);
      // An open editor only comes to the front.
      expect(plugin.openEditorWindow(title: 'CLAP editor test'), isTrue);
      await Future<void>.delayed(const Duration(milliseconds: 300));
      plugin.closeEditorWindow();
      expect(plugin.isEditorOpen, isFalse);
      plugin.closeEditorWindow();

      expect(plugin.openEditorWindow(), isTrue);
      expect(plugin.isEditorOpen, isTrue);
      await Future<void>.delayed(const Duration(milliseconds: 300));
      // A host close is not reported.
      plugin.closeEditorWindow();
      expect(
        plugin.drainEvents().map((e) => e.type),
        isNot(contains(ClapHostEventType.editorClosed)),
      );

      // Dispose closes an open editor.
      expect(plugin.openEditorWindow(), isTrue);
      plugin.dispose();
      expect(plugin.isEditorOpen, isFalse);
      expect(plugin.hasEditor, isFalse);
      expect(plugin.openEditorWindow(), isFalse);
      expect(slot.index, lessThan(0));
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

    testWidgets('the streams deliver events and edits while listened to', (
      _,
    ) async {
      const slot = CustomFilterGlobal(2);
      final plugin = host.loadEffect(
        bundlePath: mockBundle!,
        slot: slot,
        pluginId: 'mock.ext',
      );
      final image = ffi.DynamicLibrary.open(
        '$mockBundle/Contents/MacOS/MockClap',
      );
      final emitGestures = image
          .lookupFunction<
            ffi.Void Function(ffi.Int, ffi.Int),
            void Function(int, int)
          >('mock_clap_emit_gestures');
      plugin.drainEvents();
      final gain = plugin.parameterInfo(10)!;

      final edits = <ClapParamEditEvent>[];
      final events = <ClapHostEvent>[];
      final editSub = plugin.paramEdits.listen(edits.add);
      final eventSub = plugin.events.listen(events.add);
      emitGestures(1, 3);
      final deadline = DateTime.now().add(const Duration(seconds: 3));
      while (edits.length < 5 && DateTime.now().isBefore(deadline)) {
        await Future<void>.delayed(const Duration(milliseconds: 20));
      }
      debugPrint('CLAP mock edits: $edits');
      expect(edits.map((e) => e.kind), [
        ClapParamEditKind.gestureBegin,
        ClapParamEditKind.value,
        ClapParamEditKind.value,
        ClapParamEditKind.value,
        ClapParamEditKind.gestureEnd,
      ]);
      expect(edits.map((e) => e.paramId).toSet(), {10});
      expect(edits.map((e) => e.pluginHandle).toSet(), {plugin.pluginHandle});
      expect(edits.map((e) => e.value), [
        isNull,
        closeTo(2 / 3, 1e-9),
        closeTo(4 / 3, 1e-9),
        closeTo(2, 1e-9),
        isNull,
      ]);
      expect(edits.map((e) => e.normalizedValue), [
        isNull,
        closeTo(gain.toNormalized(2 / 3), 1e-9),
        closeTo(gain.toNormalized(4 / 3), 1e-9),
        closeTo(1, 1e-9),
        isNull,
      ]);
      expect(
        events
            .where(
              (e) =>
                  e.type == ClapHostEventType.paramGestureBegin ||
                  e.type == ClapHostEventType.paramValue ||
                  e.type == ClapHostEventType.paramGestureEnd,
            )
            .length,
        5,
      );

      // A manual drain also feeds the listeners. The poll blocks the
      // isolate, so the stream timer cannot take the events first.
      final before = events.length;
      emitGestures(1, 1);
      var manual = <ClapHostEvent>[];
      final manualDeadline = DateTime.now().add(const Duration(seconds: 3));
      while (manual.length < 3 && DateTime.now().isBefore(manualDeadline)) {
        manual = [...manual, ...plugin.drainEvents()];
        if (manual.length < 3) {
          sleep(const Duration(milliseconds: 5));
        }
      }
      expect(manual.map((e) => e.type), [
        ClapHostEventType.paramGestureBegin,
        ClapHostEventType.paramValue,
        ClapHostEventType.paramGestureEnd,
      ]);
      await Future<void>.delayed(Duration.zero);
      expect(edits.length, 8);
      expect(edits.last.kind, ClapParamEditKind.gestureEnd);
      expect(events.sublist(before).map((e) => e.type), [
        ClapHostEventType.paramGestureBegin,
        ClapHostEventType.paramValue,
        ClapHostEventType.paramGestureEnd,
      ]);

      // With no listeners left, nothing drains on its own.
      await editSub.cancel();
      await eventSub.cancel();
      emitGestures(1, 1);
      await Future<void>.delayed(const Duration(milliseconds: 300));
      final queued = plugin.drainEvents();
      expect(
        queued.map((e) => e.type),
        containsAllInOrder([
          ClapHostEventType.paramGestureBegin,
          ClapHostEventType.paramValue,
          ClapHostEventType.paramGestureEnd,
        ]),
      );

      // Dispose closes both streams.
      final eventsDone = plugin.events.listen(null).asFuture<void>();
      final editsDone = plugin.paramEdits.listen(null).asFuture<void>();
      plugin.dispose();
      await Future.wait([
        eventsDone,
        editsDone,
      ]).timeout(const Duration(seconds: 1));
      expect(slot.index, lessThan(0));
    });

    testWidgets('a slot parameter drives a mock parameter', (_) async {
      final image = ffi.DynamicLibrary.open(
        '$mockBundle/Contents/MacOS/MockClap',
      );
      final violations = image
          .lookupFunction<ffi.Int Function(), int Function()>(
            'mock_clap_violations',
          );
      final violationsBefore = violations();
      const slot = CustomFilterGlobal(3);
      final plugin = host.loadEffect(
        bundlePath: mockBundle!,
        slot: slot,
        pluginId: 'mock.ext',
      );
      final gain = plugin.parameterInfo(10)!;
      expect(gain.maxValue, 2);
      slot.configureParameters(const [
        CustomFilterParamSpec(name: 'A', min: 0, max: 1, defaultValue: 0),
        CustomFilterParamSpec(name: 'B', min: 0, max: 1, defaultValue: 0),
      ]);
      // Mapped at once, though configureParameters just wrote the slot
      // values: mapping alone sends nothing, the plugin keeps its default.
      final param = plugin.mapToSlotParameter(slotParamIndex: 2, paramId: 10);
      expect(param.attributeId, 2);
      expect(param.slotIndex, slot.slotIndex);
      await Future<void>.delayed(const Duration(milliseconds: 100));
      expect(plugin.getParameterValue(10), gain.defaultValue);

      // A slot value set just before mapping is the baseline: not sent.
      final other = CustomFilterParam(null, null, slot.slotIndex, 1, 0, 1)
        ..value = 0.75;
      plugin.mapToSlotParameter(slotParamIndex: 1, paramId: 20);
      await Future<void>.delayed(const Duration(milliseconds: 100));
      final mode = plugin.parameterInfo(20)!;
      expect(plugin.getParameterValue(20), mode.defaultValue);
      // A change after mapping is: 0.75 -> 1 is 3 on the 0..3 enum.
      other.value = 1;
      expect(await _waitForValue(plugin, 20, 3), 3);
      plugin.unmapSlotParameter(1);

      param.value = 0.25;
      expect(await _waitForValue(plugin, 10, 0.5), closeTo(0.5, 1e-9));
      param.value = 1;
      expect(await _waitForValue(plugin, 10, 2), closeTo(2, 1e-9));

      // A read-only parameter is refused.
      final meter = plugin.parameters.firstWhere((p) => p.isReadOnly);
      expect(
        () => plugin.mapToSlotParameter(
          slotParamIndex: 1,
          paramId: meter.paramId,
        ),
        throwsA(isA<SoLoudCppException>()),
      );

      plugin.unmapSlotParameter(2);
      param.value = 0;
      await Future<void>.delayed(const Duration(milliseconds: 150));
      expect(plugin.getParameterValue(10), 2);
      expect(violations(), violationsBefore);
      plugin.dispose();
      expect(slot.index, lessThan(0));
    });

    testWidgets('the editor follows the gui modes the plugin offers', (
      _,
    ) async {
      final image = ffi.DynamicLibrary.open(
        '$mockBundle/Contents/MacOS/MockClap',
      );
      final setGuiApis = image
          .lookupFunction<ffi.Void Function(ffi.Int), void Function(int)>(
            'mock_clap_set_gui_apis',
          );
      final guiLive = image.lookupFunction<ffi.Int Function(), int Function()>(
        'mock_clap_gui_live',
      );
      final violations = image
          .lookupFunction<ffi.Int Function(), int Function()>(
            'mock_clap_violations',
          );
      final violationsBefore = violations();
      const slot = CustomFilterGlobal(1);
      try {
        // No gui extension: no editor.
        setGuiApis(0);
        final plain = host.loadEffect(
          bundlePath: mockBundle!,
          slot: slot,
          pluginId: 'mock.ext',
        );
        expect(plain.hasEditor, isFalse);
        expect(plain.openEditorWindow(title: 'Mock'), isFalse);
        expect(plain.isEditorOpen, isFalse);
        plain
          ..closeEditorWindow()
          ..dispose();

        // Embedded in a host window, then the plugin's own window.
        for (final apis in [1, 2]) {
          setGuiApis(apis);
          final plugin = host.loadEffect(
            bundlePath: mockBundle,
            slot: slot,
            pluginId: 'mock.ext',
          );
          expect(plugin.hasEditor, isTrue, reason: 'apis $apis');
          plugin.drainEvents();
          expect(plugin.openEditorWindow(title: 'Mock $apis'), isTrue);
          expect(plugin.isEditorOpen, isTrue);
          expect(guiLive(), 1);
          expect(plugin.openEditorWindow(title: 'Mock $apis'), isTrue);
          expect(guiLive(), 1);
          plugin.closeEditorWindow();
          expect(plugin.isEditorOpen, isFalse);
          expect(guiLive(), 0);
          expect(plugin.openEditorWindow(), isTrue);
          expect(guiLive(), 1);
          plugin.closeEditorWindow();
          expect(
            plugin.drainEvents().map((e) => e.type),
            isNot(contains(ClapHostEventType.editorClosed)),
          );
          // Dispose destroys an open gui.
          expect(plugin.openEditorWindow(), isTrue);
          plugin.dispose();
          expect(guiLive(), 0);
          expect(plugin.isEditorOpen, isFalse);
        }
        expect(violations(), violationsBefore);
      } finally {
        setGuiApis(0);
      }
      expect(slot.index, lessThan(0));
    });

    testWidgets('an instrument plays notes as an audio source', (_) async {
      final image = ffi.DynamicLibrary.open(
        '$mockBundle/Contents/MacOS/MockClap',
      );
      final noteCount = image
          .lookupFunction<ffi.Int Function(), int Function()>(
            'mock_clap_note_count',
          );
      final violations = image
          .lookupFunction<ffi.Int Function(), int Function()>(
            'mock_clap_violations',
          );
      final violationsBefore = violations();
      final notesBefore = noteCount();

      final instruments = host
          .scanBundle(mockBundle!)
          .where((p) => p.isInstrument)
          .map((p) => p.pluginId);
      expect(instruments, [
        'mock.synth',
        'mock.synth_midi',
        'mock.synth_midi2',
      ]);

      // Without an id: the bundle's first instrument.
      final synth = host.loadInstrument(bundlePath: mockBundle);
      expect(synth.noteDialect, ClapNoteDialect.clap);
      final voice = SoLoud.instance.play(synth.audioSource);
      synth.noteOn(pitch: 60, velocity: 0.5, channel: 2, sampleOffset: 7);

      Future<bool> waitFor(bool Function() done) async {
        final deadline = DateTime.now().add(const Duration(seconds: 3));
        while (!done() && DateTime.now().isBefore(deadline)) {
          await Future<void>.delayed(const Duration(milliseconds: 20));
        }
        return done();
      }

      // The playing source renders the plugin, which takes the note.
      expect(await waitFor(() => noteCount() == notesBefore + 1), isTrue);
      synth.noteOff(pitch: 60, channel: 2);
      expect(await waitFor(() => noteCount() == notesBefore + 2), isTrue);
      final events = <ClapHostEvent>[];
      expect(
        await waitFor(() {
          events.addAll(synth.drainEvents());
          return events.any((e) => e.type == ClapHostEventType.noteEnd);
        }),
        isTrue,
      );
      final end = events.firstWhere(
        (e) => e.type == ClapHostEventType.noteEnd,
      );
      expect(end.noteId, -1);
      expect(end.noteKey, 60);
      expect(end.noteChannel, 2);
      expect(end.notePortIndex, 0);

      // Out of range: refused.
      expect(
        () => synth.noteOn(pitch: 128),
        throwsA(isA<SoLoudCppException>()),
      );
      expect(
        () => synth.noteOn(pitch: 60, channel: 16),
        throwsA(isA<SoLoudCppException>()),
      );
      expect(
        () => synth.noteOn(pitch: 60, sampleOffset: -1),
        throwsA(isA<SoLoudCppException>()),
      );
      // Values that would narrow into range in the FFI call.
      expect(
        () => synth.noteOn(pitch: 0x10000003c),
        throwsA(isA<SoLoudCppException>()),
      );
      expect(
        () => synth.noteOff(pitch: 60, channel: 0x100000002),
        throwsA(isA<SoLoudCppException>()),
      );
      expect(
        () => synth.noteOn(pitch: -1),
        throwsA(isA<SoLoudCppException>()),
      );
      await Future<void>.delayed(const Duration(milliseconds: 100));
      expect(noteCount(), notesBefore + 2);

      await SoLoud.instance.stop(voice);
      synth.dispose();
      expect(synth.isDisposed, isTrue);
      expect(synth.noteDialect, ClapNoteDialect.none);
      // A no-op after dispose.
      synth
        ..noteOn(pitch: 60)
        ..noteOff(pitch: 60);

      final midi = host.loadInstrument(
        bundlePath: mockBundle,
        pluginId: 'mock.synth_midi',
        channels: Channels.mono,
      );
      expect(midi.noteDialect, ClapNoteDialect.midi);
      midi.dispose();

      // No dialect the host speaks: it loads, notes are refused.
      final midi2 = host.loadInstrument(
        bundlePath: mockBundle,
        pluginId: 'mock.synth_midi2',
      );
      expect(midi2.noteDialect, ClapNoteDialect.none);
      expect(
        () => midi2.noteOn(pitch: 60),
        throwsA(isA<SoLoudCppException>()),
      );
      midi2.dispose();
      expect(violations(), violationsBefore);
    });
  });
}
