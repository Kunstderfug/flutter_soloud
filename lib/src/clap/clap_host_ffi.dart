import 'dart:async';
import 'dart:ffi' as ffi;
import 'dart:io';

import 'package:ffi/ffi.dart';
import 'package:flutter/foundation.dart';
import 'package:flutter_soloud/src/audio_source.dart';
import 'package:flutter_soloud/src/bindings/flutter_soloud_ffigen.dart'
    as ffigen;
import 'package:flutter_soloud/src/bindings/soloud_controller.dart';
import 'package:flutter_soloud/src/clap/clap_slot_owners.dart';
import 'package:flutter_soloud/src/clap/clap_text.dart';
import 'package:flutter_soloud/src/clap/clap_types.dart';
import 'package:flutter_soloud/src/enums.dart';
import 'package:flutter_soloud/src/exceptions/exceptions.dart';
import 'package:flutter_soloud/src/filters/custom_filter.dart';
import 'package:flutter_soloud/src/soloud.dart';
import 'package:flutter_soloud/src/sound_handle.dart';
import 'package:meta/meta.dart';

export 'package:flutter_soloud/src/clap/clap_types.dart';

/// Converts a native error and throws it unless it is
/// [PlayerErrors.noError].
void _check(ffigen.PlayerErrors error) {
  final err = PlayerErrors.values[error.value];
  if (err != PlayerErrors.noError) {
    throw SoLoudCppException.fromPlayerError(err);
  }
}

/// The [ClapEffectPlugin] last bound to each native slot (see
/// [ClapSlotOwners]). Binds by other means (a VST3 plugin or a custom
/// callback) are not seen: dispose a CLAP plugin before its slot is used for
/// anything else.
final _slotOwners = ClapSlotOwners<ClapEffectPlugin>();

/// Scans and loads native CLAP audio effects and instruments (macOS only for
/// now).
///
/// Requires `enable_clap: macos` (or `true`) under
/// `hooks.user_defines.flutter_soloud` in the application's `pubspec.yaml`.
///
/// Scanning, loading, unloading, reading a parameter's value or text and
/// saving or loading state run on, and wait for, the platform main thread.
/// Call them from the root isolate's thread or another thread that holds no
/// lock the main thread needs.
@experimental
class ClapHost {
  /// Creates a [ClapHost] instance.
  @experimental
  ClapHost();

  /// Whether CLAP hosting is compiled in and supported on this platform.
  @experimental
  bool get isSupported => ffigen.soloud_clapIsSupported() != 0;

  /// The CLAP version the host was built against, or `(0, 0, 0)` when CLAP
  /// hosting is not compiled in.
  @experimental
  ({int major, int minor, int revision}) get hostClapVersion {
    final out = calloc<ffi.UnsignedInt>(3);
    try {
      ffigen.soloud_clapGetHostClapVersion(out, out + 1, out + 2);
      return (major: out[0], minor: out[1], revision: out[2]);
    } finally {
      calloc.free(out);
    }
  }

  /// Standard directories where `.clap` bundles are installed:
  /// `/Library/Audio/Plug-Ins/CLAP` and `~/Library/Audio/Plug-Ins/CLAP` on
  /// macOS, and none elsewhere.
  @experimental
  List<String> get standardSearchPaths {
    if (!Platform.isMacOS) {
      return const [];
    }
    final home = Platform.environment['HOME'];
    return [
      '/Library/Audio/Plug-Ins/CLAP',
      if (home != null && home.isNotEmpty) '$home/Library/Audio/Plug-Ins/CLAP',
    ];
  }

  /// Lists the plugins inside the `.clap` bundle at [bundlePath].
  ///
  /// Returns an empty list when CLAP hosting is not supported. Throws a
  /// [SoLoudCppException] when the bundle cannot be opened.
  @experimental
  List<ClapPluginInfo> scanBundle(String bundlePath) {
    if (!isSupported) {
      return const [];
    }
    return using((arena) {
      final path = bundlePath.toNativeUtf8(allocator: arena).cast<ffi.Char>();
      final outCount = arena<ffi.Int>();
      final outIds = arena<ffi.Pointer<ffi.Pointer<ffi.Char>>>();
      final outNames = arena<ffi.Pointer<ffi.Pointer<ffi.Char>>>();
      final outVendors = arena<ffi.Pointer<ffi.Pointer<ffi.Char>>>();
      final outVersions = arena<ffi.Pointer<ffi.Pointer<ffi.Char>>>();
      final outFeatures = arena<ffi.Pointer<ffi.Pointer<ffi.Char>>>();
      _check(
        ffigen.soloud_clapScanBundle(
          path,
          outCount,
          outIds,
          outNames,
          outVendors,
          outVersions,
          outFeatures,
        ),
      );
      final count = outCount.value;
      final ids = outIds.value;
      final names = outNames.value;
      final vendors = outVendors.value;
      final versions = outVersions.value;
      final features = outFeatures.value;
      try {
        String read(ffi.Pointer<ffi.Pointer<ffi.Char>> array, int i) {
          if (array == ffi.nullptr) {
            return '';
          }
          final s = array[i];
          return s == ffi.nullptr
              ? ''
              : readClapText(s, s.cast<Utf8>().length);
        }

        return List<ClapPluginInfo>.unmodifiable([
          for (var i = 0; i < count; i++)
            ClapPluginInfo(
              pluginId: read(ids, i),
              name: read(names, i),
              vendor: read(vendors, i),
              version: read(versions, i),
              features: List<String>.unmodifiable(
                read(features, i).split(';').where((f) => f.isNotEmpty),
              ),
            ),
        ]);
      } finally {
        ffigen.soloud_clapFreeScanResults(
          count,
          ids,
          names,
          vendors,
          versions,
          features,
        );
      }
    });
  }

  /// Unloads every loaded CLAP plugin and releases the cached modules.
  ///
  /// Plugin objects returned earlier become inert: their [CustomFilterSlot]s
  /// pass audio through unchanged, and their handles never resolve to a
  /// plugin loaded later. Call `dispose()` on them to also release the slots.
  @experimental
  void unloadAll() {
    if (!isSupported) {
      return;
    }
    ffigen.soloud_clapHostUnloadAll();
  }

  /// Loads the CLAP audio effect [pluginId] of the bundle at [bundlePath]
  /// and runs it in [slot] (a [CustomFilterSlot] `0..7` on the global or a
  /// bus filter chain), activating the slot when it is not active.
  ///
  /// Without [pluginId] the first plugin that declares `audio-effect` is
  /// used, or else the first that does not declare `instrument`.
  ///
  /// The plugin is set up for [sampleRate], which must be the rate of the
  /// audio it filters: it defaults to the engine's ([SoLoud.sampleRate]).
  /// [maxBlockSize] is the most frames per process call (`1..65536`) and
  /// [channels] the width of its main input and output.
  ///
  /// A plugin already bound to [slot] stays loaded but loses the slot: it
  /// no longer unbinds it when disposed.
  ///
  /// Throws a [SoLoudCppException] when the plugin cannot be loaded or the
  /// slot cannot be bound; nothing stays loaded then, and the slot is left
  /// as it was.
  @experimental
  ClapEffectPlugin loadEffect({
    required String bundlePath,
    required CustomFilterSlot slot,
    String? pluginId,
    int? sampleRate,
    int maxBlockSize = 2048,
    Channels channels = Channels.stereo,
  }) {
    if (!isSupported) {
      throw SoLoudCppException.fromPlayerError(PlayerErrors.invalidParameter);
    }
    if (!SoLoud.instance.isInitialized) {
      throw const SoLoudNotInitializedException();
    }
    final id = (pluginId != null && pluginId.isNotEmpty)
        ? pluginId
        : _defaultEffectId(bundlePath);
    final handle = using((arena) {
      final outHandle = arena<ffi.Int>();
      final error = ffigen.soloud_clapLoadEffect(
        bundlePath.toNativeUtf8(allocator: arena).cast(),
        id.toNativeUtf8(allocator: arena).cast(),
        sampleRate ?? SoLoud.instance.sampleRate,
        maxBlockSize,
        channels.count,
        outHandle,
      );
      _check(error);
      if (outHandle.value <= 0) {
        throw SoLoudCppException.fromPlayerError(PlayerErrors.fileLoadFailed);
      }
      return outHandle.value;
    });

    var activatedHere = false;
    try {
      if (slot.index < 0) {
        slot.activate();
        activatedHere = true;
      }
      slot.setNativeCallback(
        callbackAddress: ffigen.soloud_clapGetFilterCallbackPtr().address,
        userdataAddress: ffigen.soloud_clapGetPluginUserData(handle).address,
      );
    } catch (_) {
      // Leave the slot as it was found: a slot activated here is removed
      // again, and a slot that was active keeps its binding (the native side
      // only replaces it on success).
      if (activatedHere) {
        try {
          slot.deactivate();
        } catch (_) {}
      }
      ffigen.soloud_clapUnloadPlugin(handle);
      rethrow;
    }
    final plugin = ClapEffectPlugin._(pluginHandle: handle, slot: slot);
    _slotOwners.bind(slot, plugin, SoLoud.instance.engineSession);
    return plugin;
  }

  /// Loads the CLAP instrument [pluginId] of the bundle at [bundlePath] and
  /// wraps it in a playable [ClapInstrumentPlugin.audioSource].
  ///
  /// Without [pluginId] the first plugin that declares `instrument` is used.
  ///
  /// The plugin renders at [sampleRate] (the engine's, [SoLoud.sampleRate],
  /// by default), which is also the source's rate. [maxBlockSize] is the
  /// most frames per process call (`1..65536`) and [channels] the source's
  /// width: channels past the plugin's main output are silent. The plugin's
  /// audio inputs are silent.
  ///
  /// Throws a [SoLoudCppException] when the plugin cannot be loaded or the
  /// source cannot be created; nothing stays loaded then.
  @experimental
  ClapInstrumentPlugin loadInstrument({
    required String bundlePath,
    String? pluginId,
    int? sampleRate,
    int maxBlockSize = 2048,
    Channels channels = Channels.stereo,
  }) {
    if (!isSupported) {
      throw SoLoudCppException.fromPlayerError(PlayerErrors.invalidParameter);
    }
    if (!SoLoud.instance.isInitialized) {
      throw const SoLoudNotInitializedException();
    }
    final id = (pluginId != null && pluginId.isNotEmpty)
        ? pluginId
        : _defaultInstrumentId(bundlePath);
    final rate = sampleRate ?? SoLoud.instance.sampleRate;
    final handle = using((arena) {
      final outHandle = arena<ffi.Int>();
      final error = ffigen.soloud_clapLoadInstrument(
        bundlePath.toNativeUtf8(allocator: arena).cast(),
        id.toNativeUtf8(allocator: arena).cast(),
        rate,
        maxBlockSize,
        channels.count,
        outHandle,
      );
      _check(error);
      if (outHandle.value <= 0) {
        throw SoLoudCppException.fromPlayerError(PlayerErrors.fileLoadFailed);
      }
      return outHandle.value;
    });

    try {
      final source = SoLoud.instance.createCustomAudioSource(
        sampleRate: rate,
        channels: channels,
        dspCallbackAddress: ffigen.soloud_clapGetSourceCallbackPtr().address,
        userdataAddress: ffigen.soloud_clapGetPluginUserData(handle).address,
        // One plugin renders for every voice: with two voices it would run
        // twice per mix.
        singleInstance: true,
      );
      return ClapInstrumentPlugin._(pluginHandle: handle, audioSource: source);
    } catch (_) {
      ffigen.soloud_clapUnloadPlugin(handle);
      rethrow;
    }
  }

  String _defaultInstrumentId(String bundlePath) {
    final chosen = scanBundle(
      bundlePath,
    ).where((p) => p.isInstrument).firstOrNull;
    if (chosen == null) {
      throw SoLoudCppException.fromPlayerError(PlayerErrors.invalidParameter);
    }
    return chosen.pluginId;
  }

  String _defaultEffectId(String bundlePath) {
    final plugins = scanBundle(bundlePath);
    final chosen =
        plugins.where((p) => p.isAudioEffect).firstOrNull ??
        plugins.where((p) => !p.isInstrument).firstOrNull;
    if (chosen == null) {
      throw SoLoudCppException.fromPlayerError(PlayerErrors.invalidParameter);
    }
    return chosen.pluginId;
  }
}

/// What a loaded CLAP plugin offers whether it is an effect
/// ([ClapEffectPlugin]) or an instrument ([ClapInstrumentPlugin]).
///
/// Parameter values are plain: in the plugin's own units, within
/// [ClapParamInfo.minValue]..[ClapParamInfo.maxValue]. The `*Normalized`
/// members convert with [ClapParamInfo.toNormalized] and
/// [ClapParamInfo.fromNormalized].
///
/// The plugin's events are taken with [drainEvents], or delivered by the
/// [events] and [paramEdits] streams, which drain on a timer while they have
/// listeners. Each event is taken once: every event [drainEvents] returns
/// is also delivered to the streams' current listeners.
@experimental
abstract class ClapPluginBase {
  ClapPluginBase._(this.pluginHandle)
    : _parameters = _queryParameters(pluginHandle) {
    _events = StreamController<ClapHostEvent>.broadcast(
      onListen: _updateDrainTimer,
      onCancel: _updateDrainTimer,
    );
    _paramEdits = StreamController<ClapParamEditEvent>.broadcast(
      onListen: _updateDrainTimer,
      onCancel: _updateDrainTimer,
    );
  }

  /// How often the streams drain while they have listeners.
  static const Duration drainInterval = Duration(milliseconds: 16);

  /// The native plugin handle.
  final int pluginHandle;

  List<ClapParamInfo> _parameters;
  bool _disposed = false;
  late final StreamController<ClapHostEvent> _events;
  late final StreamController<ClapParamEditEvent> _paramEdits;
  Timer? _drainTimer;

  /// Set once draining reports the native handle gone (after
  /// [ClapHost.unloadAll]): nothing will arrive, so polling stops for good.
  bool _nativeGone = false;

  /// Whether [dispose] was called.
  bool get isDisposed => _disposed;

  /// Whether the [events] and [paramEdits] drain timer is running.
  @visibleForTesting
  bool get isPollingEvents => _drainTimer != null;

  /// The plugin's latency in frames, or 0.
  int get latencySamples =>
      _disposed ? 0 : ffigen.soloud_clapGetLatencySamples(pluginHandle);

  /// The plugin's parameters as last read.
  ///
  /// [drainEvents] refreshes the list when the plugin reports a change. The
  /// list is empty while the plugin rebuilds it: a
  /// [ClapHostEventType.resync] with [ClapHostEvent.isParamsReset] follows
  /// once it is settled.
  List<ClapParamInfo> get parameters => _parameters;

  /// Reads the parameter list again and updates [parameters].
  List<ClapParamInfo> refreshParameters() {
    if (_disposed) {
      return const [];
    }
    return _parameters = _queryParameters(pluginHandle);
  }

  static List<ClapParamInfo> _queryParameters(int handle) {
    final count = ffigen.soloud_clapGetParameterCount(handle);
    if (count <= 0) {
      return const [];
    }
    // CLAP_NAME_SIZE and CLAP_PATH_SIZE.
    const nameLen = 256;
    const moduleLen = 1024;
    return using((arena) {
      final outId = arena<ffi.UnsignedInt>();
      final outName = arena<ffi.Char>(nameLen);
      final outModule = arena<ffi.Char>(moduleLen);
      final outMin = arena<ffi.Double>();
      final outMax = arena<ffi.Double>();
      final outDefault = arena<ffi.Double>();
      final outFlags = arena<ffi.UnsignedInt>();
      final list = <ClapParamInfo>[];
      for (var i = 0; i < count; i++) {
        final error = ffigen.soloud_clapGetParameterInfo(
          handle,
          i,
          outId,
          outName,
          nameLen,
          outModule,
          moduleLen,
          outMin,
          outMax,
          outDefault,
          outFlags,
        );
        if (error != ffigen.PlayerErrors.noError) {
          // The list went pending mid-read: a resync follows once settled.
          return const <ClapParamInfo>[];
        }
        list.add(
          ClapParamInfo(
            paramId: outId.value,
            name: readClapText(outName, nameLen),
            module: readClapText(outModule, moduleLen),
            minValue: outMin.value,
            maxValue: outMax.value,
            defaultValue: outDefault.value,
            flags: outFlags.value,
          ),
        );
      }
      return List<ClapParamInfo>.unmodifiable(list);
    });
  }

  /// The plugin's current plain value of [paramId].
  double getParameterValue(int paramId) {
    _checkNotDisposed();
    return using((arena) {
      final out = arena<ffi.Double>();
      _check(ffigen.soloud_clapGetParameterValue(pluginHandle, paramId, out));
      return out.value;
    });
  }

  /// Formats the plain [value] of [paramId] the plugin's way (for example
  /// `"-4.2 dB"`).
  String valueToText(int paramId, double value) {
    _checkNotDisposed();
    const textLen = 512;
    return using((arena) {
      final out = arena<ffi.Char>(textLen);
      _check(
        ffigen.soloud_clapParameterValueToText(
          pluginHandle,
          paramId,
          value,
          out,
          textLen,
        ),
      );
      return readClapText(out, textLen);
    });
  }

  /// Parses [text] into a plain value of [paramId] the plugin's way.
  double textToValue(int paramId, String text) {
    _checkNotDisposed();
    return using((arena) {
      final out = arena<ffi.Double>();
      _check(
        ffigen.soloud_clapParameterTextToValue(
          pluginHandle,
          paramId,
          text.toNativeUtf8(allocator: arena).cast(),
          out,
        ),
      );
      return out.value;
    });
  }

  /// Queues the plain [value] (clamped to the range) for [paramId]. The
  /// plugin receives it with its next processed block. Never waits for the
  /// main thread.
  ///
  /// Throws a [SoLoudCppException] for an unknown parameter, or while the
  /// parameter list is being rebuilt or the queue is full.
  void setParameterValue(int paramId, double value) {
    _checkNotDisposed();
    _check(ffigen.soloud_clapSetParameterValue(pluginHandle, paramId, value));
  }

  /// The [ClapParamInfo] of [paramId] in [parameters], or null.
  ClapParamInfo? parameterInfo(int paramId) {
    for (final info in _parameters) {
      if (info.paramId == paramId) {
        return info;
      }
    }
    return null;
  }

  ClapParamInfo _requireInfo(int paramId) {
    final info = parameterInfo(paramId);
    if (info == null) {
      throw SoLoudCppException.fromPlayerError(PlayerErrors.invalidParameter);
    }
    return info;
  }

  /// The plugin's current value of [paramId], normalized to `0..1` with
  /// [ClapParamInfo.toNormalized].
  ///
  /// Throws a [SoLoudCppException] for a parameter missing from
  /// [parameters].
  double getParameterNormalized(int paramId) {
    _checkNotDisposed();
    final info = _requireInfo(paramId);
    return info.toNormalized(getParameterValue(paramId));
  }

  /// Queues the normalized [value] (`0..1`) for [paramId], converted with
  /// [ClapParamInfo.fromNormalized]. See [setParameterValue].
  ///
  /// Throws a [SoLoudCppException] for a parameter missing from
  /// [parameters], and an [ArgumentError] for a value that is not finite.
  void setParameterNormalized(int paramId, double value) {
    _checkNotDisposed();
    final info = _requireInfo(paramId);
    setParameterValue(paramId, info.fromNormalized(value));
  }

  /// Saves the plugin's state.
  ///
  /// Throws a [SoLoudNotImplementedException] when the plugin has no CLAP
  /// `state` extension, and another [SoLoudCppException] when it fails to
  /// save.
  Uint8List saveState() {
    _checkNotDisposed();
    return using((arena) {
      final outData = arena<ffi.Pointer<ffi.UnsignedChar>>();
      final outSize = arena<ffi.UnsignedInt>();
      _check(ffigen.soloud_clapGetState(pluginHandle, outData, outSize));
      final data = outData.value;
      final size = outSize.value;
      if (data == ffi.nullptr) {
        return Uint8List(0);
      }
      try {
        return Uint8List.fromList(data.cast<ffi.Uint8>().asTypedList(size));
      } finally {
        ffigen.nativeFree(data.cast());
      }
    });
  }

  /// Loads a state saved by [saveState]. Parameter values queued before the
  /// call are dropped, and a [ClapHostEventType.paramsRescan] follows.
  ///
  /// Throws a [SoLoudNotImplementedException] when the plugin has no CLAP
  /// `state` extension, and another [SoLoudCppException] when it rejects the
  /// state.
  void restoreState(Uint8List state) {
    _checkNotDisposed();
    using((arena) {
      final data = arena<ffi.UnsignedChar>(state.isEmpty ? 1 : state.length);
      data.cast<ffi.Uint8>().asTypedList(state.length).setAll(0, state);
      _check(ffigen.soloud_clapSetState(pluginHandle, data, state.length));
    });
  }

  /// Takes up to [maxEvents] of the plugin's pending events, oldest first.
  ///
  /// Call it regularly (for example once per frame) and again while it
  /// returns [maxEvents] events. Returns no events once the plugin is gone
  /// (after [dispose] or [ClapHost.unloadAll]). A
  /// [ClapHostEventType.paramsRescan] that
  /// changes the list (`CLAP_PARAM_RESCAN_INFO` or `_ALL`) and a resync with
  /// [ClapHostEvent.isParamsReset] refresh [parameters] before they are
  /// returned. Never waits for the main thread.
  ///
  /// The events are also delivered to the current listeners of [events]
  /// and, for parameter changes, [paramEdits].
  List<ClapHostEvent> drainEvents({int maxEvents = 1024}) {
    if (_disposed || _nativeGone || maxEvents <= 0) {
      return const [];
    }
    const chunk = 256;
    final events = <ClapHostEvent>[];
    var refresh = false;
    using((arena) {
      final types = arena<ffi.Int>(chunk);
      final ids = arena<ffi.UnsignedInt>(chunk);
      final values = arena<ffi.Double>(chunk);
      final flags = arena<ffi.UnsignedInt>(chunk);
      final outCount = arena<ffi.Int>();
      while (events.length < maxEvents) {
        final capacity = maxEvents - events.length < chunk
            ? maxEvents - events.length
            : chunk;
        final error = ffigen.soloud_clapDrainEvents(
          pluginHandle,
          capacity,
          types,
          ids,
          values,
          flags,
          outCount,
        );
        // invalidParameter once the handle is gone (after unloadAll): there
        // is nothing left to take, now or later.
        if (error != ffigen.PlayerErrors.noError) {
          if (error == ffigen.PlayerErrors.invalidParameter) {
            _nativeGone = true;
          }
          break;
        }
        final count = outCount.value;
        for (var i = 0; i < count; i++) {
          final type = ClapHostEventType.fromValue(types[i]);
          if (type == null) {
            // A type this version does not know: skip it.
            continue;
          }
          final event = ClapHostEvent(
            type: type,
            paramId: ids[i],
            value: values[i],
            flags: flags[i],
          );
          if (event.isParamsReset ||
              (event.rescanFlags & ((1 << 2) | (1 << 3))) != 0) {
            refresh = true;
          }
          events.add(event);
        }
        if (count < capacity) {
          break;
        }
      }
    });
    if (refresh) {
      refreshParameters();
    }
    if (_nativeGone) {
      _updateDrainTimer();
    }
    _deliver(events);
    return events;
  }

  /// Every event the plugin reports, as [drainEvents] would return it.
  ///
  /// While this stream or [paramEdits] has a listener, the plugin's events
  /// are drained every [drainInterval]. Closed by [dispose].
  Stream<ClapHostEvent> get events => _events.stream;

  /// The parameter changes the plugin makes itself, usually from its
  /// editor: value changes and gesture starts and ends, in order.
  ///
  /// Drains like [events]. A [ClapHostEventType.resync] on [events] means
  /// edits were lost. Closed by [dispose].
  Stream<ClapParamEditEvent> get paramEdits => _paramEdits.stream;

  void _deliver(List<ClapHostEvent> events) {
    if (events.isEmpty) {
      return;
    }
    if (_events.hasListener && !_events.isClosed) {
      events.forEach(_events.add);
    }
    if (!_paramEdits.hasListener || _paramEdits.isClosed) {
      return;
    }
    for (final event in events) {
      final kind = switch (event.type) {
        ClapHostEventType.paramValue => ClapParamEditKind.value,
        ClapHostEventType.paramGestureBegin => ClapParamEditKind.gestureBegin,
        ClapHostEventType.paramGestureEnd => ClapParamEditKind.gestureEnd,
        _ => null,
      };
      if (kind == null) {
        continue;
      }
      double? value;
      double? normalized;
      if (kind == ClapParamEditKind.value) {
        value = event.value;
        final info = parameterInfo(event.paramId);
        if (info != null && value.isFinite) {
          normalized = info.toNormalized(value);
        }
      }
      _paramEdits.add(
        ClapParamEditEvent(
          pluginHandle: pluginHandle,
          paramId: event.paramId,
          kind: kind,
          value: value,
          normalizedValue: normalized,
          isDontRecord: event.isDontRecord,
        ),
      );
    }
  }

  void _updateDrainTimer() {
    final wanted =
        !_disposed &&
        !_nativeGone &&
        (_events.hasListener || _paramEdits.hasListener);
    if (wanted && _drainTimer == null) {
      _drainTimer = Timer.periodic(drainInterval, (_) => _drainTick());
    } else if (!wanted && _drainTimer != null) {
      _drainTimer!.cancel();
      _drainTimer = null;
    }
  }

  void _drainTick() {
    // drainEvents takes up to 1024 events per call. Bounded, so a plugin
    // that floods events cannot hold the isolate.
    for (var i = 0; i < 8; i++) {
      if (_disposed || _nativeGone || drainEvents().length < 1024) {
        return;
      }
    }
  }

  // --- native editor ---

  /// Whether the plugin offers a native editor (a CLAP gui on macOS).
  bool get hasEditor =>
      !_disposed && ffigen.soloud_clapHasEditor(pluginHandle) != 0;

  /// Whether the editor is on screen.
  bool get isEditorOpen =>
      !_disposed && ffigen.soloud_clapIsEditorOpen(pluginHandle) != 0;

  /// Opens the native editor: embedded in a host window titled [title], or
  /// in the plugin's own window when it only offers that. An open editor
  /// comes to the front.
  ///
  /// Returns false without an editor, after [dispose], or when the plugin
  /// fails to show it. A [ClapHostEventType.editorClosed] event follows when
  /// the user or the plugin closes it.
  bool openEditorWindow({String? title}) {
    if (_disposed) {
      return false;
    }
    final titlePtr = title != null
        ? title.toNativeUtf8(allocator: calloc).cast<ffi.Char>()
        : ffi.nullptr.cast<ffi.Char>();
    try {
      return ffigen.soloud_clapOpenEditor(pluginHandle, titlePtr) ==
          ffigen.PlayerErrors.noError;
    } finally {
      if (titlePtr != ffi.nullptr) {
        calloc.free(titlePtr);
      }
    }
  }

  /// Closes the editor if open. No [ClapHostEventType.editorClosed] event
  /// follows.
  void closeEditorWindow() {
    if (_disposed) {
      return;
    }
    ffigen.soloud_clapCloseEditor(pluginHandle);
  }

  void _checkNotDisposed() {
    if (_disposed) {
      throw StateError('The CLAP plugin $pluginHandle was disposed.');
    }
  }

  /// Closes [events] and [paramEdits] and unloads the plugin. Safe to call
  /// more than once, and after [ClapHost.unloadAll].
  @mustCallSuper
  void dispose() {
    if (_disposed) {
      return;
    }
    _disposed = true;
    _drainTimer?.cancel();
    _drainTimer = null;
    unawaited(_events.close());
    unawaited(_paramEdits.close());
    // invalidParameter after unloadAll: nothing is left to unload.
    ffigen.soloud_clapUnloadPlugin(pluginHandle);
  }
}

/// A loaded CLAP audio effect running in a [CustomFilterSlot].
///
/// See [ClapPluginBase] for its parameters, state, events and editor.
@experimental
class ClapEffectPlugin extends ClapPluginBase {
  ClapEffectPlugin._({required int pluginHandle, required this.slot})
    : super._(pluginHandle);

  /// The [CustomFilterSlot] this effect runs in.
  final CustomFilterSlot slot;

  /// Binds parameter [slotParamIndex] (`1..7`) of [slot] to the plugin
  /// parameter [paramId], so `fadeFilterParameter` and
  /// `oscillateFilterParameter` on the returned [CustomFilterParam] drive
  /// it. The slot value (`0..1`) is converted with
  /// [ClapParamInfo.fromNormalized] and sent at the start of each processed
  /// block in which it changed.
  ///
  /// Binding does not send the slot's current value: the plugin keeps its
  /// value until the slot value changes. The first processed block after
  /// binding sends the slot value only when it differs (as a plain value)
  /// from the one read at binding time, even if blocks were skipped in
  /// between. A binding replaces an earlier one for the same index, and
  /// survives a rescan while [paramId] still exists and is writable.
  ///
  /// Throws an [ArgumentError] for an index outside `1..7` (0 is the slot's
  /// wet level), and a [SoLoudCppException] for an unknown or read-only
  /// parameter, or while the parameter list is being rebuilt.
  CustomFilterParam mapToSlotParameter({
    required int slotParamIndex,
    required int paramId,
    SoundHandle? soundHandle,
  }) {
    _checkNotDisposed();
    _checkSlotParamIndex(slotParamIndex);
    // The value the binding is made against (NaN when it cannot be read:
    // the native side then falls back to the slot's changed bit).
    var baseline = double.nan;
    try {
      final read = SoLoudController().soLoudFFI.getCustomFilterParams(
        slot.slotIndex,
        slotParamIndex,
        handle: soundHandle,
        busId: slot.busId,
      );
      if (read.error == PlayerErrors.noError) {
        baseline = read.value;
      }
    } catch (_) {}
    _check(
      ffigen.soloud_clapMapSlotParameter(
        pluginHandle,
        slotParamIndex,
        paramId,
        baseline,
      ),
    );
    return CustomFilterParam(
      soundHandle,
      slot.busId,
      slot.slotIndex,
      slotParamIndex,
      0,
      1,
    );
  }

  /// Removes the binding of slot parameter [slotParamIndex] (`1..7`), if
  /// any. The plugin keeps the last value it was sent.
  ///
  /// Throws an [ArgumentError] for an index outside `1..7`.
  void unmapSlotParameter(int slotParamIndex) {
    _checkNotDisposed();
    _checkSlotParamIndex(slotParamIndex);
    _check(ffigen.soloud_clapUnmapSlotParameter(pluginHandle, slotParamIndex));
  }

  static void _checkSlotParamIndex(int slotParamIndex) {
    if (slotParamIndex < 1 || slotParamIndex > CustomFilterSlot.maxUserParams) {
      throw ArgumentError.value(
        slotParamIndex,
        'slotParamIndex',
        'must be between 1 and ${CustomFilterSlot.maxUserParams} '
            '(index 0 is the wet level)',
      );
    }
  }

  /// Closes [events] and [paramEdits], unbinds and deactivates [slot] and
  /// unloads the plugin. Safe to call more than once, and after
  /// [ClapHost.unloadAll].
  ///
  /// The slot is left alone when another CLAP plugin was loaded into it
  /// since, or the engine was shut down since this plugin was loaded.
  @override
  void dispose() {
    if (isDisposed) {
      return;
    }
    try {
      if (_slotOwners.release(slot, this, SoLoud.instance.engineSession) &&
          SoLoud.instance.isInitialized &&
          slot.index >= 0) {
        slot
          ..setNativeCallback()
          ..deactivate();
      }
    } catch (_) {}
    super.dispose();
  }
}

/// A loaded CLAP instrument wrapped in a playable SoLoud [AudioSource].
///
/// Play [audioSource] with `SoLoud.instance.play` (one voice: playing it
/// again restarts the voice) and send notes with [noteOn] and [noteOff]. A
/// [ClapHostEventType.noteEnd] event on [events] reports a note the plugin
/// finished. See [ClapPluginBase] for its parameters, state, events and
/// editor.
@experimental
class ClapInstrumentPlugin extends ClapPluginBase {
  ClapInstrumentPlugin._({required int pluginHandle, required this.audioSource})
    : super._(pluginHandle);

  /// The playable SoLoud [AudioSource] the plugin renders. Pass it to
  /// `SoLoud.instance.play` or `play3d`. Disposed by [dispose].
  final AudioSource audioSource;

  /// How notes reach the plugin. [ClapNoteDialect.none] when it has no note
  /// port this host speaks: [noteOn] and [noteOff] then throw. It can change
  /// when the plugin restarts.
  ClapNoteDialect get noteDialect => _disposed
      ? ClapNoteDialect.none
      : ClapNoteDialect.fromValue(
          ffigen.soloud_clapGetNoteDialect(pluginHandle),
        );

  /// Starts note [pitch] (`0..127`) on [channel] (`0..15`), [sampleOffset]
  /// frames into the next rendered block (clamped to the block).
  /// [velocity] is `0..1` (clamped). Never waits for the main thread. A no-op
  /// after [dispose].
  ///
  /// Throws a [SoLoudCppException] for a value out of range, a plugin
  /// without a note dialect, or a full note queue.
  @experimental
  void noteOn({
    required int pitch,
    double velocity = 0.8,
    int channel = 0,
    int sampleOffset = 0,
  }) => _sendNote(true, pitch, velocity, channel, sampleOffset);

  /// Releases note [pitch] on [channel]. See [noteOn].
  @experimental
  void noteOff({
    required int pitch,
    double velocity = 0,
    int channel = 0,
    int sampleOffset = 0,
  }) => _sendNote(false, pitch, velocity, channel, sampleOffset);

  void _sendNote(
    bool on,
    int pitch,
    double velocity,
    int channel,
    int sampleOffset,
  ) {
    if (_disposed) {
      return;
    }
    // Checked here: the FFI call narrows to 32 bits, so 0x10000003c would
    // arrive as 60.
    if (pitch < 0 ||
        pitch > 127 ||
        channel < 0 ||
        channel > 15 ||
        sampleOffset < 0 ||
        sampleOffset > 0xffffffff) {
      throw SoLoudCppException.fromPlayerError(PlayerErrors.invalidParameter);
    }
    _check(
      ffigen.soloud_clapSendNote(
        pluginHandle,
        on ? 1 : 0,
        channel,
        pitch,
        velocity,
        sampleOffset,
      ),
    );
  }

  /// Disposes [audioSource], closes [events] and [paramEdits] and unloads
  /// the plugin. Safe to call more than once, and after
  /// [ClapHost.unloadAll].
  @override
  void dispose() {
    if (isDisposed) {
      return;
    }
    try {
      if (SoLoud.instance.isInitialized) {
        unawaited(SoLoud.instance.disposeSource(audioSource));
      }
    } catch (_) {}
    super.dispose();
  }
}
