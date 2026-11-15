# CLAP hosting + native plugin editor UI (CLAP and VST3): implementation plan

Date: 2026-11-15. Branch `clap-host`, based on `feature_capture-v5.0` (11a91016).
Background research: [`clap_hosting_research.md`](clap_hosting_research.md) (file:line refs are
against `feature_capture-v5.0`).

## Goal
1. Host CLAP effect plugins (and later instruments) natively in flutter_soloud, beside the
   existing VST3 host, on macOS first.
2. **Hard requirement:** native plugin editor UI for **both CLAP and VST3**, opened from GPhil's
   Plugins tab and verified live on real plugins.
3. In GPhil, a single Plugins chain can hold VST3 and CLAP entries side by side.

Out of scope for v1:
- Windows and Linux. Keep the seams portable, with stubs that compile, but only macOS is live.
- CLAP instruments in GPhil. Do the native/Dart API, but no app UI.
- Preset discovery, note expressions, and the remote/out-of-process editor for CLAP.

## Ground rules
- Every `[main-thread]` CLAP call hops to the real OS main thread (dispatch_sync if off-main),
  just like `src/vst3/vst3_editor_macos.mm`. This covers load, activate, params, state and gui.
- The audio thread never allocates, never locks a contended mutex, and never calls dispatch.

## Test plugins already installed (CLAP + VST3 copies)
- `~/Library/Audio/Plug-Ins/CLAP/{ZeroEQ,ZeroComp,ZeroLimit,Airwindows Consolidated}.clap`
- `~/Library/Audio/Plug-Ins/VST3/{ZeroEQ,ZeroComp,ZeroLimit,Airwindows Consolidated}.vst3`
- `/Library/Audio/Plug-Ins/VST3/TDR Nova.vst3`. VST3 load is verified, with 184 samples of
  latency. Its editor had never been opened in GPhil before Phase 0.

## Phases
Each phase ends with a commit and a push. UI risk comes first.

### Phase 0: verify the VST3 editor UI as it stands
- In GPhil (debug macOS, sandboxed), load TDR Nova and ZeroEQ.vst3 in the Plugins tab and press
  "Open editor". Check: the window opens and resizes; it closes from its own close button; it
  reopens; removing the plugin while the editor is open is safe; app window close with the
  editor open is safe.
- Record failures with evidence. Fix fork-side bugs on `clap-host` and app-side bugs on GPhil
  `v2_daw`.
- Extend the GPhil live integration test, or add a focused one, to open and close the editor
  programmatically and assert no crash. Use screenshots for the window itself.

### Phase 1: scaffold
- Vendor `free-audio/clap` tag `1.2.10` `include/` into `src/clap/third_party/clap/`, with LICENSE
  and a pinned-version note.
- Optionally add `free-audio/clap-helpers` (MIT) at a pinned commit. Decided in plan review.
- `hook/build.dart`: an `enable_clap` user_define (a bool or a platform list), parsed like
  `enable_vst3` (`hook/build.dart:121-144`), with env fallback `SOLOUD_ENABLE_CLAP`. It adds
  `-DSOLOUD_ENABLE_CLAP`, `src/clap/**` sources, and `-isystem` for the vendored headers.
- FFI surface `soloud_clap*` in `bindings.cpp`, with `#else` stubs when disabled (mirroring the
  VST3 stubs near `bindings.cpp:5455-5976`). Update ffigen and the generated bindings.

### Phase 2: native core (`src/clap/`)
- Loader: CFBundle on the `.clap` bundle, then the `clap_entry` symbol; `init(path)` and a
  refcounted module cache; `get_factory(CLAP_PLUGIN_FACTORY_ID)`; descriptors give id, name,
  vendor, version and features.
- Scan with search paths `/Library/Audio/Plug-Ins/CLAP`, `~/Library/Audio/Plug-Ins/CLAP`
  (sandbox real-home aware) and `CLAP_PATH`.
- State machine: create → init → activate(sr, min, max) [main] → start_processing (lazy, audio
  thread) → process → stop_processing [audio] → deactivate [main] → destroy.
- Handle table with generation bits and an audio-thread barrier on unload, copying the
  `Vst3HostManager` patterns (`src/vst3/vst3_host.h:199-284`). `ClapHostManager::unloadAll()`
  goes in the same dispose block as VST3 (`bindings.cpp` ~2167).
- `process()`: planar scratch (factor a shared helper from VST3 where cheap); `audio-ports`
  negotiation (stereo main, plus mono plugins); preallocated merged in/out event lists, with
  `LockFreeSpscQueue` for param edits in and GUI edits out.
- Engine binding through the existing seams: `clapFilterDspCallback` for
  `CustomFilterSlot.setNativeCallback`, `clapSourceDspCallback` for `createCustomAudioSource`.
  Engine code stays unchanged.

### Phase 3: host extensions
- Host side: `thread-check`, `log`, `params` (rescan, clear, request_flush), `state`
  (mark_dirty), `latency` (changed), `gui` (request_resize, request_show/hide, closed),
  `timer-support`.
- `request_callback`: the audio thread only sets an atomic flag. A main-thread
  `dispatch_source` timer at about 16 ms, shared with `timer-support`, calls `on_main_thread`.
  Calls from other threads may `dispatch_async` to main.
- `request_restart`: deactivate and reactivate on main behind the barrier, then emit the same
  restart event the Dart VST3 path uses.
- Plugin side: `params` count/info/get_value/value_to_text/text_to_value (plain values with
  min/max/default); `flush` when not processing; `state` save/load into Dart bytes;
  `latency.get`.

### Phase 4: native editor UI. Hard requirement
- Refactor `Vst3EditorWindow` (`src/vst3/vst3_editor_window.h`, `vst3_editor_macos.mm`) into a
  format-agnostic `PluginHostWindow`: it owns the NSWindow, title, resize, the close callback
  and main-queue dispatch, and exposes a parent NSView. VST3's `IPlugFrame` becomes a thin
  adapter. VST3 must not regress, so rerun the Phase 0 checks.
- CLAP gui: embedded flow `is_api_supported(cocoa, false)` → create → set_scale → get_size →
  `set_parent(NSView)` → show; floating fallback `set_transient` plus show; honour
  `can_resize`, `adjust_size` and `request_resize`; handle `closed(was_destroyed)` and destroy
  the gui before deactivate or unload.
- Dart: `openEditorWindow({title})`, `closeEditorWindow()` and `isEditorOpen`, plus an
  editor-closed event, for both formats. GUI param edits become out-events, which drive a Dart
  `paramEdits` stream with the same shape as `Vst3ParamEditEvent`.

### Phase 5: Dart API
- `ClapHost`: `isSupported`, `standardSearchPaths`, `scanBundle` returning
  `List<ClapPluginInfo>`, `loadEffect({bundlePath, slot, pluginId, sampleRate, maxBlockSize,
  channels})`, `loadInstrument`, `unloadAll`.
- `ClapEffectPlugin` and `ClapInstrumentPlugin`, mirroring the `Vst3*` members: params (plain
  plus normalized helpers), state, latency, restart events, editor, and `mapToSlotParameter`.
- A web stub (`isSupported == false`), exports and the README section.
- Defer a format-agnostic `AudioPluginHost` unless the GPhil integration clearly needs it. If it
  does, keep it small.

### Phase 6: tests
- Dart unit tests against the stubs.
- A native smoke test or example page with ZeroEQ.clap on master: scan, load, process,
  round-trip state, read latency, and open and close the editor.
- An integration probe like GPhil's `integration_test/vst3_master_chain_probe_test.dart`.

### Phase 7: GPhil integration (after the fork is pushed; `/Volumes/DEV/gphil-flutter` `v2_daw`)
- `macos/Runner/DebugProfile.entitlements`: add the CLAP read-only exceptions (absolute
  `/Library/Audio/Plug-Ins/CLAP/` and home-relative `/Library/Audio/Plug-Ins/CLAP/`; the
  leading-slash format is verified working for VST3).
- pubspec user_define `enable_clap: macos`.
- `lib/services/vst3/`: entries carry a format (`vst3` | `clap`); scan both folders; badge the
  format in `_AddPluginDialog`; keep the pending-load crash marker (`FileVst3PendingLoadStore`),
  the latency total (bypassed plugins excluded), the close guard (`requiresCloseGuard`) and
  engine shutdown/resume working for both formats.
- "Open editor" works for both formats. Add tests that mirror the existing controller and tab
  tests.
- Live tests with ZeroEQ.clap and Airwindows.clap, plus the VST3 editor regression.
- Check rendered contrast of at least 4.5:1 on the dark background.
- Update `APP_ARCHITECTURE.md`. No help article while the tab is debug/profile-only.

## Done means
- In the sandboxed debug GPhil app, a native editor window has been seen to open, resize, close
  and reopen for one CLAP and one VST3 plugin (screenshots).
- Analyze and tests are green, run serially.
- Both pair members accepted the work.
- The fork and app are pushed.

Estimate: about 4–6 weeks for macOS with GUI.

## Plan review log

### 2026-11-15, round 1 (Sol 6.1 reviewed Phases 1–4 against the CLAP 1.2.10 headers): ACCEPT WITH AMENDMENTS
Opus accepted every amendment. They override the phase text above where they conflict.

**Order:** Phase 1 → **Phase 4a** (the shared `PluginHostWindow` extraction with the VST3 adapter, plus a VST3 regression re-run) → Phase 2 → Phase 3 → **a minimal Dart wrapper** (enough for the live probe) → Phase 4b (CLAP gui) → Phase 5 (the rest of the Dart API) → 6 → 7.

1. **No clap-helpers.** Only `free-audio/clap` 1.2.10 `include/` is vendored. Our own event list has aligned storage, stable pointers for the duration of a call, sample-sorted input, bounded capacity, and `try_push` returning false on overflow. SysEx and other pointer-bearing events are rejected in v1. These properties are tested directly. Thread asserts track the per-instance *symbolic* audio-thread role, not one fixed OS thread.
2. **Phase 1:**
   - `isSupported` means "compiled in on macOS". The parser never enables CLAP hosting on Windows or Linux in v1.
   - The ABI is declared unconditionally in `bindings.h`. Every out pointer, count and handle is initialised on every error path and every disabled path.
   - Scan results come with a matching free function.
   - Builds are checked with both flags off, VST3 only, CLAP only, and both on.
   - The source collector is non-recursive (`hook/build.dart:485`). `.mm` files go through the ObjC++ builder (`:361`). AppKit/QuartzCore linking must no longer depend only on VST3 (`:420`).
3. **Phase 4a stays narrow.** The shared window owns the NSWindow, content view, sizing and close notification. VST3 attachment, `IPlugFrame`, the remote helper and shm telemetry stay in the VST3 adapter. The remote-editor harness (`test/run_vst3_host_test.sh`) and the Phase 0 real-plugin checks are re-run.
4. **Restart and unload never depend on a future DSP callback.** The sequence:
   1. Clear processing eligibility. On unload, invalidate the handle.
   2. Run the SoLoud barrier.
   3. With future processing excluded, take the instance's exclusive audio role on the current thread and call `stop_processing` if processing had started.
   4. Release the role.
   5. Deactivate and reactivate, or destroy, on main.
   6. Republish eligibility only after activation succeeds.

   Thread-safe host requests only defer work: no synchronous restart inside a plugin callback, and no manager or engine lock held across a main hop or a reentrant plugin call. The shared main-queue pump tracks each timer's id and period, handles unregister and reentrancy, and drops pending work on destroy.
5. **Params:** while active, `flush` runs in the audio role. Only while inactive does it run on main. `CLAP_PARAM_REQUIRES_PROCESS` is honoured. Metadata is cached off the DSP path. Readonly and stepped flags are honoured and conversions are clamped. Global changes use wildcard note addressing. Rescans invalidate mappings and cookies. Normalisation is a linear host convention.
6. **Out-events** are drained from both `process` and `flush`. They keep the kind (value vs gesture begin/end) and `DONT_RECORD`, and they carry plain values. A new Dart event type is needed: `Vst3ParamEditEvent` can't express gestures. Producers are serialised. Overflow recovery guarantees no stuck "editing" gesture. The editor-closed event fires once. Stale queued events are dropped after unload. Plugin-origin changes are never echoed back to the plugin.
7. **CLAP gui:**
   - Never call `set_scale` for cocoa: sizes are logical points.
   - When the host accepts a plugin-origin `request_resize`, it doesn't call `set_size` (no recursion). Off-main requests are fulfilled asynchronously, or reverted with `set_size`.
   - Resize hints are honoured. Every partial failure is cleaned up.
   - Floating mode gets its own api/create flow plus `suggest_title`.
   - `closed(true)` still requires `gui.destroy()`.
   - Thread-safe gui callbacks are marshalled without blocking audio.
   - If a restart closes an open editor, it reopens after reactivation.
8. **Verification additions:**
   - A mock-plugin native harness covering:
     - restart/unload with DSP running, paused or bypassed, with no first process call, and with the device stopped
     - failed init/activate/start, asserting lifecycle order and thread roles
     - active and inactive flush, and GUI output through both paths
     - rescan, queue overflow, two instances on one module, stale handles, timers pending at destroy
     - mono/stereo/extra ports, a process error, and block chunking
     - GUI close and reopen, partial failure, resize recursion, and the floating fallback
   - The fake cocoa GUI only proves the protocol. Real NSView embedding and resizing stay a separate gate in sandboxed GPhil.

**Added to Phases 2–3:**
- **Ports:** `audio-ports` describes the layout, it doesn't negotiate one. Scan while deactivated. Supply every declared port: auxiliaries get silence, and layouts we can't route are rejected. Buffer counts match the declared counts.
- **Loader:** check `clap_version_is_compatible`. Copy descriptor strings. Canonicalise module keys. Pair every successful `init` with `deinit`, and never deinit after a failed init. Keep host storage alive until the plugin is destroyed.
- **Latency:** read on main during activation or while active. Don't restart recursively. Notify Dart only once the new value is readable.
- **Process policy (v1):** `process` is always called while eligible. `request_process` and sleep are honoured as no-ops. On a process error, output is silence. Steady time is a running sample counter.
- **Instruments:** the native/Dart instrument API (with note-ports and dialect) lands after effects, with its own tests.
- **State streams** handle partial reads/writes, EOF and errors.

Flutter 3.35+ runs the root isolate on the macOS main thread, and GPhil sets the merged-thread flag (`macos/Runner/Info.plist:46`). The main-thread guard stays for other isolates.

### 2026-11-15, round 2 (Sol 6.1 at HIGH re-reviewed Phases 2–7 after 4a): ACCEPT WITH AMENDMENTS

Round 1 ran at medium effort, so the remaining phases were reviewed again at HIGH. These amendments override the phase text above where they conflict.

1. **Host plumbing moves into Phase 2.** Plugins may query host extensions from `init()`. Phase 2 provides valid core host callbacks, deferred restart handling, the main-thread callback pump (16 ms) and per-instance `thread-check`. Optional extensions return null until implemented; never advertise an incomplete interface. Clear the pending-callback flag *before* calling `on_main_thread`, so requests made from inside it are kept.
2. **Flush without DSP callbacks.** When the instance is paused, bypassed, not yet processed or the device is stopped, a queued `request_flush` is serviced exclusively: exclude DSP entry, wait out in-flight processing, take the instance's symbolic audio role on main, flush, then restore eligibility. Inactive flush runs on main. `CLAP_PARAM_REQUIRES_PROCESS` changes are never sent through an active flush: they stay queued until processing resumes. `request_process` is a no-op only while processing is guaranteed; otherwise it marks the instance for the flush/servicing path and the behaviour is documented.
3. **Restart rebuilds the configuration while deactivated.** Between deactivate and activate: re-read ports and parameters, invalidate changed mappings, cookies and queued edits, validate routing, and rebuild scratch and event metadata. Host `audio-ports` is advertised only with truthful rescan flags. An unsupported new layout is rejected safely, leaving processing disabled. New latency and restart notifications go out only after a successful reactivation; `host_latency.changed` is only legal on main while being activated.
4. **Shared window extended narrowly for 4b.** Add a reversible hide that keeps the parent view and GUI resources; the CLAP adapter tracks created, visible and destroyed separately, and a hide request never becomes an editor-closed event. `can_resize == false` → `Fixed`, otherwise `Native`; apply axis/aspect hints, call `adjust_size` while constraining, `set_size` for host-origin resizes, and on `set_size` false restore the last accepted size without recursion. Handle `resize_hints_changed`. Native size limits become configurable (VST3 keeps half-to-four-times). Floating mode stays separate, with a valid transient parent, and never runs the embedded sizing calls. Round 1's no-`set_scale`, asynchronous resize rollback and `closed(true)` → destroy rules stand.
5. **The minimal Dart wrapper is a gate before the 4b live probe.** It exposes: support detection, scan/free, descriptor and plugin id, effect load onto `CustomFilterGlobal` with callback/userdata binding; idempotent dispose and host unload-all, including slot cleanup when binding fails; latency, state round-trip, parameter metadata/get/set and bounded native-event draining; and for the GUI probe `hasEditor`, open/close, `isEditorOpen` and a single editor-closed notification. These are the seams GPhil's backend uses (`lib/services/vst3/vst3_chain_backend.dart`); Phase 7 generalises GPhil's small plugin interface rather than adding a package-wide host abstraction. Restart/change events are CLAP-specific, not Steinberg flags. Parameter events stay gesture-aware plain values; slot automation semantics (plain vs normalized, stepped and zero-range parameters) are defined explicitly. No VST3-style parameter-value fallback when the plugin has no `state` extension.
6. **The mock harness starts with Phase 2.** A standalone `.clap` mock bundle (vendored headers, `clap_entry`, CoreFoundation/AppKit; no Flutter or VST3 SDK) with a runner mirroring `test/run_vst3_host_test.sh`, including the sanitizer modes. Gates: lifecycle and thread assertions for Phase 2, extensions for Phase 3, GUI protocol for 4b. Added cases: paused-flush progression, restart topology changes, reentrant callback requests, hide/show, rejected host resize, and a scan while another instance of the same module processes (entry init/deinit must not overlap other calls into the module). Real sandboxed GPhil screenshots remain a separate gate.

### 2026-11-15, Phase 3 design (Sol 6.1 at HIGH, two rounds, then Fable 5.2 arbitration)

**Escalation rule (set by the project owner for all phases):** when Sol still disagrees after two review rounds, the dispute goes to Fable 5.2 at medium effort with both positions, the diff and the evidence. Its decision is adopted without asking the owner and is recorded here.

1. **Round 1, REJECT (11 findings).** Revision 1 answered every finding:
   - edits are tied to the cache generation;
   - retained `REQUIRES_PROCESS` edits are bounded;
   - work is taken before the call;
   - DSP consumption is bounded;
   - out-events are gesture-safe and have a resync handshake;
   - lifecycle phases are explicit and rescans are latched;
   - there is a guard protocol;
   - timers can be registered from init;
   - the idle heuristic is described honestly;
   - one order contract applies to all of the above.
2. **Round 2, REJECT (blockers B1–B5).**
   - B1: the resync boundary.
   - B2: immediate invalidation on `rescan(ALL)`.
   - B3: notification starvation.
   - B4: retained saturation.
   - B5: timer teardown order.
3. **Fable arbitration (binding).**
   - **Out-ring boundary:** a lock-free resync boundary on free-running 64-bit ring positions, with a persistent consumer phase.
   - **Reset rule:** the main thread issues the out reset only while it holds `mProcessMutex`.
   - **Pending cache:** `rescan(ALL)` installs a pending sentinel under `mEditProducerMutex` → `mParamsMutex`. It is rebuilt at three defined points, and a rebuild is published only if the generation is unchanged (at most 3 attempts).
   - **Contract while pending:** edits fail, the count is 0, and `resync(paramsReset)` is never delivered.
   - **Required tests:** the arbitration also named the tests listed in the Phase 3 result below.

## Phase results

### Phase 0: VST3 editor UI verified (2026-11-15, Sol 6.1 ACCEPT in round 3)

Live macOS checks, committed in GPhil `v2_daw` 51e5ae22 (`integration_test/vst3_plugin_editor_live_*`):

- Lifecycle matrix (Plugins panel, TDR Nova and ZeroEQ): open, resize through System Events, close with the window's own close button, reopen, remove with the editor open, re-add and open, engine-shutdown release with the editor open. All windows were counted by the test's own pid and every step was screenshotted. Result: all tests passed. Nova 830x630 to 1052x790; ZeroEQ 760x572 to 1000x732 and reopens at its last size.
- App close (real `app.main()`, one session per plugin): the close guard is armed and the editor is open; a click on the main window's close button runs the app's close handler (`close_event` marker); the chain releases 16 ms (Nova) / 19 ms (ZeroEQ) later; the process exits within 100 ms; no crash report within 15 s. Not captured: the OS exit status. Termination goes through `NSApp.terminate`.
- Review rounds: round 1 fixed the driver's pid binding, error handling and step names. Round 2 rejected because app close covered only one plugin and lacked evidence of a normal exit. Round 3 accepted.

No VST3 editor defects were found, so Phase 4a extracts the existing window behaviour as is.

### Phase 4a: shared native editor window (2026-11-15, Sol 6.1 ACCEPT in round 1)

`src/plugin_ui/plugin_host_window.h` and `plugin_host_window_macos.mm` own the NSWindow, its delegate, sizing (Fixed, Native, HostScaled with Cmd +/-/0 zoom), focus, occlusion and termination. `Vst3EditorWindowMacOS` is now an adapter (IPlugFrame and `PluginHostWindowListener`); the out-of-process editor helper stays in the VST3 file. The hook defines `SOLOUD_ENABLE_PLUGIN_UI` and builds the window whenever a plugin format is on macOS, and links it into the VST3 editor helper.

- Builds without warnings under ARC (the helper) and without it (the plugin library).
- `test/run_vst3_host_test.sh`: plain, TSan and ASan, 114 assertions, 0 failures.
- Hook matrix: all four `enable_vst3` x `enable_clap` configurations build and pass `test/clap_stub_test.dart`. With CLAP alone the window is compiled but dead-stripped until 4b uses it.
- Live GPhil, Debug, built from this worktree (checked in the binary): the Phase 0 lifecycle matrix passed for TDR Nova and ZeroEQ, and app close released the chain 18 ms after the close event for both, with a clean exit and no crash reports.
- Testing note: `flutter test` does not re-resolve after `pubspec_overrides.yaml` changes. Run `flutter pub get` and check the built binary before trusting a run against another checkout.

### Phase 2: native core (2026-11-15, Sol 6.1 ACCEPT in round 2)

`src/clap/` holds the module registry and cache (`clap_module`), the effect instance with its host struct, ports, scratch and event lists (`clap_instance`, `clap_event_list.h`), and `ClapHostManager` (`clap_host`). The manager owns the handle table, the main-thread lifecycle sequences, deferred restart and callbacks on a 16 ms pump, and barrier-quiesced retirement batches with an epoch that stops anything from coming back after `unloadAll` or dispose. `src/clap_bindings.cpp` is the thin C ABI. Host extensions other than `thread-check`, the Dart wrapper, scan search paths and instruments are deferred to Phases 3 and 5, as in the design.

- Ports: only a port 0 flagged `CLAP_AUDIO_PORT_IS_MAIN` is routed. Without a main input, every input is auxiliary and gets silence. Without a main output, the layout is rejected: load fails, and a restart leaves the instance Failed (silent) until a later good restart.
- Dispose: under the engine's write lock it only retires CLAP instances (`retireAllForDispose`) and detaches the VST3 edit callback. `Vst3HostManager::unloadAll()` runs after the write guard, still under `init_deinit_mutex`, because it waits for the main thread, and the main thread's plugin barrier takes the lifetime lock shared.
- A load captures the epoch before it waits for main, so a dispose or `unloadAll` that runs while it is queued cancels it.
- Tests: `test/run_clap_host_test.sh` (a mock `.clap` bundle and 24 harness tests, including the C ABI from a worker) passes in plain, ASan+UBSan and TSan builds. `test/run_engine_lifecycle_test.sh` (222 assertions, with CLAP compiled in and a dispose-versus-main-barrier deadlock test) and `test/run_vst3_host_test.sh` (114 assertions) pass in all three. The four-configuration hook matrix passes `test/clap_stub_test.dart`.
- Mutation probes: re-reading the epoch on main, ignoring `IS_MAIN`, ignoring a failed scratch allocation, and shutting plugins down under the lifetime lock each fail their test.
- Review: round 1 rejected the queued load surviving dispose, the cross-host dispose deadlock, ignored `IS_MAIN` flags, unhandled scratch OOM, and the untested successful C ABI. All five are fixed above. Round 2 accepted: it found no mixing window from the dispose reordering, because `Player::dispose()` stops the device and backend before the player is replaced and `init_deinit_mutex` is held through the VST3 unload. It also accepted the input-side policy. The deadlock test uses a main-thread shutdown stand-in, not a real VST3 editor.

### Phase 3: host extensions (2026-11-15, Fable 5.2 arbitration ACCEPT after two Sol 6.1 rounds)

**Host extensions advertised:**
- `thread-check` and `log`. Audio-thread log messages are dropped and counted.
- `params`: `rescan`, `clear` and `request_flush`.
- `state`: `mark_dirty`.
- `latency`: `changed`, legal only while being activated.
- `timer-support`: at most 64 timers, clamped to the 16 ms pump.
- `audio-ports`: truthful rescan flags.
- `gui` stays null until 4b.

**Plugin side:**
- A parameter metadata cache, off the DSP path.
- Host → plugin edits through a bounded SPSC queue (`src/clap/clap_spsc.h`). Edits are validated against the cache generation. `REQUIRES_PROCESS` edits are retained until processing resumes.
- Flush servicing while paused or inactive.
- State save/load, limited to 64 MiB.
- Latency is published after each activation.
- A plugin → host out ring that never leaves a gesture stuck open. An overflow or a parameter reset becomes a single `resync` event.

**Notifications:**
- Five are coalesced and drained in this fixed order: restartFailed, restarted, latencyChanged, paramsRescan, stateDirty.
- `editorClosed` is reserved for 4b.
- They are drained after the ring and its resync.

**C ABI:** `soloud_clap{GetLatencySamples, GetParameterCount, GetParameterInfo, GetParameterValue, ParameterValueToText, ParameterTextToValue, SetParameterValue, GetState, SetState, DrainEvents}` and `ClapHostEventType` (C and Dart). Bindings are mirrored by hand, with inert stubs when CLAP is off.

**Consumer contract for Phase 5:** a `resync(paramsReset)` means the list was settled when the event was acknowledged. A later `rescan(ALL)` can make it pending again before Dart re-queries, and another resync always follows. So Dart treats a pending list (count 0, or `unknownError` on set) after a reset as "wait for the next resync". While a reset is held behind a pending list, notifications such as `restartFailed` wait too. They are delivered after the resync.

**Tests:**
- `test/run_clap_host_test.sh` runs 56 harness tests against the mock bundle (new knobs for edits, flushes, out-events, state, latency, timers, port-query rescans and reentrant hooks). It passes in plain, ASan+UBSan and TSan builds, with zero lock-order violations asserted after every test.
- The four-configuration hook matrix passes `test/clap_stub_test.dart`.
- `dart analyze` shows only pre-existing infos.

**Mutation probes (11), each failing its test:**
- the drain's pending re-check removed;
- the drain's acknowledgement taken outside `mParamsMutex`;
- the reset flag dropped;
- the settle-before-activate removed on load, or on restart, or its result ignored;
- edits never treated as stale;
- `REQUIRES_PROCESS` ignored;
- audio-thread logs emitted;
- the timer clamp removed.

**Implementation review:**
- **Round 1, Sol REJECT.**
  - (1) A reset ABA: a second inactive `rescan(ALL)` on an already-reset word let the drain announce the reset while the list was pending.
  - (2) A `rescan(ALL)` from inactive port queries was serviced only after activation, causing a restart cycle.
  - Fix for (2): `settleParamsBeforeActivate()` rebuilds after `readPorts` and before activation, on load and on restart. A list that never settles fails the load (`fileLoadFailed`) or the restart (Failed, inited, recoverable).
  - Fix for (1): a reset tag in the out-state word.
- **Round 2, Sol REJECT.**
  - The tag caught only a completed reset, not the gap between the pending store and the reset. An active `rescan(ALL)` never changes the word at all.
  - Fix: the drain's pending check and its acknowledging CAS now run under `mParamsMutex`, the lock under which both pending transitions happen. That adds the edge `mDrainMutex` → `mParamsMutex`, and `mParamsMutex` stays a leaf lock.
  - With that, the tag became redundant: no test detected its removal. It was removed.
  - New coordinated tests pause the consumer inside the acknowledgement and main between the pending store and the reset, for both inactive and active, plus an active-rescan hold test.
- **Fable arbitration, ACCEPT.**
  - The guarantee holds for inactive and active `rescan(ALL)`. Both `mParamsPending` stores are under the lock, and an off-main `rescan` is rejected by the thread guard.
  - The tag removal is right, and no reset can be lost.
  - There is no deadlock or audio-thread impact: DSP never takes `mParamsMutex`.
  - The tests exercise the real interleaving with bounded waits.
  - Non-blocking notes: the consumer contract above; the pending sentinel is installed even for created or retired instances, as the arbitration's B2 specifies.

### Minimal Dart wrapper (2026-11-15, Sol 6.1 ACCEPT in round 3)

`lib/src/clap/` holds the first slice of the Phase 5 API, exported from `flutter_soloud.dart` as `@experimental`:
- `ClapHost`: `isSupported`, `hostClapVersion`, `standardSearchPaths`, `scanBundle`, `unloadAll` and `loadEffect({bundlePath, slot, pluginId, sampleRate, maxBlockSize, channels})`. Without a `pluginId`, `loadEffect` takes the first `audio-effect` plugin, or else the first plugin that is not an instrument.
- `ClapEffectPlugin`: `parameters`/`refreshParameters`, plain get/set, value↔text, save/restore state, `latencySamples`, `drainEvents({maxEvents})` and an idempotent `dispose`. Disposing after `unloadAll` is still safe.
- The value types `ClapPluginInfo`, `ClapParamInfo` and `ClapHostEvent`, and a web stub.
- `drainEvents` refreshes `parameters` on a `resync(paramsReset)` or a `rescan(INFO|ALL)`, following the Phase 3 consumer contract.

**Slot ownership** (`clap_slot_owners.dart`, internal):
- Each native slot is keyed the way the native side routes it. Sound and bus are unsigned 32-bit values, a nonzero sound wins over the bus, and sound 0 with bus 0 is the global chain.
- The key maps to the last CLAP plugin bound to the slot, held weakly, together with `SoLoud.engineSession`. That counter goes up on every teardown, including init's hot-restart teardown.
- `dispose` unbinds and deactivates the slot only while that plugin is still the owner in the current session.
- A failed bind deactivates only a slot it activated itself. The native callback setter leaves the slot unchanged on every error.
- Binds made through VST3 or custom callbacks are invisible to this registry: dispose a CLAP plugin before reusing its slot for anything else.

**Text:** names use 256 bytes, modules 1024 and formatted values 512. All plugin text is decoded with `allowMalformed`, so a truncated multi-byte character never throws.

**Tests:**
- `test/clap_wrapper_test.dart` (runs in the hook matrix, 18/18 in all four configurations) covers:
  - value types;
  - text decoding;
  - slot keys and owners;
  - the paths that do not need the main thread;
  - the web stub.
- `example/integration_test/clap_wrapper_test.dart` runs live against ZeroEQ (9/9):
  - scan;
  - load, edit, state and dispose;
  - `unloadAll`;
  - stale disposal after `unloadAll` and after reload, after loading over a live plugin, and after a deinit/init restart;
  - slots that alias one native slot;
  - failed loads.
- An opt-in group with `CLAP_MOCK_BUNDLE` uses the mock's new `mock_clap_emit_gestures` export. It checks that a bounded drain leaves the remaining events queued and in order. ZeroEQ cannot show this, because notifications of one kind coalesce.
- Mutation probe: the unguarded dispose fails both stale-disposal tests.

**Review:**
- Round 1 (ACCEPT WITH AMENDMENTS) found three things:
  - a stale dispose removing a replacement's slot;
  - undersized buffers and a throwing decode;
  - a weak bounded-drain test.
- Round 2 (ACCEPT WITH AMENDMENTS) found two more:
  - sound/bus aliases getting separate keys;
  - retired sessions retaining plugin objects.
- Round 3 accepted with no findings.

### Phase 4b: CLAP gui (2026-11-15, Fable 5.2 arbitration ACCEPT after two Sol 6.1 rounds)

`src/clap/clap_editor.{h,cpp}` (macOS, `SOLOUD_ENABLE_PLUGIN_UI`) is the host side of one instance's gui, in the shared `PluginHostWindow` from 4a.

**Flows:**
- Embedded: `create(cocoa, false)` → `can_resize` → `get_size` → the window opens → `get_resize_hints` → `set_parent(content view)` → `show`. No `set_scale`: cocoa sizes are logical points.
- Floating fallback, when embedding is not offered or fails: `create(cocoa, true)` → `suggest_title` → `set_transient(application window)`, which is optional → `show`.
- Close: `hide` (when shown) → `destroy` → the parent view is released. This order holds on every path, and the gui always goes before deactivate or unload.
- User resize: `adjust_size`/`set_size` through `constrainUserResize`. An accepted size becomes the window's base size (`setAcceptedUserSize`), so native limits and a locked axis follow it. Resize hints map to native limits.

**Plugin requests:**
- On main, `request_resize` applies at once and sends no `set_size` back.
- Off main, requests are posted as `GuiRequestBit` flags and the pump applies them under the call guard. An off-main `request_resize` is acknowledged, then applied, or reverted with `set_size(accepted)`.
- Every `request_resize` gets a serial (wrap-safe, int32 difference). A queued request older than one already applied is dropped. Each new gui starts after every serial issued so far.
- `request_show`, `request_hide` and an off-main `request_resize` are refused unless a gui exists and no destroy is pending (`mEditorCreated`).
- `closed(was_destroyed)` destroys the gui and reports `editorClosed`.

**Nested run loops:**
- A user close or an application termination that arrives during a plugin call (when the guard cannot be acquired) closes the window at once. The gui is destroyed on a later pump tick.
- The content view the gui is parented to is retained until that destroy (`retainNativeView`/`releaseNativeView`). It is also retained across the host's own `hide`/`destroy`, in case the user closes the window from a run loop one of them spins.
- If the user closes the window while the host opens or reshows the editor, the open fails, `editorClosed` is reported, and the pump destroys the gui.

**Events and C ABI:**
- `editorClosed` is emitted only for a user close or a plugin `closed()`. A host close, a hidden editor, an app termination or an unload emits nothing.
- `soloud_clap{HasEditor, OpenEditor, CloseEditor, IsEditorOpen}`, with inert stubs when CLAP or plugin UI is off.

**Dart:** `ClapEffectPlugin.hasEditor`, `openEditorWindow({String? title})` returning bool, `closeEditorWindow()` and `isEditorOpen`, mirroring VST3. Dispose closes the editor. The web stub declares the same API. `paramEdits` stays in Phase 5.

**Tests:**
- The native harness, now 65 tests including gui_open_close, gui_hide_show, gui_user_resize, gui_request_resize, gui_closed, gui_failures, gui_floating, gui_lifetime and gui_abi, uses a fake `PluginHostWindow` (`test/clap_fake_window.{h,cpp}`) that can act as the user (close, resize, terminate) and tracks the content view's lifetime. It passes in plain, ASan+UBSan and TSan builds.
- The live example test passes 11/11. ZeroEQ's real editor opens, reopens, closes, reopens again and closes on dispose. A mock group follows the gui modes the plugin offers.
- The hook matrix passes 18/18 in all four configurations. Syntax checks are clean in every CLAP/plugin-UI configuration, and for the `.mm` with and without ARC. `flutter analyze` shows only the 4 pre-existing infos.

**Mutation probes, each failing its test:** the serial check; the parent retain on a deferred destroy; `setAcceptedUserSize`; show/hide gating; servicing a pending destroy first; the retain in `destroyGui`; the deferral on a nested terminate; the unsigned serial comparison, in `service` and in the CAS; the baseline at open; failing an open closed meanwhile (on first open and on reshow); and the off-main resize gate.

**Review:**
- **Round 1, Sol REVISE.** Five fixes:
  - stale off-main resizes overriding newer ones → serials;
  - a deferred destroy outliving its parent view → retain;
  - accepted user sizes not becoming the window base;
  - show/hide accepted with no gui;
  - the pending destroy not serviced first.
- **Round 2, Sol REVISE.**
  - (1) `destroyGui` cleared `mCreated` before `hide`/`destroy`, so a close from their run loop did not retain the parent.
  - (2) A nested application termination closed the window but left the gui created and its parent unprotected.
  - (3) Unsigned serial comparisons broke across the 32-bit wrap.
  - Fixes: `retainParent()` in `destroyGui`; a shared `deferDestroy()` for a nested close and a nested terminate; `guiResizeNewer` plus a baseline when a gui is created.
- **Fable arbitration, ACCEPT, no blocking findings.**
  - All three fixes are correct. `isOpen()` holds during `onShouldClose`. A terminate with a destroy already pending does not destroy twice. A request racing `open()` is either dropped or applied by the pump.
  - Two of its minors were folded in before landing: an open the user closed meanwhile now fails instead of returning true, and an off-main `request_resize` is gated on a live gui.
  - The other minors are left as non-blocking:
    - a close during `get_resize_hints` reports `editorClosed` for a failed open, which is accurate because the user closed it;
    - request bits cleared while the plugin is not callable, which teardown or the next open covers;
    - a busy `closeEditorWindow()` from a reentrant call is ignored.

### Phase 5a: effect API completion (2026-11-15, Fable 5.2 arbitration ACCEPT after two Sol 6.1 rounds)

Phase 5 is split in two. 5a completes the effect API; 5b adds instruments (`loadInstrument`, `ClapInstrumentPlugin`, note ports and dialect).

**Filter-slot parameter mapping (native):**
- SoLoud filter-slot parameters 1..15 (0 is the wet) drive CLAP parameters. Each slot has a seqlock cell holding the id, cookie, range, flags, parameter-list generation, version and baseline. Writers take `mSlotMapMutex`, before `mParamsMutex`, and never hold it across a plugin call. DSP reads the cell lock-free and skips a torn read.
- After `consumeEditsDsp`, `pushSlotEventsDsp` turns each changed slot value into a `CLAP_EVENT_PARAM_VALUE` at time 0. The value is mapped linearly onto the parameter's range; stepped, enum and bypass values are rounded.
- Arming is per mapping version. The version changes on map, unmap and a drop, but not on a re-resolve.
- The baseline is the slot value read when mapping. Until something is sent, it counts as sent: it is compared as a slot value under the current mapping, so a range change cannot fake a match. A slot value that differs is sent exactly once, even across dry blocks, seqlock skips or cleared changed bits. Without a baseline (NaN, C callers), the slot's changed bit decides, as a best effort.
- A block against a stale parameter list changes no state and waits for the re-resolve. A full event list leaves the value to be retried next block, and later slots still arm. Non-finite slot values are skipped.
- `resolveSlotMappings` runs after every params publish: it follows new cookies and ranges, and unmaps a parameter that is gone or now read-only.
- Refusals: slot 0 or ≥16, an invalid id, and an unknown or read-only parameter give `invalidParameter`; a pending list gives `unknownError`.
- C ABI: `soloud_clapMapSlotParameter(handle, slot, paramId, float slotValue)` and `soloud_clapUnmapSlotParameter`.

**Dart:**
- `ClapParamInfo.toNormalized`/`fromNormalized` (snapping for stepped, enum and bypass; 0 or the minimum for an empty range), `parameterInfo`, `getParameterNormalized` and `setParameterNormalized`.
- `mapToSlotParameter({slotParamIndex 1..7, paramId, soundHandle})` reads the slot value as the baseline and returns a `CustomFilterParam` for set, fade and oscillate. `unmapSlotParameter` removes the binding.
- `events` and `paramEdits` (`ClapParamEditEvent` with value, gestureBegin and gestureEnd, the plain and normalized value, and dontRecord) are broadcast streams. While listened to, they are drained every 16 ms, at most 8×1024 events per tick. `drainEvents` delivers to the listeners too.
- After `unloadAll`, the first drain marks the handle gone and polling stops for good. `dispose` closes both streams.
- The web stub declares the same API. The README has an opt-in CLAP section.

**Tests:**
- The native harness has 70 tests, including slot_mapping, slot_mapping_rescan, slot_mapping_race (deterministic mid-write and mid-read hooks plus a concurrent remap stress), slot_mapping_baseline and slot_mapping_baseline_rescan. slot_mapping_baseline covers a dry block during a paused flush and a remap over a live mapping that fails the seqlock read; slot_mapping_baseline_rescan covers Sol's stale-generation case. It passes in plain, ASan+UBSan and TSan.
- The live example test passes 14/14:
  - ZeroEQ: normalized values, and a slot parameter driving it with set and fade.
  - Mock: the streams with kinds and values; mapping right after `configureParameters` sends nothing; a slot value set just before mapping is not sent, a later one is; read-only refused; `unloadAll` stops polling.
- The hook matrix passes 20/20 in all four configurations. `flutter analyze` shows only the 4 pre-existing infos.

**Mutation probes, each failing its test:**
- Native: no baseline arming; no baseline copy on re-resolve; no cell store or DSP load of the baseline; the raw baseline instead of its plain value; the map dropping the baseline; the round-1 DSP; not clearing the unsent value after a send; not recording the standing value.
- Dart: no delivery; no timer cancel; no normalized value; snapping only stepped values; no empty-range guard; an always-NaN baseline; a listener restarting polling after `unloadAll`.

**Review:**
- **Round 1, Sol REVISE.**
  - (1) Blocking: an initial slot write was lost when the first block was skipped (dry during servicing, or a failed seqlock read) and SoLoud cleared the changed bit. Fix: the baseline.
  - (2) After `unloadAll`, the drain timer kept polling the dead handle. Fix: stop on `invalidParameter`.
- **Round 2, Sol REVISE.** (2) resolved. New blocking finding: the generation check ran after arming, so a block against a stale list stored the baseline as a plain value in the old range, and after a range change a real change could match it and be suppressed forever. Fix: the generation check comes first, and an unsent baseline is kept as a slot value compared under the current mapping.
- **Fable arbitration, ACCEPT, no blocking findings.** The round-2 fix is correct. Full lists, non-finite values, unmap and remap, a dropping re-resolve, a map racing a re-resolve, the seqlock and the DSP cost all hold. Its three minors were folded in before landing: `continue` instead of `break` on a full list, and two comment fixes.

### Phase 5b: instruments (2026-11-15, Sol 6.1 ACCEPT in round 1)

**Native:**
- `ClapInstance` has an instrument mode. An instrument's audio inputs are silent. SoLoud renders it through `clapSourceDspCallback`, a `SoLoudSourceDspCallback_t` whose user data is the handle.
  - An unknown handle ends the source.
  - An effect handle renders silence.
  - `clapFilterDspCallback` skips instruments.
- `renderInstrument` shares `runBlock` with the effect path. It writes every frame: silence while servicing, failed, retired or contended.
  - It renders the plugin's main output to the first two channels; channels past that are silent.
  - A mono output goes to both channels.
  - A null buffer, zero samples or zero channels returns 0.
- **Note ports.** `note-ports` is read with the audio ports while deactivated. Port 0's dialect is chosen this way:
  - the preferred dialect when it is CLAP or MIDI 1.0;
  - else CLAP when supported;
  - else MIDI 1.0;
  - else none (MIDI 2.0 only, no port, or a broken extension).

  The host's `supported_dialects` is CLAP | MIDI. A `rescan` with `CLAP_NOTE_PORTS_RESCAN_ALL` restarts the plugin; other flags are ignored. A call off the main thread is logged.
- **Note queue.** `sendNote(on, channel 0..15, key 0..127, velocity, sampleOffset)` runs on any non-audio thread and never waits for main.
  - Producers are serialised by `mNoteProducerMutex`, a leaf lock, onto an SPSC ring of 1024 notes. A full ring gives `outOfMemory`.
  - DSP stages up to 512 notes per block, with offsets clamped to the block, and pushes them sample-accurately within each chunk.
  - A CLAP-dialect plugin gets `NOTE_ON`/`NOTE_OFF` with the velocity clamped to 0..1. A MIDI plugin gets a MIDI 1.0 message with the velocity scaled to 1..127 (on) or 0..127 (off).
  - Notes stay queued while a block is dry or fails. A note whose dialect went away in a restart is dropped and counted.
- **Note end.** `CLAP_EVENT_NOTE_END` from the plugin becomes host event `noteEnd` (11): the note id, the key, and `(port << 16) | channel` in the flags.
- **C ABI.** `soloud_clapLoadInstrument`, `soloud_clapGetSourceCallbackPtr`, `soloud_clapSendNote` and `soloud_clapGetNoteDialect`.

**Dart:**
- `ClapHost.loadInstrument({bundlePath, pluginId, sampleRate, maxBlockSize, channels})`. It takes the first instrument when no id is given, and creates a `singleInstance` custom audio source. If the source fails, the plugin is unloaded.
- `ClapInstrumentPlugin` (a `ClapPluginBase`) has:
  - `audioSource`;
  - `noteDialect` (`ClapNoteDialect` none/clap/midi);
  - `noteOn`/`noteOff({pitch, velocity, channel, sampleOffset})`, which check the pitch, channel and offset ranges in Dart before the FFI call narrows them, throw for no dialect and are no-ops after `dispose`;
  - `dispose`, which disposes the source too.
- `ClapHostEventType.noteEnd` with the decoded note.
- The web stub mirrors the API, and the README shows an instrument.

**Mock:** three synths:
- `mock.synth` (CLAP and MIDI, stereo);
- `mock.synth_midi` (MIDI only, mono);
- `mock.synth_midi2` (MIDI 2.0 only).

They record each note with its block time and steady time, render `0.1 × held notes` sample-accurately, and emit `NOTE_END` on release. Their knobs switch dialects on deactivate, break or remove `note-ports`, and override the preferred dialect.

**Tests:**
- The native harness has 77 tests (`instrument_load`, `instrument_notes`, `instrument_queue`, `instrument_producers`, `instrument_midi`, `instrument_restart` and `instrument_abi` are new). `instrument_producers` races two note producers against a rendering audio thread and checks that every accepted note arrives once, in order. It passes plain, ASan+UBSan and TSan.
- The hook matrix passes 24/24 in all four configurations, with 29 exported CLAP symbols.
- The live example test passes 15/15. Its mock synth test covers default id, play, note on/off, a drained `noteEnd`, range errors, dispose, mono MIDI, and MIDI 2.0 giving `none`.
- The Dart unit and stub tests pass 24/24. `flutter analyze` shows only the 3 pre-existing infos.

**Mutation probes, each failing its test:**
- the chunk window bound and the chunk-relative event time;
- the offset clamp, the velocity clamp and the non-finite velocity check;
- the MIDI status byte and the MIDI on-velocity floor of 1;
- preferring MIDI, ignoring a `NAMES` rescan, and counting dropped notes;
- staging notes in a dry block, the full ring, silence on a failed block and zeroing the extra channels;
- the filter callback skipping instruments, silent instrument inputs, and an unknown handle ending the source;
- the `noteEnd` flags;
- removing `mNoteProducerMutex` (`instrument_producers` fails, and TSan reports the race).

The first runs of the chunk window and prefer-MIDI probes survived. A boundary note at 128 and the `notePreferredDialect` knob now kill them.

Three survivors are accepted as equivalent:
- removing the CLAP-preferred rule (CLAP is the fallback anyway);
- dropping MIDI 2.0 from the spoken mask (it is never chosen);
- the end-of-block `mBlockNoteCount = 0` (staging resets the count).

**Review:** Round 1, Sol ACCEPT with no blocking findings. Both of its minors were folded in before landing:
1. `noteOn`/`noteOff` passed `pitch` and `channel` straight into 32-bit FFI arguments, so `pitch: 0x10000003c` played key 60. Dart now range-checks both, and the live test covers oversized values.
2. Concurrent producers had no regression test. `instrument_producers` was added.

### Phase 6: tests (2026-11-15, Sol 6.1 ACCEPT in round 2)

Most of the planned coverage already landed with the earlier phases:
- Dart unit tests against the stubs (`test/clap_wrapper_test.dart` and `test/clap_stub_test.dart`, 24 tests).
- The native harness (77 tests, plain, ASan+UBSan and TSan).
- The ZeroEQ live group: scan, load, values, state round-trip, latency, slot mapping, the editor, and an engine restart.

Phase 6 adds the two things still missing. Both are in `example/integration_test/clap_wrapper_test.dart`.
- **"installed CLAP plugins".** Every bundle in the standard CLAP folders is scanned, and each of its plugins is checked. This is the counterpart of GPhil's `vst3_master_chain_probe_test.dart`, run here over all installed plugins, and it is a smoke test for arbitrary third-party plugins.
  - An effect is loaded into a global slot with a 440 Hz tone playing. The test checks for a non-negative latency and a finite output peak. The peak is a meter smoke check only: it is read after the engine's hard clipper and 0.95 scaler, and plugins may be silent or loud at their defaults.
  - An instrument plays a held note into its audio source, and its peak is read while the note is held. A plugin with dialect `none` gets no notes, and the log says so.
  - **State** is reported as one of three levels, because the CLAP `state` extension is optional:
    - `none`: `saveState` throws `SoLoudNotImplementedException`, which is now documented.
    - `smoke`: no writable continuous parameter takes an observable edit. Up to four candidates are tried, each in both directions, because a linked parameter may clamp an edit.
    - `verified`: a parameter must first move at least 10% of its range away from the saved value (polled), then come back within 0.1% of the range after `restoreState`.
  - The editor is opened and closed when the plugin has one. This checks the lifecycle the API reports, not what is on screen. `dispose` must free the slot.
  - Each plugin is cleaned up in nested `finally` blocks, so one failure, even in a cleanup step, leaves nothing loaded or paused.
  - On this machine ZeroLimit, ZeroComp, Airwindows Consolidated and ZeroEQ all report `state verified`.
- **"plugin audio reaches the engine output"** (mock bundle). It measures the real output peak with `getApproximateVolume`, with visualization on. Earlier live tests only proved that values reached a plugin; this proves the plugin's audio is what plays.
  - `mock.stereo` halves the dry peak. The dry and halved ranges do not overlap.
  - Slot wet 0 bypasses it and wet 1 brings it back.
  - `dispose` restores the dry peak.
  - With the tone paused, two held notes on `mock.synth` give its 0.2 level in the mix, and releasing them gives silence.
  - Its cleanup runs as tear-downs, so it also runs after a failed expectation.

**Verification:**
- The live example test passes 17/17. It passed twice before the round-1 fixes, once after them and once after the round-2 minors.
- `flutter analyze` shows only the 3 pre-existing infos.
- A no-op `restoreState` fails the installed sweep (ZeroLimit's Threshold stays at 0.2 after restoring 1.0).
- Native code is unchanged, so the native harness and the matrix still stand from Phase 5b.

**Review:**
- **Round 1, Sol REVISE.**
  - (1) Blocking: state support was treated as mandatory. Fix: the `none`/`smoke`/`verified` levels.
  - (2) Blocking: the round-trip could pass with a no-op restore, because the edit was never confirmed. Fix: polled move and return, with tolerances relative to the range.
  - Minors folded in: (3) the peak is now described as a meter smoke check; (4) cleanup runs on failure; (5) instrument coverage is described as smoke, with a held-note peak and dialect `none` reported.
- **Round 2, Sol ACCEPT.** No blocking findings. Two minors, both folded in:
  - The first eligible parameter may not take an edit (for example, clamped by a linked one). Fix: try both directions and up to four parameters, report `smoke` when none moves, and keep a failed restore fatal once a move is seen.
  - A throwing cleanup step could skip the rest. Fix: the instrument cleanup is nested `try`/`finally`. The effect cleanup is a single `dispose`.

### Phase 7: GPhil integration (2026-11-15, Sol 6.1 ACCEPT in round 1)

GPhil (`/Volumes/DEV/gphil-flutter`, `v2_daw`) now hosts CLAP effects in its experimental master chain, alongside VST3. The change generalises GPhil's small plugin interface; no fork API was added.
- **Backend** (`lib/services/vst3/vst3_chain_backend.dart`):
  - `PluginFormat { vst3, clap }`, carried on `Vst3BundleRef`. The backend's `load(bundle, slot)` dispatches on it.
  - Discovery covers `/Library/Audio/Plug-Ins/{VST3,CLAP}` and the same folders under the real home (the sandbox HOME is the container). It matches the extension case-insensitively and sorts by name, VST3 before CLAP.
  - A CLAP bundle is scanned, and the first non-instrument plugin (preferring `audio-effect`) is loaded onto `CustomFilterGlobal(slot)`.
  - Latency changes come from the drained `events` stream (`latencyChanged`, `restarted`).
  - `unloadAll` unloads both hosts, even when the first throws.
- **Controller and panel:** VST3 and CLAP share one chain, its slots, the pending-load crash marker, the latency total, the close guard and engine shutdown/resume.
  - Each row shows the format.
  - The add dialog badges each plugin (`PluginFormatBadge`, white70 on the dark dialog, at least 4.5:1, checked in a widget test).
  - The dialog's search also matches a format name.
- **Build and sandbox:** `enable_clap: macos` is in the pubspec user_defines. `DebugProfile.entitlements` has the CLAP read-only exceptions, both absolute and home-relative.
- **Tests:**
  - Two new controller tests: CLAP discovery, and a mixed VST3/CLAP chain with editor and release.
  - One new tab test: badges, the format search, a CLAP load and its editor.
- **Live checks:** these now include CLAP bundles. A `.clap` suffix in a probe name selects the CLAP bundle.
  - The editor driver reports whether the window accepts a new size (`AXSize` settable). Each probe declares its expected resizability (`VST3_EDITOR_FIXED_SIZE`, default `Airwindows Consolidated.clap`). The test asserts the reported value against it: a fixed-size editor must keep its size, and any other editor must change size.
- `APP_ARCHITECTURE.md` is updated. There is no help article, because the tab is debug/profile-only.

**Verification** (sandboxed Debug `GPhil.app`, built against this branch through a temporary path override):
- **Editor live driver: all four editors pass the full lifecycle.** The lifecycle is open, resize, close button, reopen, remove, open again, and release.
  - TDR Nova VST3: 830x630→1052x790.
  - ZeroEQ VST3: 760x572→1000x732.
  - ZeroEQ CLAP: 760x572→1000x412. The plugin's `adjust_size` set the height.
  - Airwindows Consolidated CLAP: fixed size. `can_resize` is false, so the host window drops `NSWindowStyleMaskResizable`. The driver reads `resizable=false` and the size stays 600x632.
- **Panel live test:** it passes with TDR Nova VST3 (184 samples) and ZeroEQ CLAP (0 samples). ZeroEQ CLAP was loaded from the home CLAP folder inside the sandbox.
- **App close with the ZeroEQ CLAP editor open:** the app exits cleanly. The plugin is released 17 ms after the close event, and there is no crash report.
- **Analyze and tests:** GPhil analyze is clean apart from 4 pre-existing infos in untouched tests. The controller and tab tests pass (29).
- The screenshots are kept local, because full-desktop captures show unrelated windows.

**Landing:** `v2_daw` depends on `flutter_soloud` through `path: ../flutter_soloud` (the `feature_capture-v5.0` checkout). After acceptance, `feature_capture-v5.0` is fast-forwarded to `clap-host`, so the GPhil commit builds without an override.

**Review:** Round 1, Sol ACCEPT. No blocking findings. One minor finding, folded in:
- The resize expectation was derived from the observed `AXSize` capability, so a resizable editor that regressed to fixed size would still pass.
- Fix: each probe now declares its expected resizability, and the test asserts the reported value against it.
- Re-run: all four editors pass. A deliberately wrong declaration (ZeroEQ.clap marked fixed) fails on `resizability`.
