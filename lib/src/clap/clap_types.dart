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

  /// Whether plain values snap to whole numbers: stepped, enum and bypass
  /// parameters.
  bool get _snaps => isStepped || isEnum || isBypass;

  /// Converts the plain [plain] value to `0..1`, linearly over
  /// [minValue]..[maxValue]. The value is clamped to the range first, and
  /// rounded when [isStepped], [isEnum] or [isBypass]. A parameter with an
  /// empty range maps everything to 0.
  ///
  /// Throws an [ArgumentError] for a value that is not finite.
  double toNormalized(double plain) {
    if (!plain.isFinite) {
      throw ArgumentError.value(plain, 'plain', 'must be finite');
    }
    if (!(maxValue > minValue)) {
      return 0;
    }
    var v = plain.clamp(minValue, maxValue);
    if (_snaps) {
      v = v.roundToDouble().clamp(minValue, maxValue);
    }
    return ((v - minValue) / (maxValue - minValue)).clamp(0.0, 1.0);
  }

  /// Converts the normalized [normalized] value (clamped to `0..1`) to a
  /// plain value, linearly over [minValue]..[maxValue], rounded when
  /// [isStepped], [isEnum] or [isBypass]. A parameter with an empty range
  /// maps everything to [minValue].
  ///
  /// This is the mapping the host uses for a slot parameter bound with
  /// `ClapEffectPlugin.mapToSlotParameter`.
  ///
  /// Throws an [ArgumentError] for a value that is not finite.
  double fromNormalized(double normalized) {
    if (!normalized.isFinite) {
      throw ArgumentError.value(normalized, 'normalized', 'must be finite');
    }
    if (!(maxValue > minValue)) {
      return minValue;
    }
    final n = normalized.clamp(0.0, 1.0);
    var v = minValue + n * (maxValue - minValue);
    if (_snaps) {
      v = v.roundToDouble();
    }
    return v.clamp(minValue, maxValue);
  }

  @override
  String toString() =>
      'ClapParamInfo(paramId: $paramId, name: $name, module: $module, '
      'range: $minValue..$maxValue, default: $defaultValue, flags: $flags)';
}

/// An event a loaded CLAP plugin reported, taken by
/// `ClapPluginBase.drainEvents`.
///
/// Events arrive in order. A [ClapHostEventType.resync] means events were
/// lost: re-read every parameter value. With [isParamsReset] the parameter
/// list itself changed, and `ClapPluginBase.parameters` has been refreshed.
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

  bool get _isNoteEnd => type == ClapHostEventType.noteEnd;

  /// The note id of a [ClapHostEventType.noteEnd] event, or -1 (none or
  /// any; notes sent by the host have none).
  int get noteId => _isNoteEnd ? paramId.toSigned(32) : -1;

  /// The key (`0..127`) of a [ClapHostEventType.noteEnd] event, or -1 (any).
  int get noteKey => _isNoteEnd && value.isFinite ? value.toInt() : -1;

  /// The channel (`0..15`) of a [ClapHostEventType.noteEnd] event, or -1
  /// (any).
  int get noteChannel => _isNoteEnd ? (flags & 0xffff).toSigned(16) : -1;

  /// The note port of a [ClapHostEventType.noteEnd] event, or -1 (any).
  int get notePortIndex =>
      _isNoteEnd ? ((flags >> 16) & 0xffff).toSigned(16) : -1;

  @override
  String toString() =>
      'ClapHostEvent(type: ${type.name}, paramId: $paramId, value: $value, '
      'flags: $flags)';
}

/// What a [ClapParamEditEvent] reports.
@experimental
enum ClapParamEditKind {
  /// The plugin changed the parameter's value.
  value,

  /// The user started dragging the parameter's control.
  gestureBegin,

  /// The user released the parameter's control.
  gestureEnd,
}

/// A parameter change a CLAP plugin made itself, usually because the user
/// moved a control in its editor. Delivered by
/// `ClapPluginBase.paramEdits`.
@experimental
@immutable
class ClapParamEditEvent {
  /// Creates a [ClapParamEditEvent].
  const ClapParamEditEvent({
    required this.pluginHandle,
    required this.paramId,
    required this.kind,
    this.value,
    this.normalizedValue,
    this.isDontRecord = false,
  });

  /// The native handle of the plugin that made the change.
  final int pluginHandle;

  /// The parameter's CLAP id.
  final int paramId;

  /// A value change, or the start or end of a gesture.
  final ClapParamEditKind kind;

  /// The new plain value of a [ClapParamEditKind.value] event, or null for
  /// a gesture.
  final double? value;

  /// [value] normalized to `0..1` with [ClapParamInfo.toNormalized], or
  /// null for a gesture or a parameter missing from
  /// `ClapPluginBase.parameters` when the event was delivered.
  final double? normalizedValue;

  /// The plugin asked not to record this change as automation
  /// (`CLAP_EVENT_DONT_RECORD`).
  final bool isDontRecord;

  @override
  String toString() =>
      'ClapParamEditEvent(handle: $pluginHandle, param: $paramId, '
      'kind: ${kind.name}, value: $value, normalized: $normalizedValue, '
      'dontRecord: $isDontRecord)';
}
