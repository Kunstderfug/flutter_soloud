# 04 — Adopt the shared-clock session in GPhil

**What to build:** Make simultaneous record-and-play in GPhil use the duplex session as its timing authority. A saved take must be placed on the composition timeline from shared frame positions and effective route latency, while the existing separate-device path remains available for controlled comparison during migration.

**Blocked by:** 02 — Write durable recordings from duplex input; 03 — Start playback on an exact duplex output frame.

**Status:** ready-for-agent

- [ ] GPhil arms recording and playback as one generation-scoped shared-clock operation.
- [ ] Take placement derives from actual input and output frame positions rather than Dart call order or wall-clock estimates.
- [ ] Effective input and output latency are recorded separately from the shared frame origin.
- [ ] Stop and save reject or visibly flag writer failure, discontinuity, stale generation, and incomplete finalization.
- [ ] A migration switch can select the legacy path for comparison without changing project data semantics.
- [ ] End-to-end tests prove deterministic take placement for delayed start, seeked start, stop, cancel, and playback-start failure.

