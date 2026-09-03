# 09 — Migrate to the standalone GPhil audio-I/O host

**What to build:** Ship a focused GPhil audio-I/O package that owns the duplex device and shared frame timeline, uses SoLoud as its playback renderer, and uses the capture subsystem as its recording sink. GPhil can select the new host while the proven in-fork path remains available for parity checks.

**Blocked by:** 08 — Expand: isolate the duplex host boundary.

**Status:** ready-for-agent

- [ ] The new package owns duplex initialization, permissions and audio-session coordination, route lifecycle, generation changes, shared frame counting, and latency reporting.
- [ ] SoLoud is driven as a renderer and does not open a competing playback device during a shared-clock session.
- [ ] The capture sink does not open a competing input device during a shared-clock session.
- [ ] GPhil can switch between the in-fork and standalone hosts without changing take or project semantics.
- [ ] The standalone path passes the same automated contract, interruption, durability, and physical-device qualification gates as the in-fork path.
- [ ] `flutter_recorder` remains an independent package; reuse of its code requires a device-independent sink boundary and must not introduce a second live capture device.

