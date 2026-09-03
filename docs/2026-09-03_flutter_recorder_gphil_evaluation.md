# `flutter_recorder` 2.0.1 as the GPhil capture substrate

Date: 2026-09-03

## Decision

**Do not run published `flutter_recorder` beside `flutter_soloud` if GPhil's common capture/playback sample clock is non-negotiable. Use one duplex native device owner. Reuse or upstream `flutter_recorder`'s capture-specific components, but do not give a second plugin independent ownership of the input device.**

`flutter_recorder` now has the correct product boundary and much of the required plumbing: miniaudio input capture, selectable formats and devices, PCM streaming, file recording, route/interruption notifications, platform input presets, and an explicit integration path with `flutter_soloud`. Its 2.0.1 API even describes feeding `flutter_soloud` mixer output into recorder-side AEC, which is strong upstream evidence that playback and input capture are intended to remain separate packages ([2.0.1 release](https://github.com/alnitak/flutter_recorder/releases/tag/v2.0.1), [recorder/SoLoud integration API](https://github.com/alnitak/flutter_recorder/blob/49fcf73f699f6e06451ecb782ad18e81c6787623/lib/src/flutter_recorder.dart#L516-L631)).

However, GPhil's recorder is a timeline source, not merely a file or byte stream. Published 2.0.1 exposes no capture sample position, monotonic timestamp, discontinuity/overflow count, effective negotiated format, asynchronous writer result, or capture/playback start relationship. Those are required to place a take against GPhil's composition clock and to distinguish a valid take from a damaged one. The right next step is therefore a single-duplex-device spike, reusing recorder-specific implementation where useful—not a direct dependency swap.

### Common-clock constraint

If GPhil requires capture and playback to advance on one sample timeline, two independently opened devices—one in `flutter_soloud` and one in `flutter_recorder`—do not satisfy that requirement. Giving both callbacks timestamps from `steady_clock` only correlates two device clocks; it does not eliminate their start uncertainty or long-run drift.

The current local SoLoud capture extension also does **not yet** provide a true shared sample clock. It opens a capture-only `ma_device` separately from SoLoud's playback-only `ma_device`, even where both use the same miniaudio context ([capture device](../src/capture/capture_session.cpp#L211-L284), [playback device](../src/soloud/src/backend/miniaudio/soloud_miniaudio.cpp#L776-L798)). Its coordinated-start API starts capture, unpauses a SoLoud voice, and records the host time after that call; it does not observe the first rendered output frame ([coordinated start](../src/capture/capture_session.cpp#L294-L343)). A shared context and common host-time units are useful, but they are not a shared audio clock.

For a strict common clock, one native owner must open a miniaudio **duplex** device and handle input plus output in the same callback/frame progression. The callback should render SoLoud into `pOutput`, enqueue `pInput` to the recorder writer, and publish the exact frame origin plus input/output latency. There are two viable ownership shapes:

1. Keep the duplex adapter in the `flutter_soloud` fork because SoLoud already owns the output device. Isolate capture/writer code in focused modules so the public playback package does not accumulate unrelated recording policy.
2. Preferably for long-term separation, create a small GPhil/native audio-I/O host that owns the duplex device, runs SoLoud as a device-less/manual-render mixer, and delegates captured frames to recorder modules. `flutter_soloud` remains the playback engine and `flutter_recorder` can supply reusable capture/writer components, but neither independently opens an audio device during a GPhil session.

Making published `flutter_recorder` independently open capture while SoLoud independently opens playback is appropriate only if GPhil accepts clock correlation plus measured drift correction instead of a genuinely shared sample clock.

## Release evaluated

- pub.dev currently publishes **2.0.1**, released on 2026-09-03 ([package page](https://pub.dev/packages/flutter_recorder), [changelog](https://pub.dev/packages/flutter_recorder/changelog)).
- Tag `v2.0.1` resolves to commit [`49fcf73f699f6e06451ecb782ad18e81c6787623`](https://github.com/alnitak/flutter_recorder/tree/49fcf73f699f6e06451ecb782ad18e81c6787623).
- It requires Dart 3.11, Flutter 3.41, iOS 15, and macOS 12, and uses Dart native build hooks ([pubspec](https://github.com/alnitak/flutter_recorder/blob/49fcf73f699f6e06451ecb782ad18e81c6787623/pubspec.yaml#L1-L40), [2.0.1 changelog](https://github.com/alnitak/flutter_recorder/blob/49fcf73f699f6e06451ecb782ad18e81c6787623/CHANGELOG.md#L1-L18)).
- The package is Apache-2.0, while bundled miniaudio, SpeexDSP, Opus, and Ogg have permissive licenses documented by upstream ([package license](https://github.com/alnitak/flutter_recorder/blob/49fcf73f699f6e06451ecb782ad18e81c6787623/LICENSE), [third-party licenses](https://github.com/alnitak/flutter_recorder/blob/49fcf73f699f6e06451ecb782ad18e81c6787623/README.md#third-party-libraries--licenses)). A maintained fork and upstream contributions are legally viable, subject to preserving notices.

This review used the published API, tagged source, release/changelog, repository tests and CI, and upstream issue tracker. Local verification of the clean tag ran `flutter test`: 51 tests passed. That is useful implementation evidence but not device qualification.

## Capability comparison

| Contract | `flutter_recorder` 2.0.1 | GPhil need / consequence |
| --- | --- | --- |
| Platform coverage | Declares Android, iOS, Linux, macOS, Windows, and web. The README still labels macOS and iOS "under test" ([pubspec](https://github.com/alnitak/flutter_recorder/blob/49fcf73f699f6e06451ecb782ad18e81c6787623/pubspec.yaml#L10-L16), [README](https://github.com/alnitak/flutter_recorder/blob/49fcf73f699f6e06451ecb782ad18e81c6787623/README.md#L1-L20)). | Broad enough as a base, but GPhil must qualify its actual macOS/iOS/Android targets and routes. |
| Input capture | A capture-specific miniaudio device; singleton in Dart and native code ([public singleton contract](https://github.com/alnitak/flutter_recorder/blob/49fcf73f699f6e06451ecb782ad18e81c6787623/lib/src/flutter_recorder.dart#L21-L68), [native global](https://github.com/alnitak/flutter_recorder/blob/49fcf73f699f6e06451ecb782ad18e81c6787623/src/flutter_recorder.cpp#L26-L35)). | One active GPhil input session is compatible. Wrap the singleton behind a GPhil service so global access does not leak into UI/domain code. |
| PCM configuration | Requested sample rate; mono/stereo; unsigned 8-bit, signed 16/24/32-bit, or float32 ([init API](https://github.com/alnitak/flutter_recorder/blob/49fcf73f699f6e06451ecb782ad18e81c6787623/lib/src/flutter_recorder.dart#L218-L287), [format enums](https://github.com/alnitak/flutter_recorder/blob/49fcf73f699f6e06451ecb782ad18e81c6787623/lib/src/enums.dart#L72-L123)). | Covers GPhil's 48 kHz stereo PCM request, but the API returns no effective/negotiated native rate, channels, or period. Add those to the start result. |
| Live PCM | `uint8ListStream` emits `AudioDataContainer`, and native code coalesces PCM into fixed 2,048-frame packets. At 48 kHz that is about 42.7 ms before Dart scheduling ([stream API](https://github.com/alnitak/flutter_recorder/blob/49fcf73f699f6e06451ecb782ad18e81c6787623/lib/src/flutter_recorder.dart#L131-L143), [native buffering](https://github.com/alnitak/flutter_recorder/blob/49fcf73f699f6e06451ecb782ad18e81c6787623/src/capture.cpp#L22-L35), [delivery](https://github.com/alnitak/flutter_recorder/blob/49fcf73f699f6e06451ecb782ad18e81c6787623/src/capture.cpp#L283-L320)). | Fine for meters/waveform previews; unsuitable as the sole irreplaceable recording path or a low-latency timing seam. Keep durable native writing off Dart delivery. Make packet size configurable if live use needs it. |
| Stream metadata | The container holds only bytes and length—no frame index, timestamp, route, generation, or discontinuity ([container](https://github.com/alnitak/flutter_recorder/blob/49fcf73f699f6e06451ecb782ad18e81c6787623/lib/src/audio_data_container.dart#L6-L32)). | Blocking gap. Add capture-generation ID, first-frame index, monotonic host time, frame count, effective format, and discontinuity flags/counters. |
| Stream ownership | Native/Dart currently copies the incoming native block before publishing, although public documentation warns consumers that streamed memory is reused and must be copied immediately ([FFI callback](https://github.com/alnitak/flutter_recorder/blob/49fcf73f699f6e06451ecb782ad18e81c6787623/lib/src/bindings/recorder_io.dart#L69-L80), [public warning](https://github.com/alnitak/flutter_recorder/blob/49fcf73f699f6e06451ecb782ad18e81c6787623/lib/src/flutter_recorder.dart#L131-L143)). | Clarify and test an immutable packet ownership contract before GPhil relies on buffering across isolates. |
| File recording | Native WAV or Ogg Opus; pause is supported ([recording API](https://github.com/alnitak/flutter_recorder/blob/49fcf73f699f6e06451ecb782ad18e81c6787623/lib/src/flutter_recorder.dart#L366-L413), [native write path](https://github.com/alnitak/flutter_recorder/blob/49fcf73f699f6e06451ecb782ad18e81c6787623/src/capture.cpp#L323-L350)). | WAV is a useful safety format; Opus is lossy and not the archival/editing master. GPhil's optional lossless FLAC/WavPack mirror is absent. |
| Writer safety | WAV/Opus writes are called directly from the audio callback; their return values are not surfaced there. `stopRecording()` is void and returns no frame count, flush/finalization result, overflow, or failure details ([callback writes](https://github.com/alnitak/flutter_recorder/blob/49fcf73f699f6e06451ecb782ad18e81c6787623/src/capture.cpp#L323-L350), [writer lifecycle](https://github.com/alnitak/flutter_recorder/blob/49fcf73f699f6e06451ecb782ad18e81c6787623/src/capture.cpp#L677-L709)). | Blocking gap for irreplaceable takes. Move file/codec work behind a bounded native ring and writer thread; define overflow policy; await finalization; return frame counts and errors. |
| Device selection/routes | Desktop-style enumeration and selected device ID are public. 2.0.1 adds started/stopped/rerouted/interruption/unlocked events ([device/init API](https://github.com/alnitak/flutter_recorder/blob/49fcf73f699f6e06451ecb782ad18e81c6787623/lib/src/flutter_recorder.dart#L125-L129), [device enumeration](https://github.com/alnitak/flutter_recorder/blob/49fcf73f699f6e06451ecb782ad18e81c6787623/lib/src/flutter_recorder.dart#L210-L287)). | Promising, but notifications carry only an enum—no timestamp, old/new device identity, format change, restart success, or loss interval. Open issue [#29](https://github.com/alnitak/flutter_recorder/issues/29) reports Android enumeration returning only the default instead of an attached USB input. |
| Platform processing | Android, iOS, and web expose input presets; passing no iOS preset leaves the active audio session to the app ([preset API](https://github.com/alnitak/flutter_recorder/blob/49fcf73f699f6e06451ecb782ad18e81c6787623/lib/src/flutter_recorder.dart#L218-L255), [Apple implementation](https://github.com/alnitak/flutter_recorder/blob/49fcf73f699f6e06451ecb782ad18e81c6787623/src/flutter_recorder_miniaudio_objc.mm#L13-L59)). | Good boundary. GPhil should retain audio-session ownership and explicitly choose unprocessed/music capture policy; do not silently accept voice DSP. |
| Simultaneous playback/recording | Separate capture works alongside external playback. The package supports native mic loopback and accepts external playback PCM as the AEC reference ([loopback/AEC API](https://github.com/alnitak/flutter_recorder/blob/49fcf73f699f6e06451ecb782ad18e81c6787623/lib/src/flutter_recorder.dart#L516-L631)). The capture callback records the processed microphone, not a mix of playback and microphone ([capture callback](https://github.com/alnitak/flutter_recorder/blob/49fcf73f699f6e06451ecb782ad18e81c6787623/src/capture.cpp#L228-L350)). | Correct separation for GPhil: accompaniment remains in `flutter_soloud`; the user-audio take remains in recorder. Composition/mix decisions remain in GPhil Core. |
| AEC synchronization | `feedPlaybackData` supplies PCM format/channels but no playback sample rate, timestamp, or frame position ([public feed API](https://github.com/alnitak/flutter_recorder/blob/49fcf73f699f6e06451ecb782ad18e81c6787623/lib/src/flutter_recorder.dart#L597-L631)). The native implementation is FIFO arrival-order alignment with silence when reference data is unavailable ([AEC source](https://github.com/alnitak/flutter_recorder/blob/49fcf73f699f6e06451ecb782ad18e81c6787623/src/filters/echo_cancellation.cpp#L221-L315)). | Useful for experiments, not yet a deterministic GPhil alignment contract. Add sample rate and clock/frame metadata; characterize delay and drift. AEC is optional and should not define take timing. |
| Timing/sample clocks | No public timestamp or captured-frame clock exists. `start()`, `startRecording()`, and `stopRecording()` are synchronous/void after initialization ([lifecycle API](https://github.com/alnitak/flutter_recorder/blob/49fcf73f699f6e06451ecb782ad18e81c6787623/lib/src/flutter_recorder.dart#L316-L413)). | Primary blocking gap. GPhil currently depends on monotonic session/capture/first-buffer/playback/stop timestamps and input-frame positions ([current GPhil capture result models](../lib/src/capture/soloud_capture.dart#L26-L328)). |
| Capture/playback coordinated start | No API starts capture and a `flutter_soloud` voice under one operation, nor maps recorder time to SoLoud engine time. | Replace the current coupling with a neutral coordination contract: recorder exposes capture clock anchors; playback exposes scheduled-start/actual-start anchors; a GPhil integration layer joins them. Do not make `flutter_recorder` depend on `flutter_soloud`. |
| Levels/visualization | Volume, waveform, and FFT are available for float32 capture; 2.0.1 adds streamed multichannel visualization ([README](https://github.com/alnitak/flutter_recorder/blob/49fcf73f699f6e06451ecb782ad18e81c6787623/README.md#real-time-audio-visualization-waveform--fft)). | Suitable for GPhil UI after device qualification. Current GPhil peak/RMS/peak-since-read/held-peak/frame-count snapshot is richer and clock-associated ([current model](../lib/src/capture/soloud_capture.dart#L304-L328)); either retain that contract or extend visualization packets. |
| Interruption/background | Native miniaudio notification types are forwarded, including interruption begin/end and reroute ([notification callback](https://github.com/alnitak/flutter_recorder/blob/49fcf73f699f6e06451ecb782ad18e81c6787623/src/capture.cpp#L216-L223)). No documented durable background-recording policy or automatic take recovery contract was found. | Treat interruptions as explicit state transitions in GPhil. Add exact discontinuity ranges and recovery/finalization behavior. Background capture must be separately designed and qualified per platform. |
| Lifecycle/errors | Initialization/start throw typed exceptions; many later operations are void/no-op. `deinit()` stops recording/stream/device and closes callbacks ([Dart lifecycle](https://github.com/alnitak/flutter_recorder/blob/49fcf73f699f6e06451ecb782ad18e81c6787623/lib/src/flutter_recorder.dart#L247-L413), [native teardown](https://github.com/alnitak/flutter_recorder/blob/49fcf73f699f6e06451ecb782ad18e81c6787623/src/flutter_recorder.cpp#L415-L487)). | Start is usable; stop/finalize/error reporting is insufficient. Open issues [#26](https://github.com/alnitak/flutter_recorder/issues/26) (web mic release) and [#30](https://github.com/alnitak/flutter_recorder/issues/30) (stream controller lifecycle) argue for targeted lifecycle tests. |

## Why this is preferable to capture inside `flutter_soloud`

The libraries still permit a clean responsibility split even though a common-clock GPhil session needs one device owner:

1. `flutter_soloud` owns playback voices, buses, scheduling, and rendered mixer output.
2. Recorder modules own input DSP, durable writing, codecs, and input telemetry, but not a second live device during a common-clock session.
3. A thin GPhil audio-I/O adapter owns the duplex device and dispatches the same callback's input and output frames to those two engines.
4. GPhil Core owns take lifecycle, project storage policy, recovery, placement on the composition timeline, and mixing the recorded take with orchestral material.

Keeping that responsibility split avoids making the SoLoud engine responsible for capture codecs and writer policy. The device adapter must nevertheless coordinate enumeration, permissions, route changes, and interruption recovery because those affect one duplex stream. Recorder improvements can still be reusable independently outside the GPhil common-clock mode.

GPhil's existing `startCaptureAndPlay` coupling returns capture-start and playback-start host timestamps in one result ([current model](../lib/src/capture/soloud_capture.dart#L85-L159)). That behavior cannot simply disappear, but host timestamps alone are weaker than the intended guarantee. The duplex adapter should instead define frame zero in the shared callback and report the input/output frame positions and route latencies associated with that origin.

## Minimum extension required before migration

Prefer upstreamable, recorder-generic primitives. Keep GPhil project/domain concepts out of the package.

1. **Versioned capture session/result API**
   - Session/generation ID.
   - Effective sample rate, channels, PCM format, period/buffer size, selected device identity, and route.
   - Monotonic host timestamp after device start.
   - First-input-buffer timestamp and first frame index.
   - Current `(hostTime, inputFramePosition)` snapshot.

2. **Loss-aware native writer**
   - Audio callback only copies into a preallocated bounded ring.
   - Writer/encoder work runs off the real-time callback.
   - Defined overflow policy; GPhil's current policy accounts dropped frames and inserts equivalent silence to preserve duration ([current stop diagnostics](../lib/src/capture/soloud_capture.dart#L161-L233)).
   - `Future<CaptureStopResult>` waits for flush/finalization and reports captured/written/silence/overflow frames, writer failure, file path, and finalization success.
   - Retain a WAV safety master; add a lossless mirror only if GPhil still requires FLAC/WavPack.

3. **Timestamped immutable live packets**
   - Bytes plus first-frame index, frame count, monotonic timestamp, generation, effective format, and discontinuity information.
   - Explicit immutable ownership across Dart listeners/isolate boundaries.
   - Configurable notification size separate from native device period.

4. **Route/interruption state**
   - Timestamped old/new device and effective-format details.
   - Whether capture stopped, restarted, or continued.
   - Exact lost/inserted-frame range after interruption or reroute.

5. **One duplex frame timeline without package coupling**
   - A single native audio-I/O adapter owns the duplex device and increments one callback-frame counter.
   - It renders SoLoud into output and enqueues captured input using that same frame range.
   - It reports the shared frame origin plus effective input/output device latency; common clock does not mean zero acoustic latency.
   - `flutter_recorder` must not independently open capture during this mode. Reuse its recorder components behind the adapter or refactor them into a device-agnostic capture sink.

6. **Real-time safety audit**
   - Remove file I/O, growing-vector operations, and avoidable allocation from the capture callback.
   - Audit the AEC FIFO path before enabling it in production; its current implementation uses mutex-protected vectors and front erasure in the processing path ([AEC implementation](https://github.com/alnitak/flutter_recorder/blob/49fcf73f699f6e06451ecb782ad18e81c6787623/src/filters/echo_cancellation.cpp#L221-L315)).

## Maintenance and adoption risk

The repository is active and 2.0.1 is a substantial release, but it is still a small project with one declared maintainer ([pubspec](https://github.com/alnitak/flutter_recorder/blob/49fcf73f699f6e06451ecb782ad18e81c6787623/pubspec.yaml#L1-L10)). The checked-in CI workflow runs dependency resolution, analyzer, formatting, and publish dry-run, but not `flutter test`, native tests, or device integration tests ([CI workflow](https://github.com/alnitak/flutter_recorder/blob/49fcf73f699f6e06451ecb782ad18e81c6787623/.github/workflows/analyze.yaml#L16-L43)). Therefore:

- propose generic timing/writer/lifecycle extensions upstream in small PRs;
- keep a pinned GPhil fork until those contracts are released and qualified;
- add callback-level C++ tests plus real macOS/iOS/Android device tests, not only source-shape/Dart tests;
- test 48 kHz stereo capture during simultaneous SoLoud playback, long takes, slow storage, interruption, route change, sleep/background transitions, hot restart, deinit, and storage exhaustion;
- validate USB device selection on Android because issue #29 remains open;
- do not enable silence-skipping for GPhil masters because removing silent frames changes timeline duration.

## Recommended migration sequence

1. Freeze the current GPhil capture behavior as public contract tests: start/stop/cancel, negotiated 48 kHz stereo, clocks, first buffer, levels, coordinated playback alignment, overflow/silence accounting, WAV safety file, optional lossless mirror, interruption, and teardown.
2. Spike a single miniaudio duplex device callback that renders SoLoud output and captures input with one frame counter. Prove the clock and latency contract on physical targets before moving package boundaries.
3. Refactor capture-specific writer, codec, telemetry, and lifecycle code behind a device-agnostic sink. Reuse or upstream those parts through `flutter_recorder` where its maintainer accepts the abstraction.
4. Put the duplex owner in a small GPhil audio-I/O adapter; as the lower-risk transitional option, keep it isolated inside the `flutter_soloud` fork because that backend already owns output.
5. Run the old separate-device path and new duplex path behind a narrow interface, measure start offset and long-take drift on actual devices, then switch.

## Bottom line

**For GPhil's common-clock requirement, independently extending and running `flutter_recorder` is not the right architecture.** The root requirement is one duplex native device callback. The pragmatic first implementation belongs alongside the existing SoLoud miniaudio backend, with capture responsibilities kept in focused modules; the cleaner eventual boundary is a small GPhil audio-I/O host with SoLoud as its render engine and recorder code as its capture sink. Adoption remains gated on a verified shared frame timeline, input/output latency reporting, loss-aware writing, durable finalization, and physical-device qualification.
