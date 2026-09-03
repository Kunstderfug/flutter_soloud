# 02 — Write durable recordings from duplex input

**What to build:** Record the microphone input received by the shared duplex callback without performing file or codec work on the real-time thread. A completed take must carry enough evidence for GPhil to distinguish a valid recording from a truncated or damaged one.

**Blocked by:** 01 — Establish a real duplex frame timeline.

**Status:** ready-for-agent

- [ ] Duplex input is copied into a preallocated bounded transport and written off the audio callback.
- [ ] Overflow behavior is deterministic, counted, and preserves timeline duration through explicit silence accounting where required.
- [ ] Stopping awaits writer flush and container finalization before returning success.
- [ ] The stop result reports captured, written, overflow, silence, and optional mirror frame counts together with writer and finalization status.
- [ ] Cancel removes only the active take's incomplete artifacts and leaves unrelated recordings untouched.
- [ ] Tests cover normal writing, slow-writer pressure, overflow, writer failure, finalization failure, stop, and cancel.

