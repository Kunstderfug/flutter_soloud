# 07 — Qualify the shared clock on Android and desktop targets

**What to build:** Demonstrate that supported Android and desktop backends preserve the same duplex-frame contract, or explicitly report a platform/route as unsupported rather than falling back to falsely synchronized independent devices.

**Blocked by:** 05 — Preserve timeline truth through route changes and interruptions.

**Status:** ready-for-agent

- [ ] Physical or hardware-backed runs measure first-frame alignment and long-take drift on every supported target class.
- [ ] Android qualification covers built-in and USB input selection, negotiated format, callback period, route replacement, and lifecycle transitions.
- [ ] Desktop qualification covers available native backends, selected-device replacement, sleep/wake, and teardown/reinitialization.
- [ ] Unsupported duplex combinations fail explicitly without producing a take labelled as shared-clock synchronized.
- [ ] Long and writer-pressure sessions produce correct frame and loss diagnostics without callback stalls.
- [ ] The qualification record names devices, operating systems, audio backends, build mode, exact revision, measurements, tolerances, and exclusions.

