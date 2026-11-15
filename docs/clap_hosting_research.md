# CLAP hosting in flutter_soloud (beside VST3): research

Date: 2026-11-14. Read-only research. No repo edits and no Flutter commands were run.
Fork: `/Volumes/DEV/flutter_soloud`, branch `feature_capture-v5.0`.
Labels: **[V]** means verified against a primary source or a file:line. **[E]** means an estimate or judgment.

## TL;DR

- **Feasible: yes.** CLAP is a C ABI made entirely of headers, under the MIT licence. **[V]** A CLAP host can live in `src/clap/`, mirror `src/vst3/`, and plug into the same two engine seams the VST3 host uses: the filter-slot DSP callback and `createCustomAudioSource`.
- **Build:** there's no prebuilt to download. Vendor (or fetch and pin) `free-audio/clap/include` and, optionally, `free-audio/clap-helpers`, then compile them into the main FFI lib behind `-DSOLOUD_ENABLE_CLAP`. That's simpler than `Vst3Link`.
- **Main cost:** writing a correct CLAP host (threading contract, event lists, GUI embedding on three platforms, host extensions such as timer-support/posix-fd on Linux). Estimate **[E]**: about 3–5 weeks for parity with today's VST3 feature set on macOS only, and about 6–9 weeks for all three desktops.
- **Value for GPhil today: low.** **No CLAP plugins are installed on this Mac** (`/Library/Audio/Plug-Ins/CLAP` and `~/Library/Audio/Plug-Ins/CLAP` don't exist, and Spotlight finds no `.clap` bundles). **[V]** Everything in use (iZotope, NI Kontakt, Pianoteq, SWAM, TDR, VSL/VEP, Audio Modeling) is VST3. I found no CLAP builds for Pianoteq, SWAM, Kontakt, VSL or TDR **[V: not found; not proof of absence]**.
- **Upstream:** alnitak/flutter_soloud has **no CLAP issue or PR**. A GitHub search for "CLAP" in that repo returns 0. **[V]** VST3 hosting arrived upstream through the user's PR #593 (plus hardening in #600 and #612).

## 1. Feasibility and licence/build

- The `free-audio/clap` licence is MIT (GitHub API `license.spdx_id = MIT`). **[V]** https://github.com/free-audio/clap/blob/main/LICENSE
- The spec is C headers only (`include/clap/*.h`, `include/clap/ext/*.h`, `ext/draft/*.h`). There's no library to link: the host `dlopen`s/`LoadLibrary`s/`CFBundle`-loads the plugin binary and resolves the `clap_entry` symbol (`entry.h`). **[V]** https://github.com/free-audio/clap/blob/main/include/clap/entry.h
- Latest tag: `1.2.10` (repo pushed 2026-09-14). **[V]** Pin a tag the way `Vst3Link` pins a prebuild tag.
- `free-audio/clap-helpers` is MIT and header-only C++ (`.hh`/`.hxx` templates). **[V]** https://github.com/free-audio/clap-helpers
  - `host.hh/.hxx`: a host-side base with misbehaviour checking.
  - `plugin-proxy.hh/.hxx`: a checked plugin wrapper that validates thread rules.
  - `event-list.hh`, `param-queue.hh`, `reducing-param-queue.hh`: event-list and queue types.
  - `preset-discovery-*`
- `free-audio/clap-host` is the MIT reference host, built on Qt (`host/plugin-host.cc`, `engine.cc`). **[V]** https://github.com/free-audio/clap-host
  - It's useful to read as the canonical host lifecycle, but its Qt dependency rules out reusing it as a library.

### Build implications vs VST3

VST3 today:
- `hook/build.dart:121-144` parses `enable_vst3` (a bool or a platform list, with env `SOLOUD_ENABLE_VST3` as a fallback) and calls `Vst3Link.forPrebuild`.
- `hook/build.dart:2352-2441` downloads `libvst3_hosting.a` and the headers from alnitak/flutter_soloud_prebuilds.
- `hook/build.dart:161-260` builds the `soloud_vst3_editor_helper` executable.
- `hook/build.dart:281-283` and `:371,:404,:420-444,:530-540` handle defines, source collection, frameworks and libs.

CLAP would need:
- An `enable_clap` user_define parsed exactly like `parseVst3Platforms`, plus env `SOLOUD_ENABLE_CLAP`.
- `-DSOLOUD_ENABLE_CLAP` and `src/clap/**` in `collectSources`.
- `-isystem` for the vendored `clap/include` (and `clap-helpers/include`).
- No new static lib. System libs are already present or trivial: `dl` on Linux, AppKit/QuartzCore on macOS, user32 on Windows.
- **[E]** Vendoring about 60 small headers into `src/clap/third_party/` is simplest. A download-and-pin step in the hook isn't worth it for headers.

## 2. Concept mapping to existing seams

| CLAP concept | Spec source **[V]** | Existing VST3 seam in fork | Share / new |
|---|---|---|---|
| Discovery: `.clap` files/bundles; mac `/Library/Audio/Plug-Ins/CLAP`, `~/Library/...`; Win `%COMMONPROGRAMFILES%\CLAP`, `%LOCALAPPDATA%\Programs\Common\CLAP`; Linux `~/.clap`, `/usr/lib/clap`; **host must also read `CLAP_PATH`** and search recursively | `entry.h:12-33` | `Vst3Host.standardSearchPaths` `lib/src/vst3/vst3_host_ffi.dart:84-117` | New list + `CLAP_PATH` parsing; same pattern |
| `clap_entry` → `init(path)` → `get_factory(CLAP_PLUGIN_FACTORY_ID)` → descriptors (id, name, features such as `"instrument"`, `"audio-effect"`) | `entry.h`, `factory/plugin-factory.h`, `plugin-features.h` | `Vst3HostManager::scanBundle` / `getOrLoadModule`, `Vst3DiscoveredClass` (`src/vst3/vst3_host.h:31-35,203-246`); `Vst3ClassInfo.isInstrument/isEffect` (`vst3_types.dart:6-36`) | New loader; descriptor ≈ class info (`id` replaces `classUid`, `features[]` replaces `subCategories`) |
| `create_plugin(host, id)` → `init` → `activate(sr, min, max frames)` [main] → `start_processing` [audio] → `process` → `stop_processing` → `deactivate` → `destroy` | `plugin.h:48-109` | `Vst3PluginInstance::initialize/shutdown/stopProcessing` (`vst3_host.h:56-64`) | New state machine; note **start/stop_processing must run on the audio thread** (lazy-start on first process) |
| `process()` with `clap_audio_buffer` (`data32[]` per channel), `in_events`/`out_events` | `process.h`, `audio-buffer.h`, `events.h` | `prepareAndProcessBlock`, scratch planar buffers `mScratchInput/Output`, `mIn/OutChannelPtrs` (`vst3_host.h:119-163`) | Planar deinterleave logic is identical, so **factor out a shared `PlanarScratch` helper** |
| Param + note events in one sorted `clap_input_events` (`CLAP_EVENT_PARAM_VALUE`, `NOTE_ON/OFF`, `MIDI`) | `events.h` | Separate `PreallocatedParameterChanges` + `PreallocatedEventList` + SPSC queues `mPendingParamQueue`/`mPendingMidiQueue` (`vst3_host.h:165-185`, `vst3_param_changes.h`) | **Reuse `LockFreeSpscQueue`**; new preallocated merged event list (or `clap::helpers::EventList`) |
| `params` ext: `count/get_info/get_value/value_to_text/text_to_value/flush`; values are **plain (not normalized)** with min/max; cookies; `host_params.rescan/clear/request_flush` | `ext/params.h:14-378` | `getParameterInfo`, `get/setParameterNormalized`, `mapSlotParameter` (`vst3_host.h:85-91`); Dart `Vst3ParamInfo` | New. Dart API must expose min/max/default plain values. Slot-param mapping must convert the slot's 0..1 to plain |
| Plugin→host param edits from GUI arrive as `CLAP_EVENT_PARAM_VALUE` (+ `PARAM_GESTURE_BEGIN/END`) in **out_events** of process/flush | `ext/params.h:52-63` | `Vst3ComponentHandler::performEdit` → `mParamEditCallback` (`vst3_component_handler.h`, `vst3_host.h:248-250`) → Dart `Vst3ParamEditEvent` stream | Output path differs (drain out_events on audio thread → SPSC → main), Dart stream shape reusable |
| `state` ext `save/load(clap_ostream/istream)`, `host_state.mark_dirty` | `ext/state.h:9-40` | `getState/setState` (`vst3_host.h:100-101`); Dart `saveState/restoreState` (`vst3_host_ffi.dart:470-525`) | Trivial adapter; same Dart bytes API |
| `latency` ext `get()`; `host_latency.changed()` only while activating → requires `request_restart` | `ext/latency.h` | `getLatencySamples` (`vst3_host.h:67`); restart via `restartComponent` → `dartVst3RestartComponentCallback` | Map `request_restart` onto the same "restart" Dart event |
| `gui` ext: `is_api_supported(api, floating)`, `create`, `set_scale`, `get_size`, `set_parent` (embedded) or `set_transient` (floating), `show/hide`, `host_gui.request_resize/closed` — APIs `cocoa` (NSView*), `win32` (HWND), `x11` (Window), `wayland` | `ext/gui.h:20-210` | `Vst3EditorWindow` (IPlugFrame) w/ NSWindow/HWND/X11 impls (`src/vst3/vst3_editor_window.h:30-158`, `vst3_editor_macos.mm`, `_windows.cpp`, `_linux.cpp`) | **Shareable**: window creation, resize, close, main-queue dispatch. Split a format-agnostic `PluginHostWindow` (owns NSWindow/HWND/X11 win, exposes native parent handle + resize/close callbacks) from VST3's `IPlugFrame`. Floating mode = no host window at all (easy fallback) |
| `thread-check` (host provides `is_main_thread`/`is_audio_thread`) | `ext/thread-check.h` | none | New; tiny |
| `timer-support` (`register_timer`) and Linux `posix-fd-support` (`register_fd`) | `ext/timer-support.h:22-26`, `ext/posix-fd-support.h:36-44` | Linux `IRunLoop` impl in `vst3_editor_linux.cpp:260,749-816`; Win `SetTimer` `vst3_editor_windows.cpp:244` | **Same concept as VST3 IRunLoop**, so reuse the run-loop plumbing |
| `request_callback` → host calls `plugin->on_main_thread` "within ~33 ms" | `host.h:50-62`, `plugin.h:103-109` | none (VST3 has no equivalent) | **New**, see §3 |
| `audio-ports`, `note-ports` (channel/port negotiation, MIDI vs CLAP note dialect) | `ext/audio-ports.h`, `ext/note-ports.h` | Implicit bus setup in VST3 `initialize` (`mHasAudioInput`, `mMaxChannels`) | New, required for correct instrument and sidechain handling |
| Out-of-process editor helper + shm (`vst3_editor_shm.h`, `vst3_editor_helper_*.{mm,cpp}`) | — | Exists only for VST3 bundles that embed their own `FlutterMacOS.framework` (`supportsRemoteEditor`, `vst3_editor_window.h:38-48`) | **Not needed for CLAP** v1. Could reuse later if a Flutter-built CLAP plugin conflicts |
| Engine binding: effect → `CustomFilterSlot.setNativeCallback` with `soloud_*GetFilterCallbackPtr()` + userdata; instrument → `createCustomAudioSource(singleInstance: true)` | — | `vst3_host_ffi.dart:197-229` (effect), `:236-270` (instrument); `custom_filter.dart:270` `setNativeCallback`; `bindings.cpp:4579-4596` `createCustomAudioSource`, `:4599` `addCustomFilter`, `:4698` `setCustomFilterCallback`; C callbacks `vst3FilterDspCallback`/`vst3SourceDspCallback` (`vst3_host.h:288-300`) | **Fully reusable**, no engine changes. Add `clapFilterDspCallback`/`clapSourceDspCallback` with the same signatures |
| Handle table w/ generation (`kSlotBits`), audio-thread barrier on unload, `unloadAll` on dispose | — | `Vst3HostManager` (`vst3_host.h:199-284`); dispose hook `bindings.cpp:2167-2171` | Copy the pattern (or template it). Add a `ClapHostManager::unloadAll()` to the same dispose block |
| FFI surface `soloud_vst3*` + stubs when disabled | — | `bindings.cpp:5455-5976` | Mirror as `soloud_clap*` with `#else` stubs so Dart can always bind |

## 3. Threading and real-time concerns specific to CLAP

- **Strict thread tags.** The spec marks every call `[main-thread]`, `[audio-thread]`, `[thread-safe]` or `[being-activated]`. **[V]** Examples:
  - `gui.h` and `params.h` count/get_info/get_value are main-thread (`params.h:260-289`).
  - `activate` is main-thread.
  - `start_processing`, `process` and `stop_processing` are audio-thread.
  Plugins (u-he, Surge) assert on these rules, and `clap-helpers` `plugin-proxy` and `misbehaviour-handler` enforce them in debug builds. **[V]** (helpers file list).
- **"Main thread" must be the real OS main thread** (cocoa GUI needs it). On macOS the existing VST3 editor already hops to the main queue with `dispatch_sync` when off-main (`vst3_editor_macos.mm:540-543,752-755,831-834,907-926`). A CLAP host must apply that same hop to *all* `[main-thread]` plugin calls (load, activate, params, state), not just GUI calls. Otherwise calls from a Dart background isolate break the rule.
  - **[E/unverified]** Recent Flutter versions run the root isolate on the platform thread on macOS and Windows (the merged UI/platform thread). If that holds for the Flutter version GPhil pins, Dart FFI calls are already on main. The guard is still required for safety.
- **`request_callback` pumping.** This is `[thread-safe]`, can be called from the audio thread, and expects `on_main_thread` "usually within 33 ms" (`host.h:50-62`). **[V]** Implementation:
  1. Set an atomic flag.
  2. Post to the main loop. On macOS use `dispatch_async(main_queue)` (it's safe because the Flutter app runs a CFRunLoop). On Windows post a message to a hidden HWND or use a `SetTimer`. On Linux use a GLib idle source (the GTK main loop Flutter Linux uses, matching `vst3_editor_linux.cpp`).
  3. Don't route it through Dart.
  - `dispatch_async` is not real-time safe from the audio thread, so the audio thread should only set the atomic. A main-thread timer of about 16–30 ms, shared with `timer-support`, then services the flags. **[E]**
- **`request_restart`** means deactivate then reactivate on main, while the audio thread must not be in `process`. Reuse the existing audio-thread barrier (`Vst3HostManager::setAudioThreadBarrier`, `vst3_host.h:229-230`) and the `mIsProcessing` atomic pattern.
- **`request_process`** and `CLAP_PROCESS_SLEEP`/`CONTINUE_IF_NOT_QUIET` return codes: the host may skip process calls on sleep. Simplest choice: keep calling `process` (SoLoud pulls anyway) and honour only the tail. **[E]**
- **`params.flush`.** When the plugin isn't processing (voice paused or filter inactive), param changes from the UI must go through `flush()` on main or audio thread, not `process`. VST3 has no equivalent, so this is new logic.
- **Real-time allocation.** The event lists must be preallocated (the VST3 code already does this with `PreallocatedEventList`, 1024 SPSC). The audio-thread `process` must not lock `mProcessMutex` in the hot path. The VST3 code uses `std::mutex mProcessMutex`, so check how it's used before copying.

## 4. Proposed Dart API

Pick one of two options:

- **(A) Minimal, mirrors VST3** **[E] (recommended first)**:
  - `ClapHost` with `isSupported`, `standardSearchPaths` (with `CLAP_PATH`), `scanBundle(path) → List<ClapPluginInfo>` (id, name, vendor, version, features, `isInstrument`, `isEffect`), `loadEffect({bundlePath, slot, pluginId, sampleRate, maxBlockSize, channels})`, `loadInstrument(...)` and `unloadAll()`.
  - Plugin classes: `ClapPluginBase`, `ClapEffectPlugin`, `ClapInstrumentPlugin`, with the same methods as `Vst3PluginBase` (`vst3_host_ffi.dart:313-575`), `Vst3EffectPlugin.mapToSlotParameter` (`:581-635`) and `Vst3InstrumentPlugin.noteOn/noteOff` (`:640-700`).
  - Params expose `minValue/maxValue/defaultValue` (plain) plus helpers for normalized values.
- **(B) Format-agnostic** later: `abstract interface class AudioPluginHost { PluginFormat format; ... }` and `AudioPluginInstance`, with `Vst3Host` and `ClapHost` implementing it.
  - GPhil's plugin-chain UI would then deal with one type, and state blobs would be tagged `{format, id, bytes}`.
  - Do (A) first and extract (B) when there's a second consumer. Renaming `Vst3PluginBase` now would churn the upstream PR surface.

Other pieces:
- Hook flag: `hooks: user_defines: flutter_soloud: enable_clap: macos` (bool or platform list, identical semantics to `enable_vst3`), with env fallback `SOLOUD_ENABLE_CLAP`.
- The web stub mirrors `vst3_host_web.dart` (`isSupported == false`).

## 5. Effort estimate **[E]** (one experienced dev who already knows this codebase)

| Work item | macOS only | + Win + Linux |
|---|---|---|
| Hook flag, vendored headers, source collection, FFI stubs | 1–2 d | +1 d |
| Native core: loader/entry/factory, scan, instance state machine, handle manager, barrier/unload, process with planar buffers, merged event lists, note-ports/audio-ports negotiation | 5–8 d | +2 d (LoadLibrary/dlopen, path rules) |
| Host extensions: thread-check, log, latency, state, params (+flush, rescan, gesture out-events), request_callback/restart/process pump, timer-support | 4–6 d | +2–3 d (posix-fd + timers on Linux) |
| GUI: refactor `Vst3EditorWindow` into a shared window + CLAP `set_parent`/`request_resize`/`set_scale`; floating fallback | 3–5 d | +4–6 d (HWND, X11 embedding) |
| Dart API (A), types, web stub, ffigen | 2–3 d | — |
| Tests: Dart unit tests with stubs, a native test using a tiny MIT test plugin (e.g. build `clap-plugins` or a minimal gain plugin in-tree), integration probe like `integration_test/vst3_master_chain_probe_test.dart` | 3–4 d | +2 d CI |
| Example app page + docs | 1–2 d | — |
| **Total** | **≈ 19–30 working days (4–6 wks)** | **≈ 30–45 days (6–9 wks)** |

Using `clap-helpers` `host.hh`/`plugin-proxy.hh` probably saves 3–5 days on the extension plumbing and misbehaviour checks. It adds templated C++ that's a bit heavier to read.

### Risks

- **Plugin availability.** None are installed locally. **[V]** Verified CLAP vendors:
  - u-he (Uhbik, Zebralette, Tyrell N6, Presswerk on Mac/Win/Linux, listed at https://clapdb.tech/).
  - FabFilter (Pro-Q 4 listed on clapdb.tech).
  - Surge Synth Team (OB-Xf on clapdb.tech; Surge XT ships CLAP per its project, not re-verified here).
  - Others reported: Martinic, A.O.M., TAL, Auburn Sounds (press: KVR / Sound On Sound / Auburn blog).
  - **Not found:** TDR, Pianoteq/Modartt, Audio Modeling SWAM, NI Kontakt, iZotope, VSL/VEP. These are GPhil's actual plugins.
  - So CLAP adds no coverage for the current GPhil chain. **[V for local listing; vendor statuses = search, not exhaustive]**
- **macOS sandbox and library validation.** GPhil is sandboxed (`com.apple.security.app-sandbox`). The working-tree `macos/Runner/DebugProfile.entitlements` diff (uncommitted) adds `cs.disable-library-validation` and read-only temporary-exception paths for `/Library/Audio/Plug-Ins/VST3/`. **[V]**
  - CLAP needs the same: add `/Library/Audio/Plug-Ins/CLAP/` and the home-relative `Library/Audio/Plug-Ins/CLAP/` exceptions.
  - Note: the home-relative entry in the diff reads `/Library/Audio/Plug-Ins/VST3/` (leading slash). Check the format Apple expects for home-relative paths.
  - Temporary exceptions are generally rejected for Mac App Store builds, so plugin hosting would stay debug/direct-distribution only. **[E]**
- **Thread-rule violations crash or assert in some plugins** if main-thread calls arrive from a Dart background isolate. Mitigate with a mandatory main-thread hop.
- **GUI embedding into a Flutter-owned process.** Some plugins prefer floating windows. Linux X11 embedding under a GTK/Wayland Flutter shell is the weakest area (the same as VST3 today).
- **Upstreamability.** It adds a second large opt-in subsystem to alnitak/flutter_soloud. Ask the maintainer before investing, since there's no CLAP ask upstream. **[V: 0 issues]**

## 6. Upstream status and reuse

- alnitak/flutter_soloud: search `repo:alnitak/flutter_soloud CLAP` returns **0** issues/PRs. VST3 matches are #593 (the feature PR), #600 and #612 (hardening), and #598 (DartNative heads-up). **[V]**
- Reusable permissive code **[V licences]**:
  - `free-audio/clap` (MIT, headers, required).
  - `free-audio/clap-helpers` (MIT, header-only C++ host helpers: `host.hh`, `plugin-proxy.hh`, `event-list.hh`, `param-queue.hh`).
  - `free-audio/clap-host` (MIT, Qt reference host, read only).
  - No standalone permissive "CLAP host library" without a GUI framework turned up. Hosts like Tracktion Engine and JUCE carry GPL/commercial terms or are plugin-side only. **[E, not exhaustively searched]**

## Sources

- https://github.com/free-audio/clap (MIT; `include/clap/entry.h`, `host.h`, `plugin.h`, `ext/gui.h`, `ext/params.h`, `ext/state.h`, `ext/latency.h`, `ext/thread-check.h`, `ext/timer-support.h`, `ext/posix-fd-support.h`; tag 1.2.10)
- https://github.com/free-audio/clap-helpers (MIT)
- https://github.com/free-audio/clap-host (MIT)
- https://clapdb.tech/ (recent CLAP plugin listings)
- https://www.kvraudio.com/news/a-o-m-updates-plugins-to-v1-15-3---clap-plugin-format-is-now-supported-55769
- https://www.soundonsound.com/news/martinic-introduce-clap-support
- https://www.auburnsounds.com/blog/CLAP-Support-And-Other-Goodies.html
- https://www.sweetwater.com/insync/clap-the-new-clever-audio-plug-in-format/
- https://github.com/alnitak/flutter_soloud/pull/593, /pull/600, /pull/612, /issues/598
