# 08 — Expand: isolate the duplex host boundary

**What to build:** Introduce a device-host boundary around the qualified duplex lifecycle while leaving existing behavior and public callers intact. SoLoud rendering and recording become explicit consumers of frame blocks owned by the host, making later package extraction mechanical rather than another audio redesign.

**Blocked by:** 06 — Qualify the shared clock on Apple targets; 07 — Qualify the shared clock on Android and desktop targets.

**Status:** ready-for-agent

- [ ] Duplex device creation, lifecycle, route generation, callback-frame counting, and latency reporting have one focused owner.
- [ ] SoLoud consumes output frame requests through a narrow rendering interface without owning recording policy.
- [ ] Capture consumes timestamped input frame blocks through a narrow sink interface without owning playback policy.
- [ ] The existing GPhil-facing API continues to pass the full shared-clock and durability qualification suite.
- [ ] Playback-only clients remain supported without importing GPhil recording concepts.
- [ ] No source file added or expanded by the refactor exceeds the repository's 1000-line maintainability limit.

