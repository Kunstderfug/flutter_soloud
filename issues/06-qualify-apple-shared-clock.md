# 06 — Qualify the shared clock on Apple targets

**What to build:** Demonstrate on physical iOS and macOS targets that the duplex implementation provides the synchronization, latency reporting, durability, and recovery required for real GPhil recording sessions.

**Blocked by:** 05 — Preserve timeline truth through route changes and interruptions.

**Status:** ready-for-agent

- [ ] Physical-device runs measure first-frame alignment and long-take input/output drift rather than inferring them from API call timestamps.
- [ ] Qualification covers built-in routes, wired routes, supported Bluetooth behavior, route replacement, interruptions, and app lifecycle transitions.
- [ ] Recorded effective format, callback period, input latency, and output latency match observed runtime behavior.
- [ ] Long and writer-pressure sessions produce correct frame and loss diagnostics without callback stalls.
- [ ] The qualification record names the devices, OS versions, build mode, exact revision, commands, measurements, tolerances, and any unsupported route combinations.

