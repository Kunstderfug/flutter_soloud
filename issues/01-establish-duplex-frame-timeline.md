# 01 — Establish a real duplex frame timeline

**What to build:** Give simultaneous microphone capture and playback one native audio timeline. When shared-clock mode is active, one duplex device callback must receive input and produce SoLoud output while advancing a single generation-scoped frame counter. Preserve the existing playback-only mode for clients that do not record.

**Blocked by:** None — can start immediately.

**Status:** ready-for-agent

- [ ] Shared-clock mode uses one duplex device rather than independent capture and playback devices.
- [ ] Every callback block has one generation ID, first-frame position, and frame count that apply to both its input and output.
- [ ] The public diagnostic contract reports the effective sample rate, channel configuration, callback period, generation, and shared frame position.
- [ ] Automated native and Dart-facing tests prove capture and output observations advance in the same frame domain.
- [ ] Playback-only initialization and existing playback behavior remain supported.

