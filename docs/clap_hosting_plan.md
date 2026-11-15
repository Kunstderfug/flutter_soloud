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

## Phase results

### Phase 0: VST3 editor UI verified (2026-11-15, Sol 6.1 ACCEPT in round 3)

Live macOS checks, committed in GPhil `v2_daw` 51e5ae22 (`integration_test/vst3_plugin_editor_live_*`):

- Lifecycle matrix (Plugins panel, TDR Nova and ZeroEQ): open, resize through System Events, close with the window's own close button, reopen, remove with the editor open, re-add and open, engine-shutdown release with the editor open. All windows were counted by the test's own pid and every step was screenshotted. Result: all tests passed. Nova 830x630 to 1052x790; ZeroEQ 760x572 to 1000x732 and reopens at its last size.
- App close (real `app.main()`, one session per plugin): the close guard is armed and the editor is open; a click on the main window's close button runs the app's close handler (`close_event` marker); the chain releases 16 ms (Nova) / 19 ms (ZeroEQ) later; the process exits within 100 ms; no crash report within 15 s. Not captured: the OS exit status. Termination goes through `NSApp.terminate`.
- Review rounds: round 1 fixed the driver's pid binding, error handling and step names. Round 2 rejected because app close covered only one plugin and lacked evidence of a normal exit. Round 3 accepted.

No VST3 editor defects were found, so Phase 4a extracts the existing window behaviour as is.
