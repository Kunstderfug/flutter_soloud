# Fork maintenance

This repository is a fork of
[alnitak/flutter_soloud](https://github.com/alnitak/flutter_soloud)
(the `upstream` remote). GPhil uses it through a path dependency
(`/Volumes/DEV/gphil-flutter/pubspec.yaml`: `path: ../flutter_soloud`). This
page explains what the fork adds, where it touches upstream files, and how to
merge a new upstream release.

The aim: fork code lives in files that upstream does not have, so a merge only
conflicts on a few short, marked lines. When a fork assumption breaks, a test
fails with a message that names the cause.

## What the fork adds

| Area | Files the fork owns | Hooks in upstream files |
|---|---|---|
| Public Dart API (capture, idle stop, voice-group start, engine session) | `lib/src/fork/soloud_fork_api.dart` (a part of `soloud.dart`: the private mixin `_SoLoudForkApi`) | `lib/src/soloud.dart`: the `part` line, two lifecycle calls, and one `// fork: begin` ... `// fork: end` block of public forwarders. The forwarders keep the dartdoc pages and let mocktail mocks see the members. |
| Dart bindings | `lib/src/fork/bindings_player_fork.dart`, `bindings_ffi_fork.dart`, `bindings_player_web_fork.dart` (mixins, parts of the binding files) | `bindings_player.dart`, `bindings_player_ffi.dart`, `bindings_player_web.dart`: `part` lines and `with` clauses. `js_extension.dart`: the web binding of `scheduleVoiceGroupStartAt`. |
| Fork enums | `lib/src/fork/fork_enums.dart` (`VoiceGroupStartResult`, `ClapHostEventType`, `ClapNoteDialect`) | `lib/src/enums.dart`: one export line. |
| Input capture | `lib/src/capture/`, `lib/src/helpers/capture_device.dart`, `src/capture/`, `src/wavpack/` (vendored WavPack encoder) | Exports in `lib/flutter_soloud.dart`; the native exports (see below). |
| CLAP plugin hosting (desktop) | `lib/src/clap/` (its own API, not part of the mixin), `src/clap/`, `src/clap_bindings.cpp`, `src/plugin_ui/`, `src/plugin_audio_barrier.h`; declarations in `src/fork/fork_bindings.h` | `src/bindings.h` includes `fork/fork_bindings.h`; `hook/build.dart` (`enable_clap`). |
| Classical Leveler filter | `lib/src/filters/classical_leveler_filter.dart`, `src/filters/classical_leveler_*`, `src/filters/classical_k_weighting.h` | `FilterType.classicalLevelerFilter` in `lib/src/filters/filters.dart` and `ClassicalLevelerFilter` in `src/enums.h` (both after `customFilter`); `isValidFilterType` in `src/bindings.cpp`; `src/filters/filters.{h,cpp}`. |
| Native fork exports | `src/fork/` (`fork_bindings.h` is an ffigen entry point in `ffigen.yaml`) | `src/bindings.{h,cpp}`, `src/player.{h,cpp}`: marked `// fork:`. |
| Other native changes | none | `src/filters/filters.cpp`, `src/vst3/vst3_host.cpp`, `src/vst3/vst3_editor_macos.mm`, `src/device_lifecycle_test_hooks.h`. |
| Vendored SoLoud changes | `src/soloud/src/core/soloud_core_voicegroup_start.cpp` (voice-group start at one deadline) | `src/soloud/`: marked `// ###### flutter_soloud local patch (...) ######`. |
| Build | none | `hook/build.dart`, `src/CMakeLists.txt`, `web/compile_wasm.sh`, the test build helpers in `test/support/`. |
| Tests | `test/fork/` (Dart and native tests, `fork_registry.dart`, `fork_symbols.dart`, `all.sh`) | One spread line each in `test/common/api_before_init_test.dart`, `filters_api_test.dart` and `web_bindings_sync_test.dart`; the header list in `ffigen_bindings_sync_test.dart`; the leveler's index in `filter_type_test.dart`; one `#include` in `test/common/device_coordinator_test.cpp`. |
| Example app | `example/tests/idle_engine`, `example/tests/input_capture`, `example/integration_test/clap_wrapper_test.dart` | Microphone permissions in the example's platform files. |

### Finding every hook

```bash
git grep -nE "(//|#) fork:"                   # Dart, C++, shell and test hooks
git grep -n "flutter_soloud local patch" src/soloud   # vendored SoLoud changes
git diff --stat --diff-filter=M vX.Y.Z        # every upstream file the fork changes
```

A hook is one short line or a small marked block. When a merge conflicts on a
hook, keep upstream's change and put the hook line back.

## Merging an upstream release

Do the steps in order. Use a folder on `/Volumes/DEV` for temporary files
(`TMPDIR=/Volumes/DEV/agent-tmp/<task>`) on every build and test command.

1. **Turn on rerere once per clone**, so git remembers how you resolved a
   conflict and reuses it next time:

   ```bash
   git config rerere.enabled true
   git config rerere.autoupdate true
   ```

2. **Fetch and merge the release tag** (not `upstream/main`):

   ```bash
   git fetch upstream --tags
   git merge vX.Y.Z
   ```

   Resolve the conflicts. Most should be on `// fork:` lines. Check the
   "Known weak spots" below for the places that break without a conflict.

3. **Regenerate the FFI bindings:**

   ```bash
   dart run ffigen --config ffigen.yaml
   ```

4. **Rebuild the WASM builds:**

   ```bash
   cd web && source /Volumes/DEV/emsdk/emsdk_env.sh && ./compile_wasm.sh
   ```

   Never point `EM_CACHE` at an empty folder: Emscripten then rebuilds all
   its system libraries, which is slow and can change the output. Leave it
   unset (the emsdk's own cache) or reuse a filled cache. Commit the four
   files in `web/` in their own commit, `update wasm libs`.

5. **Regenerate the native test CMake file:**

   ```bash
   python3 test/support/gen_cmake.py
   ```

6. **Dart checks:**

   ```bash
   flutter analyze          # no new issues
   flutter test             # includes test/fork and the contract tests
   ```

7. **Native tests:**

   ```bash
   ./test/macos/all.sh      # or test/linux/all.sh: upstream's native tests
   ./test/fork/all.sh       # the fork's native tests
   ```

   `test/common/vst3_param_changes_test.sh` prints `SKIPPED` without a VST3
   prebuild. Set `VST3_PREBUILD_DIR` to the
   `.dart_tool/flutter_soloud/vst3/prebuild` folder of a checkout that has
   built with VST3 enabled (for example `/Volumes/DEV/flutter_soloud`).

8. **GPhil checks**, at `/Volumes/DEV/gphil-flutter` (it builds against
   `/Volumes/DEV/flutter_soloud`, so run them once the merge is in that
   checkout):

   ```bash
   flutter analyze
   flutter test
   ```

   See "GPhil compatibility" below for what usually breaks.

## Known weak spots

These are the places where an upstream change breaks the fork without a merge
conflict. Each has a test that fails, named in brackets.

- **Filter enum order.** The fork's `ClassicalLevelerFilter` comes after
  `CustomFilter`, in `src/enums.h` and in `FilterType`. If upstream adds a
  filter, the fork's value must stay last in both lists, with the same index.
  Upstream's `isValidFilterType` (`src/bindings.cpp`) must accept the fork's
  value: in 5.4.0 its new upper bound rejected it.
  [`test/fork/filter_type_contract_test.dart` activates every `FilterType` on
  the real engine and checks the native parameter count, so an index that
  points at another filter fails too.]
- **Native test source list.** The native test builds take the plugin's
  sources from one list, `test/support/plugin_sources.txt` (read by
  `test/support/engine_sources.sh`, `test/support/plugin_build.sh`,
  `test/support/native_build.ps1` and `test/docker/windows_native.sh`). The
  fork's `src/fork/` and `src/capture/` are one marked block at its end, and
  the test builds get `-DNO_WAVPACK_LIBS` from one marked line in
  `plugin_sources()` (plus `Get-PluginDefines` in `native_build.ps1` and the
  plugin library line in `windows_native.sh`). A new upstream script that
  lists sources by hand instead of calling `plugin_sources` misses them.
  `hook/build.dart`, `src/CMakeLists.txt` and `web/compile_wasm.sh` still
  list the sources themselves. [`test/common/plugin_sources_test.dart` fails
  when the list misses a source the hook builds; a native test that misses
  the define fails to link]
- **Registry tests.** `test/common/api_before_init_test.dart`,
  `filters_api_test.dart` and `web_bindings_sync_test.dart` check that every
  member, filter or export is listed. The fork's entries are in
  `test/fork/fork_registry.dart` and `test/fork/fork_symbols.dart`. If
  upstream rewrites one of these tests, put the `// fork:` spread line back.
  `api_before_init_test` reads `soloud.dart` as text, including the mixins in
  its part files; if upstream changes how `class SoLoud` is declared, update
  its parser. [the registry test reports the unlisted member]
- **FFI and web symbols.** Every fork export must be generated by ffigen and
  either bound on the web (`js_extension.dart`) or excluded with a reason.
  CLAP is not compiled for the web. [`ffigen_bindings_sync_test` and the
  "fork FFI symbols on the web" group of `web_bindings_sync_test`]
- **Test layout moves.** Upstream moved its tests into `test/common/` in
  5.4.0. The fork's tests are in `test/fork/` and their runners find the repo
  root from their own path. `test/common/device_coordinator_test.cpp`
  includes `../fork/device_domain_idle_stop_test.inc`; keep that line.
- **Native return types.** Upstream may change what an export returns (for
  example `getFilterParams` returns a `PlayerErrors` code, not a bool). The
  fork's native tests must follow. [`test/fork/all.sh`]
- **Vendored SoLoud.** The local patches in `src/soloud/` (render-ahead,
  device callbacks, mix checkpoints, voice groups) conflict whenever upstream
  updates SoLoud. Keep every marked patch.

## GPhil compatibility

- **Mocks.** GPhil mocks `SoLoud` with mocktail
  (for example `class _MockSoLoud extends Mock implements SoLoud`) and stubs fork members
  such as `scheduleVoiceGroupStartAt` and `requestAudioDeviceIdleStop`. Fork
  API members must stay instance members of `SoLoud` (not extensions), and
  `SoLoud` must stay implementable from another library.
  [`test/fork/soloud_implements_test.dart` does not compile otherwise]
- **Exception types.** Upstream can change which exception a call throws.
  In 5.4.0, calls before `init()` throw `SoLoudNotInitializedException`
  instead of `SoLoudBackendNotInitedException`; GPhil commit `d64036af`
  updated its tests for that. Read upstream's CHANGELOG ("Behaviour changes")
  and search GPhil for the old type names.
- **Behaviour.** GPhil's tests cover playback, recording and the master
  chain. Run them after every merge, even when the fork's own tests pass.
