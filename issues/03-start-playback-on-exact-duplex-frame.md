# 03 — Start playback on an exact duplex output frame

**What to build:** Allow GPhil to arm a SoLoud voice for an exact future frame on the shared duplex timeline and learn the first output frame that actually contained that voice. Replace caller-side post-unpause timestamps as the synchronization authority.

**Blocked by:** 01 — Establish a real duplex frame timeline.

**Status:** ready-for-agent

Implementation coordination: this ticket and Ticket 02 have independent semantic blockers but overlap in the duplex callback and public result contracts, so implement them serially.

- [ ] A caller can request a generation-scoped future output frame for playback start.
- [ ] Playback begins on the requested frame or returns an explicit superseded, late, route-changed, or failed result.
- [ ] The result reports the first frame actually rendered and never substitutes a timestamp sampled after an API call.
- [ ] Seeking, looping, pause, stop, and bus routing retain their documented behavior around the scheduled start.
- [ ] Automated tests cover exact start, late requests, cancellation, device restart, and stale-generation rejection.

