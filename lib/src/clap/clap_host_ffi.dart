import 'dart:ffi' as ffi;
import 'dart:io';

import 'package:ffi/ffi.dart';
import 'package:flutter/foundation.dart';
import 'package:flutter_soloud/src/bindings/flutter_soloud_ffigen.dart'
    as ffigen;
import 'package:flutter_soloud/src/clap/clap_slot_owners.dart';
import 'package:flutter_soloud/src/clap/clap_text.dart';
import 'package:flutter_soloud/src/clap/clap_types.dart';
import 'package:flutter_soloud/src/enums.dart';
import 'package:flutter_soloud/src/exceptions/exceptions.dart';
import 'package:flutter_soloud/src/filters/custom_filter.dart';
import 'package:flutter_soloud/src/soloud.dart';
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

/// Scans and loads native CLAP audio effects (macOS only for now).
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

/// A loaded CLAP audio effect running in a [CustomFilterSlot].
///
/// Parameter values are plain: in the plugin's own units, within
/// [ClapParamInfo.minValue]..[ClapParamInfo.maxValue].
@experimental
class ClapEffectPlugin {
  ClapEffectPlugin._({required this.pluginHandle, required this.slot})
    : _parameters = _queryParameters(pluginHandle);

  /// The native plugin handle.
  final int pluginHandle;

  /// The [CustomFilterSlot] this effect runs in.
  final CustomFilterSlot slot;

  List<ClapParamInfo> _parameters;
  bool _disposed = false;

  /// Whether [dispose] was called.
  bool get isDisposed => _disposed;

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

  /// Saves the plugin's state.
  ///
  /// Throws a [SoLoudCppException] when the plugin cannot save its state,
  /// for example when it has no CLAP `state` extension.
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
  /// Throws a [SoLoudCppException] when the plugin rejects the state, or has
  /// no CLAP `state` extension.
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
  List<ClapHostEvent> drainEvents({int maxEvents = 1024}) {
    if (_disposed || maxEvents <= 0) {
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
        // is nothing left to take.
        if (error != ffigen.PlayerErrors.noError) {
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
    return events;
  }

  void _checkNotDisposed() {
    if (_disposed) {
      throw StateError('The CLAP plugin $pluginHandle was disposed.');
    }
  }

  /// Unbinds and deactivates [slot] and unloads the plugin. Safe to call
  /// more than once, and after [ClapHost.unloadAll].
  ///
  /// The slot is left alone when another CLAP plugin was loaded into it
  /// since, or the engine was shut down since this plugin was loaded.
  void dispose() {
    if (_disposed) {
      return;
    }
    _disposed = true;
    try {
      if (_slotOwners.release(slot, this, SoLoud.instance.engineSession) &&
          SoLoud.instance.isInitialized &&
          slot.index >= 0) {
        slot
          ..setNativeCallback()
          ..deactivate();
      }
    } catch (_) {}
    // invalidParameter after unloadAll: nothing is left to unload.
    ffigen.soloud_clapUnloadPlugin(pluginHandle);
  }
}
