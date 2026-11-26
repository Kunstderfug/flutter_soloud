// fork: the enums GPhil's additions use: atomic voice-group start and the
// CLAP host. Re-exported from enums.dart, so `src/enums.dart` imports and the
// package's public API see them as before. See FORK_MAINTENANCE.md.

/// Result of atomically scheduling a prepared voice group.
enum VoiceGroupStartResult {
  /// Every group member was assigned the same sample delay and unpaused.
  success(0),

  /// The audio backend has not been initialized.
  backendNotInitialized(1),

  /// A deadline or expected-member-count input is invalid.
  invalidInput(2),

  /// The handle is not a valid, nonempty voice group.
  invalidGroup(3),

  /// The raw group does not contain exactly the expected number of members.
  memberCountMismatch(4),

  /// The required main voice is invalid or absent from the group.
  invalidMain(5),

  /// At least one group member no longer resolves to a live voice.
  invalidMember(6),

  /// At least one group member was not paused when the commit was attempted.
  memberNotPaused(7),

  /// The absolute engine deadline was already reached.
  deadlineReached(8);

  const VoiceGroupStartResult(this.value);

  /// The integer returned by the native API.
  final int value;

  /// Maps a native integer result to its public Dart value.
  static VoiceGroupStartResult fromValue(int value) => switch (value) {
    0 => success,
    1 => backendNotInitialized,
    2 => invalidInput,
    3 => invalidGroup,
    4 => memberCountMismatch,
    5 => invalidMain,
    6 => invalidMember,
    7 => memberNotPaused,
    8 => deadlineReached,
    _ => throw ArgumentError('Unknown value for VoiceGroupStartResult: $value'),
  };
}

/// Event types the CLAP host reports for a loaded plugin.
///
/// WARNING: Keep these in sync with `ClapHostEventType` in `src/enums.h`.
enum ClapHostEventType {
  /// The plugin changed a parameter's plain value.
  paramValue(1),

  /// The user started a gesture on a parameter.
  paramGestureBegin(2),

  /// The user ended a gesture on a parameter.
  paramGestureEnd(3),

  /// Events were lost: re-read every parameter value.
  resync(4),

  /// The plugin rescanned its parameters (the CLAP rescan flags).
  paramsRescan(5),

  /// The plugin's latency changed.
  latencyChanged(6),

  /// The plugin restarted and is processing again.
  restarted(7),

  /// A requested restart failed: the plugin no longer processes.
  restartFailed(8),

  /// The plugin's state changed and is worth saving.
  stateDirty(9),

  /// The user or the plugin closed the editor. Not reported for
  /// `ClapPluginBase.closeEditorWindow`, a hidden editor or an unload.
  editorClosed(10),

  /// An instrument finished a note: see `ClapHostEvent.noteKey` and the
  /// other `note*` getters.
  noteEnd(11);

  const ClapHostEventType(this.value);

  /// The integer value representing the enum in native code.
  final int value;

  /// Returns the [ClapHostEventType] for [value], or null for a type this
  /// version does not know.
  static ClapHostEventType? fromValue(int value) {
    for (final type in values) {
      if (type.value == value) {
        return type;
      }
    }
    return null;
  }
}

/// How a CLAP instrument receives notes.
///
/// WARNING: Keep these in sync with `ClapNoteDialect` in `src/enums.h`.
enum ClapNoteDialect {
  /// No note port the host speaks: notes cannot be sent.
  none(0),

  /// CLAP note events.
  clap(1),

  /// MIDI 1.0 messages.
  midi(2);

  const ClapNoteDialect(this.value);

  /// The integer value representing the enum in native code.
  final int value;

  /// Returns the [ClapNoteDialect] for [value], or [none] for a value this
  /// version does not know.
  static ClapNoteDialect fromValue(int value) {
    for (final dialect in values) {
      if (dialect.value == value) {
        return dialect;
      }
    }
    return none;
  }
}
