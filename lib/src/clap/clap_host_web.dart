import 'package:flutter/foundation.dart';
import 'package:flutter_soloud/src/audio_source.dart';
import 'package:flutter_soloud/src/clap/clap_types.dart';
import 'package:flutter_soloud/src/enums.dart';
import 'package:flutter_soloud/src/exceptions/exceptions.dart';
import 'package:flutter_soloud/src/filters/custom_filter.dart';
import 'package:flutter_soloud/src/sound_handle.dart';
import 'package:meta/meta.dart';

export 'package:flutter_soloud/src/clap/clap_types.dart';

/// Web stub for [ClapHost]: CLAP hosting is only supported on macOS.
@experimental
class ClapHost {
  /// Creates a [ClapHost] instance.
  @experimental
  ClapHost();

  /// Always `false` on the Web platform.
  @experimental
  bool get isSupported => false;

  /// Always `(0, 0, 0)` on the Web platform.
  @experimental
  ({int major, int minor, int revision}) get hostClapVersion =>
      (major: 0, minor: 0, revision: 0);

  /// Always empty on the Web platform.
  @experimental
  List<String> get standardSearchPaths => const [];

  /// Always empty on the Web platform.
  @experimental
  List<ClapPluginInfo> scanBundle(String bundlePath) => const [];

  /// No-op on the Web platform.
  @experimental
  void unloadAll() {}

  /// Throws [SoLoudCppException] on the Web platform.
  @experimental
  ClapEffectPlugin loadEffect({
    required String bundlePath,
    required CustomFilterSlot slot,
    String? pluginId,
    int? sampleRate,
    int maxBlockSize = 2048,
    Channels channels = Channels.stereo,
  }) {
    throw SoLoudCppException.fromPlayerError(PlayerErrors.invalidParameter);
  }

  /// Throws [SoLoudCppException] on the Web platform.
  @experimental
  ClapInstrumentPlugin loadInstrument({
    required String bundlePath,
    String? pluginId,
    int? sampleRate,
    int maxBlockSize = 2048,
    Channels channels = Channels.stereo,
  }) {
    throw SoLoudCppException.fromPlayerError(PlayerErrors.invalidParameter);
  }
}

/// Web stub for what a loaded CLAP plugin offers. It cannot be created on the
/// Web platform.
@experimental
abstract class ClapPluginBase {
  /// The native plugin handle.
  int get pluginHandle;

  /// Whether [dispose] was called.
  bool get isDisposed;

  /// The plugin's latency in frames.
  int get latencySamples;

  /// The plugin's parameters as last read.
  List<ClapParamInfo> get parameters;

  /// Reads the parameter list again.
  List<ClapParamInfo> refreshParameters();

  /// The plugin's current plain value of [paramId].
  double getParameterValue(int paramId);

  /// Formats the plain [value] of [paramId].
  String valueToText(int paramId, double value);

  /// Parses [text] into a plain value of [paramId].
  double textToValue(int paramId, String text);

  /// Queues the plain [value] for [paramId].
  void setParameterValue(int paramId, double value);

  /// The [ClapParamInfo] of [paramId] in [parameters], or null.
  ClapParamInfo? parameterInfo(int paramId);

  /// The current value of [paramId], normalized to `0..1`.
  double getParameterNormalized(int paramId);

  /// Queues the normalized [value] for [paramId].
  void setParameterNormalized(int paramId, double value);

  /// Saves the plugin's state.
  Uint8List saveState();

  /// Loads a state saved by [saveState].
  void restoreState(Uint8List state);

  /// Takes up to [maxEvents] of the plugin's pending events.
  List<ClapHostEvent> drainEvents({int maxEvents = 1024});

  /// Every event the plugin reports.
  Stream<ClapHostEvent> get events;

  /// The parameter changes the plugin makes itself.
  Stream<ClapParamEditEvent> get paramEdits;

  /// Whether the plugin offers a native editor.
  bool get hasEditor;

  /// Whether the editor is on screen.
  bool get isEditorOpen;

  /// Opens the native editor.
  bool openEditorWindow({String? title});

  /// Closes the editor if open.
  void closeEditorWindow();

  /// Unloads the plugin.
  void dispose();
}

/// Web stub for a loaded CLAP audio effect. It cannot be created on the Web
/// platform.
@experimental
abstract class ClapEffectPlugin extends ClapPluginBase {
  /// The [CustomFilterSlot] this effect runs in.
  CustomFilterSlot get slot;

  /// Binds slot parameter [slotParamIndex] (`1..7`) to [paramId].
  CustomFilterParam mapToSlotParameter({
    required int slotParamIndex,
    required int paramId,
    SoundHandle? soundHandle,
  });

  /// Removes the binding of slot parameter [slotParamIndex].
  void unmapSlotParameter(int slotParamIndex);
}

/// Web stub for a loaded CLAP instrument. It cannot be created on the Web
/// platform.
@experimental
abstract class ClapInstrumentPlugin extends ClapPluginBase {
  /// The playable SoLoud [AudioSource] the plugin renders.
  AudioSource get audioSource;

  /// How notes reach the plugin.
  ClapNoteDialect get noteDialect;

  /// Starts note [pitch].
  void noteOn({
    required int pitch,
    double velocity = 0.8,
    int channel = 0,
    int sampleOffset = 0,
  });

  /// Releases note [pitch].
  void noteOff({
    required int pitch,
    double velocity = 0,
    int channel = 0,
    int sampleOffset = 0,
  });
}
