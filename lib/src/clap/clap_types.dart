import 'package:flutter_soloud/src/enums.dart';
import 'package:meta/meta.dart';

/// A plugin found inside a `.clap` bundle by `ClapHost.scanBundle`.
@experimental
@immutable
class ClapPluginInfo {
  /// Creates a [ClapPluginInfo] descriptor.
  const ClapPluginInfo({
    required this.pluginId,
    required this.name,
    required this.vendor,
    required this.version,
    required this.features,
  });

  /// The plugin's reverse-DNS id (for example `"com.example.eq"`). Pass it
  /// to `ClapHost.loadEffect` to pick this plugin in a multi-plugin bundle.
  final String pluginId;

  /// The plugin's display name.
  final String name;

  /// The plugin's vendor, or empty.
  final String vendor;

  /// The plugin's version string, or empty.
  final String version;

  /// The plugin's CLAP features (for example `audio-effect`, `equalizer`).
  final List<String> features;

  /// Whether the plugin declares the `audio-effect` feature.
  bool get isAudioEffect => features.contains('audio-effect');

  /// Whether the plugin declares the `instrument` feature.
  bool get isInstrument => features.contains('instrument');

  @override
  String toString() =>
      'ClapPluginInfo(name: $name, pluginId: $pluginId, vendor: $vendor, '
      'version: $version, features: $features)';
}

/// A parameter of a loaded CLAP plugin. Values are plain (in the plugin's
/// own units, `minValue..maxValue`), not normalized.
@experimental
@immutable
class ClapParamInfo {
  /// Creates a [ClapParamInfo] descriptor.
  const ClapParamInfo({
    required this.paramId,
    required this.name,
    required this.module,
    required this.minValue,
    required this.maxValue,
    required this.defaultValue,
    required this.flags,
  });

  /// The parameter's stable CLAP id (not an index).
  final int paramId;

  /// The parameter's display name.
  final String name;

  /// The parameter's module path (for example `"Band 1/"`), or empty.
  final String module;

  /// The smallest plain value.
  final double minValue;

  /// The largest plain value.
  final double maxValue;

  /// The plain default value.
  final double defaultValue;

  /// The raw `CLAP_PARAM_*` flags.
  final int flags;

  /// The values are integers (`CLAP_PARAM_IS_STEPPED`).
  bool get isStepped => (flags & (1 << 0)) != 0;

  /// The range wraps around (`CLAP_PARAM_IS_PERIODIC`).
  bool get isPeriodic => (flags & (1 << 1)) != 0;

  /// The parameter should not be shown to the user (`CLAP_PARAM_IS_HIDDEN`).
  bool get isHidden => (flags & (1 << 2)) != 0;

  /// The host cannot change the value (`CLAP_PARAM_IS_READONLY`).
  bool get isReadOnly => (flags & (1 << 3)) != 0;

  /// The parameter bypasses the plugin (`CLAP_PARAM_IS_BYPASS`).
  bool get isBypass => (flags & (1 << 4)) != 0;

  /// The host may automate the parameter (`CLAP_PARAM_IS_AUTOMATABLE`).
  bool get isAutomatable => (flags & (1 << 5)) != 0;

  /// The values name the items of a list (`CLAP_PARAM_IS_ENUM`).
  bool get isEnum => (flags & (1 << 16)) != 0;

  @override
  String toString() =>
      'ClapParamInfo(paramId: $paramId, name: $name, module: $module, '
      'range: $minValue..$maxValue, default: $defaultValue, flags: $flags)';
}

/// An event a loaded CLAP plugin reported, taken by
/// `ClapEffectPlugin.drainEvents`.
///
/// Events arrive in order. A [ClapHostEventType.resync] means events were
/// lost: re-read every parameter value. With [isParamsReset] the parameter
/// list itself changed, and `ClapEffectPlugin.parameters` has been refreshed.
@experimental
@immutable
class ClapHostEvent {
  /// Creates a [ClapHostEvent].
  const ClapHostEvent({
    required this.type,
    required this.paramId,
    required this.value,
    required this.flags,
  });

  /// What happened.
  final ClapHostEventType type;

  /// The parameter of a [ClapHostEventType.paramValue],
  /// [ClapHostEventType.paramGestureBegin] or
  /// [ClapHostEventType.paramGestureEnd] event, or 0.
  final int paramId;

  /// The plain value of a [ClapHostEventType.paramValue] event, or the
  /// number of lost events of a [ClapHostEventType.resync] event, or 0.
  final double value;

  /// The raw flags: see [isDontRecord], [isOverflow], [isParamsReset] and
  /// [rescanFlags].
  final int flags;

  /// A [ClapHostEventType.paramValue] the plugin asked not to record as
  /// automation (`CLAP_EVENT_DONT_RECORD`).
  bool get isDontRecord =>
      type == ClapHostEventType.paramValue && (flags & 1) != 0;

  /// A [ClapHostEventType.resync] because the event ring overflowed.
  bool get isOverflow => type == ClapHostEventType.resync && (flags & 1) != 0;

  /// A [ClapHostEventType.resync] because the parameter list was reset.
  bool get isParamsReset =>
      type == ClapHostEventType.resync && (flags & 2) != 0;

  /// The `CLAP_PARAM_RESCAN_*` flags of a [ClapHostEventType.paramsRescan]
  /// event, or 0.
  int get rescanFlags =>
      type == ClapHostEventType.paramsRescan ? flags : 0;

  @override
  String toString() =>
      'ClapHostEvent(type: ${type.name}, paramId: $paramId, value: $value, '
      'flags: $flags)';
}
