// ignore_for_file: experimental_member_use

import 'dart:convert';
import 'dart:ffi' as ffi;
import 'dart:io';

import 'package:ffi/ffi.dart';
import 'package:flutter_soloud/flutter_soloud.dart';
import 'package:flutter_soloud/src/clap/clap_host_web.dart' as web;
import 'package:flutter_soloud/src/clap/clap_slot_owners.dart';
import 'package:flutter_soloud/src/clap/clap_text.dart';
import 'package:flutter_test/flutter_test.dart';

/// Whether this run's native build should have CLAP hosting compiled in:
/// `--dart-define=EXPECT_CLAP=1` when the root package enables `enable_clap`
/// on macOS. Unset, the result only has to be self-consistent.
const _expectClap = String.fromEnvironment('EXPECT_CLAP');

// Only wrapper paths that return before the host marshals to the main
// thread: a test process need not service the main dispatch queue. The
// main-thread paths are covered live by
// example/integration_test/clap_wrapper_test.dart.
void main() {
  group('CLAP value types', () {
    test('parameter flags decode the CLAP_PARAM_* bits', () {
      ClapParamInfo info(int flags) => ClapParamInfo(
        paramId: 7,
        name: 'Gain',
        module: 'Out/',
        minValue: -24,
        maxValue: 24,
        defaultValue: 0,
        flags: flags,
      );
      final none = info(0);
      expect(
        [
          none.isStepped,
          none.isPeriodic,
          none.isHidden,
          none.isReadOnly,
          none.isBypass,
          none.isAutomatable,
          none.isEnum,
        ],
        everyElement(isFalse),
      );
      expect(info(1 << 0).isStepped, isTrue);
      expect(info(1 << 1).isPeriodic, isTrue);
      expect(info(1 << 2).isHidden, isTrue);
      expect(info(1 << 3).isReadOnly, isTrue);
      expect(info(1 << 4).isBypass, isTrue);
      expect(info(1 << 5).isAutomatable, isTrue);
      expect(info((1 << 16) | 1).isEnum, isTrue);
      expect(info(1 << 5).isStepped, isFalse);
    });

    test('normalized values map linearly, snap when stepped', () {
      ClapParamInfo info(double min, double max, [int flags = 0]) =>
          ClapParamInfo(
            paramId: 1,
            name: 'P',
            module: '',
            minValue: min,
            maxValue: max,
            defaultValue: min,
            flags: flags,
          );
      final gain = info(-24, 24);
      expect(gain.toNormalized(-24), 0);
      expect(gain.toNormalized(0), 0.5);
      expect(gain.toNormalized(24), 1);
      expect(gain.toNormalized(48), 1);
      expect(gain.toNormalized(-100), 0);
      expect(gain.fromNormalized(0.25), -12);
      expect(gain.fromNormalized(-1), -24);
      expect(gain.fromNormalized(2), 24);
      for (final n in [0.0, 0.1, 0.37, 0.5, 0.9, 1.0]) {
        expect(gain.toNormalized(gain.fromNormalized(n)), closeTo(n, 1e-12));
      }

      // Stepped, enum and bypass values snap to whole numbers.
      for (final flags in [1 << 0, (1 << 16) | 1, 1 << 4]) {
        final mode = info(0, 3, flags);
        expect(mode.fromNormalized(0.4), 1, reason: 'flags $flags');
        expect(mode.fromNormalized(0.5), 2, reason: 'flags $flags');
        expect(mode.toNormalized(1.4), closeTo(1 / 3, 1e-12));
      }

      // An empty or inverted range maps everything to its minimum.
      for (final empty in [info(5, 5), info(5, 1)]) {
        expect(empty.toNormalized(5), 0);
        expect(empty.fromNormalized(0.7), 5);
      }

      expect(() => gain.toNormalized(double.nan), throwsArgumentError);
      expect(() => gain.fromNormalized(double.infinity), throwsArgumentError);
    });

    test('param edit events carry their kind and values', () {
      const edit = ClapParamEditEvent(
        pluginHandle: 4,
        paramId: 10,
        kind: ClapParamEditKind.value,
        value: 3,
        normalizedValue: 0.75,
        isDontRecord: true,
      );
      expect(edit.toString(), contains('kind: value'));
      expect(edit.toString(), contains('dontRecord: true'));
      const gesture = ClapParamEditEvent(
        pluginHandle: 4,
        paramId: 10,
        kind: ClapParamEditKind.gestureBegin,
      );
      expect(gesture.value, isNull);
      expect(gesture.normalizedValue, isNull);
      expect(gesture.isDontRecord, isFalse);
    });

    test('event flags are read only for their own event type', () {
      ClapHostEvent event(ClapHostEventType type, int flags) =>
          ClapHostEvent(type: type, paramId: 3, value: 0.5, flags: flags);
      expect(event(ClapHostEventType.paramValue, 1).isDontRecord, isTrue);
      expect(event(ClapHostEventType.paramValue, 0).isDontRecord, isFalse);
      expect(event(ClapHostEventType.resync, 1).isOverflow, isTrue);
      expect(event(ClapHostEventType.resync, 1).isParamsReset, isFalse);
      expect(event(ClapHostEventType.resync, 2).isParamsReset, isTrue);
      expect(event(ClapHostEventType.resync, 2).isOverflow, isFalse);
      expect(event(ClapHostEventType.paramsRescan, 12).rescanFlags, 12);
      // The same bits on another type mean nothing.
      expect(event(ClapHostEventType.resync, 1).isDontRecord, isFalse);
      expect(event(ClapHostEventType.paramValue, 2).isParamsReset, isFalse);
      expect(event(ClapHostEventType.stateDirty, 8).rescanFlags, 0);
    });

    test('note end events decode their note', () {
      const end = ClapHostEvent(
        type: ClapHostEventType.noteEnd,
        paramId: 0xffffffff,
        value: 60,
        flags: (1 << 16) | 15,
      );
      expect(end.noteId, -1);
      expect(end.noteKey, 60);
      expect(end.noteChannel, 15);
      expect(end.notePortIndex, 1);
      // -1 (any) for the key, channel and port of a wildcard note.
      const any = ClapHostEvent(
        type: ClapHostEventType.noteEnd,
        paramId: 7,
        value: -1,
        flags: 0xffffffff,
      );
      expect(any.noteId, 7);
      expect(any.noteKey, -1);
      expect(any.noteChannel, -1);
      expect(any.notePortIndex, -1);
      // Other types carry no note.
      const value = ClapHostEvent(
        type: ClapHostEventType.paramValue,
        paramId: 3,
        value: 60,
        flags: 1,
      );
      expect(value.noteId, -1);
      expect(value.noteKey, -1);
      expect(value.noteChannel, -1);
      expect(value.notePortIndex, -1);
      expect(end.isDontRecord, isFalse);
    });

    test('note dialects map to the native values', () {
      expect(ClapNoteDialect.values.map((d) => d.value), [0, 1, 2]);
      for (final dialect in ClapNoteDialect.values) {
        expect(ClapNoteDialect.fromValue(dialect.value), dialect);
      }
      expect(ClapNoteDialect.fromValue(3), ClapNoteDialect.none);
      expect(ClapNoteDialect.fromValue(-1), ClapNoteDialect.none);
    });

    test('event types map to the native values', () {
      expect(
        ClapHostEventType.values.map((t) => t.value),
        List<int>.generate(11, (i) => i + 1),
      );
      for (final type in ClapHostEventType.values) {
        expect(ClapHostEventType.fromValue(type.value), type);
      }
      expect(ClapHostEventType.fromValue(0), isNull);
      expect(ClapHostEventType.fromValue(12), isNull);
    });

    test('plugin features classify effects and instruments', () {
      const eq = ClapPluginInfo(
        pluginId: 'com.example.eq',
        name: 'EQ',
        vendor: 'Example',
        version: '1.0',
        features: ['audio-effect', 'equalizer'],
      );
      const synth = ClapPluginInfo(
        pluginId: 'com.example.synth',
        name: 'Synth',
        vendor: '',
        version: '',
        features: ['instrument', 'synthesizer'],
      );
      expect(eq.isAudioEffect, isTrue);
      expect(eq.isInstrument, isFalse);
      expect(synth.isAudioEffect, isFalse);
      expect(synth.isInstrument, isTrue);
    });
  });

  group('CLAP text buffers', () {
    String read(List<int> bytes, int capacity) {
      final buffer = calloc<ffi.Uint8>(capacity);
      try {
        buffer.asTypedList(capacity).setAll(0, bytes.take(capacity));
        return readClapText(buffer.cast(), capacity);
      } finally {
        calloc.free(buffer);
      }
    }

    test('stop at the NUL and at the capacity', () {
      expect(
        read([...utf8.encode('Gain'), 0, ...utf8.encode('junk')], 16),
        'Gain',
      );
      expect(read(utf8.encode('Frequency'), 4), 'Freq');
      expect(read(const [], 8), isEmpty);
      expect(readClapText(ffi.nullptr, 8), isEmpty);
    });

    test('keep long Unicode text and survive a cut character', () {
      final long = 'Größe – 低音 🎚️ ' * 40;
      final bytes = utf8.encode(long);
      expect(bytes.length, greaterThan(1024));
      expect(read([...bytes, 0], bytes.length + 1), long);
      // The native side truncates by bytes: a character cut in half decodes
      // as U+FFFD instead of throwing.
      final euro = utf8.encode('€');
      expect(
        read([...utf8.encode('12 '), euro[0], euro[1], 0], 8),
        '12 \uFFFD',
      );
      expect(read([0xFF, 0x41, 0], 3), '\uFFFDA');
    });
  });

  group('CLAP slot owners', () {
    const sound = SoundHash(5);

    test('keys follow the native routing', () {
      expect(clapSlotKey(const CustomFilterGlobal(2)), (0, 0, 2));
      expect(
        clapSlotKey(const CustomFilterSingle(2, null, 0)),
        clapSlotKey(const CustomFilterGlobal(2)),
      );
      // A sound wins over the bus.
      expect(
        clapSlotKey(const CustomFilterSingle(2, sound, 9)),
        clapSlotKey(const CustomFilterSingle(2, sound, null)),
      );
      expect(clapSlotKey(const CustomFilterSingle(2, null, 9)), (0, 9, 2));
      expect(
        clapSlotKey(const CustomFilterSingle(3, sound, null)),
        isNot(clapSlotKey(const CustomFilterSingle(2, sound, null))),
      );
      // The C ABI takes unsigned 32-bit values.
      expect(
        clapSlotKey(const CustomFilterSingle(1, null, 0x100000007)),
        (0, 7, 1),
      );
      final wrapped = clapSlotKey(const CustomFilterSingle(1, null, -1));
      expect(wrapped, (0, 0xFFFFFFFF, 1));
    });

    test('only the current owner of the session releases a slot', () {
      final owners = ClapSlotOwners<Object>();
      final a = Object();
      final b = Object();
      const viaSound = CustomFilterSingle(0, sound, null);
      const viaSoundAndBus = CustomFilterSingle(0, sound, 4);

      owners
        ..bind(viaSound, a, 1)
        ..bind(viaSoundAndBus, b, 1);
      expect(owners.length, 1);
      expect(owners.release(viaSound, a, 1), isFalse);
      expect(owners.release(viaSound, b, 1), isTrue);
      expect(owners.release(viaSound, b, 1), isFalse);
      expect(owners.length, 0);

      // A binding of an earlier session is dropped, not released.
      owners
        ..bind(const CustomFilterGlobal(0), a, 1)
        ..bind(const CustomFilterGlobal(1), b, 1);
      expect(owners.length, 2);
      expect(owners.release(const CustomFilterGlobal(0), a, 2), isFalse);
      expect(owners.length, 0);
      owners.bind(const CustomFilterGlobal(0), a, 2);
      expect(owners.release(const CustomFilterGlobal(0), a, 2), isTrue);
    });
  });

  group('ClapHost without the main thread', () {
    final host = ClapHost();

    test('support agrees with the build and the version', () {
      if (_expectClap.isNotEmpty) {
        expect(host.isSupported, _expectClap == '1');
      }
      final version = host.hostClapVersion;
      if (host.isSupported) {
        expect(version.major, 1);
        expect(version.minor, greaterThanOrEqualTo(2));
      } else {
        expect(version, (major: 0, minor: 0, revision: 0));
      }
    });

    test('search paths are the macOS CLAP folders', () {
      if (Platform.isMacOS) {
        expect(host.standardSearchPaths.first, '/Library/Audio/Plug-Ins/CLAP');
        final home = Platform.environment['HOME'];
        if (home != null && home.isNotEmpty) {
          expect(
            host.standardSearchPaths,
            contains('$home/Library/Audio/Plug-Ins/CLAP'),
          );
        }
      } else {
        expect(host.standardSearchPaths, isEmpty);
      }
    });

    test('unloadAll with nothing loaded is a no-op', host.unloadAll);

    test('loadEffect needs support and an initialized engine', () {
      expect(SoLoud.instance.isInitialized, isFalse);
      expect(
        () => host.loadEffect(
          bundlePath: '/nonexistent/Mock.clap',
          slot: const CustomFilterGlobal(0),
          pluginId: 'mock.stereo',
        ),
        host.isSupported
            ? throwsA(isA<SoLoudNotInitializedException>())
            : throwsA(isA<SoLoudCppException>()),
      );
    });

    test('loadInstrument needs support and an initialized engine', () {
      expect(SoLoud.instance.isInitialized, isFalse);
      expect(
        () => host.loadInstrument(
          bundlePath: '/nonexistent/Mock.clap',
          pluginId: 'mock.synth',
        ),
        host.isSupported
            ? throwsA(isA<SoLoudNotInitializedException>())
            : throwsA(isA<SoLoudCppException>()),
      );
    });

    test('scan is empty when unsupported', () {
      if (!host.isSupported) {
        expect(host.scanBundle('/nonexistent/Mock.clap'), isEmpty);
      }
    });
  });

  group('web stub', () {
    final host = web.ClapHost();

    test('reports no support and rejects loads', () {
      expect(host.isSupported, isFalse);
      expect(host.hostClapVersion, (major: 0, minor: 0, revision: 0));
      expect(host.standardSearchPaths, isEmpty);
      expect(host.scanBundle('/any/Plugin.clap'), isEmpty);
      host.unloadAll();
      expect(
        () => host.loadEffect(
          bundlePath: '/any/Plugin.clap',
          slot: const CustomFilterGlobal(0),
        ),
        throwsA(isA<SoLoudCppException>()),
      );
      expect(
        () => host.loadInstrument(bundlePath: '/any/Plugin.clap'),
        throwsA(isA<SoLoudCppException>()),
      );
    });
  });
}
