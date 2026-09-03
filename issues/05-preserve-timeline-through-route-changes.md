# 05 — Preserve timeline truth through route changes and interruptions

**What to build:** Keep GPhil's recording timeline truthful when the duplex device is interrupted, rerouted, stopped, or rebuilt. Recovery must never silently join audio from different device generations or conceal lost frames.

**Blocked by:** 04 — Adopt the shared-clock session in GPhil.

**Status:** ready-for-agent

- [ ] Every device rebuild creates a new clock generation with timestamped old and new route metadata.
- [ ] The session reports whether capture and playback stopped, continued, or restarted and identifies every lost or inserted frame range.
- [ ] GPhil applies an explicit continue, recover, or terminate policy for each interruption and route transition.
- [ ] A recovered take retains a deterministic mapping to composition time across all generation boundaries.
- [ ] Tests cover interruption begin/end, default-device replacement, selected-device loss, format change, restart failure, and stop during recovery.

