# 10 — Contract the SoLoud fork to playback responsibility

**What to build:** After the standalone GPhil audio-I/O host has proven parity, remove GPhil microphone-device and take-lifecycle ownership from the SoLoud fork. Retain only the rendering, scheduling, and clock primitives the external duplex host needs.

**Blocked by:** 09 — Migrate to the standalone GPhil audio-I/O host.

**Status:** ready-for-agent

- [ ] GPhil uses the standalone audio-I/O host by default on every qualified target.
- [ ] No GPhil shared-clock session opens independent capture and playback devices.
- [ ] Microphone enumeration, capture lifecycle, recording codecs, writer policy, and take diagnostics are removed from the SoLoud public surface unless they are generic upstream features.
- [ ] SoLoud retains a narrow external-render contract with generation-aware scheduled playback and actual rendered-frame reporting.
- [ ] Legacy compatibility is removed only after repository-wide consumers and stored-project behavior are verified.
- [ ] The contracted fork passes playback, scheduling, shared-host integration, and GPhil recording regression suites.
