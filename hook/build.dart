// Build hook for flutter_soloud.
//
// Compiles the SoLoud engine and the plugin sources in `src/` into
// `libflutter_soloud_plugin` for the target OS/architecture, linking the
// prebuilt Xiph libraries checked into this repo (see `xiph/`).
//
// This replaces the previous per-platform build plumbing (Android CMake,
// iOS/macOS CocoaPods script phases, SwiftPM unity build). Web is not covered
// by hooks; the emscripten build in `web/` is unchanged.
//
// Configuration (in the *root app* pubspec.yaml):
//
// hooks:
//   user_defines:
//     flutter_soloud:
//       no_xiph_libs: true         # build without Opus/Ogg/Vorbis/FLAC
//       use_system_xiph_libs: true # link system Xiph libs (apt, brew, pacman)

// ignore_for_file: avoid_print

import 'dart:io';

import 'package:code_assets/code_assets.dart';
import 'package:hooks/hooks.dart';
import 'package:native_toolchain_c/native_toolchain_c.dart';

import 'xiph.dart';

/// The asset id is `package:flutter_soloud/src/bindings.cpp`, matching the
/// `asset-id` in `ffigen.yaml` used by the generated `@Native` bindings.
const _assetName = 'src/bindings.cpp';
const _libName = 'flutter_soloud_plugin';

void main(List<String> args) async {
  await build(args, (input, output) async {
    if (!input.config.buildCodeAssets) return;

    final code = input.config.code;
    final os = code.targetOS;
    final arch = code.targetArchitecture;
    final isApple = os == OS.macOS || os == OS.iOS;

    // Xiph configuration:
    // - `no_xiph_libs: true` -> build without Xiph support
    // Platform-specific user defines:
    // - `<platform>_use_system_libs: true` -> link against system packages
    // - `<platform>_force_build_libs: true` -> compile from source via CMake
    // Default (both false): uses prebuilt libraries from GitHub release
    // or local cache (.dart_tool/flutter_soloud/xiph/prebuild/).
    bool? parseBool(Object? val) {
      if (val == null) return null;
      if (val is bool) return val;
      if (val is String) {
        if (val.toLowerCase() == 'true' || val == '1') return true;
        if (val.toLowerCase() == 'false' || val == '0') return false;
      }
      return null;
    }

    final noXiph = parseBool(input.userDefines['no_xiph_libs']) ?? false;

    final bool useSystemLibs;
    final bool forceBuildLibs;

    switch (os) {
      case OS.linux:
        useSystemLibs =
            parseBool(input.userDefines['linux_use_system_libs']) ??
            parseBool(input.userDefines['use_system_xiph_libs']) ??
            false;
        forceBuildLibs =
            parseBool(input.userDefines['linux_force_build_libs']) ?? false;
      case OS.macOS:
        useSystemLibs =
            parseBool(input.userDefines['macos_use_system_libs']) ?? false;
        forceBuildLibs =
            parseBool(input.userDefines['macos_force_build_libs']) ?? false;
      case OS.windows:
        useSystemLibs =
            parseBool(input.userDefines['windows_use_system_libs']) ?? false;
        forceBuildLibs =
            parseBool(input.userDefines['windows_force_build_libs']) ?? false;
      case OS.android:
        useSystemLibs = false;
        forceBuildLibs =
            parseBool(input.userDefines['android_force_build_libs']) ?? false;
      case OS.iOS:
        useSystemLibs = false;
        forceBuildLibs =
            parseBool(input.userDefines['ios_force_build_libs']) ?? false;
      default:
        useSystemLibs = false;
        forceBuildLibs = false;
    }

    if (useSystemLibs && forceBuildLibs) {
      throw ArgumentError(
        '[flutter_soloud] Conflicting options in pubspec.yaml: cannot set both '
        '`${os.name}_use_system_libs: true` and '
        '`${os.name}_force_build_libs: true`.',
      );
    }

    final XiphLink xiph;
    final String xiphMode;
    if (noXiph) {
      xiphMode = 'disabled';
      xiph = XiphLink.empty();
    } else if (useSystemLibs) {
      xiphMode = 'from system';
      xiph = await XiphLink.forSystem(input);
    } else if (forceBuildLibs) {
      xiphMode = 'built from git';
      xiph = await XiphLink.fromSource(input);
    } else {
      xiphMode = 'prebuilt';
      xiph = await XiphLink.forPrebuild(input);
    }

    final platformDisplayName = switch (os) {
      OS.macOS => 'macOS',
      OS.iOS => 'iOS',
      OS.linux => 'Linux',
      OS.windows => 'Windows',
      OS.android => 'Android',
      OS.fuchsia => 'Fuchsia',
      _ => os.name,
    };

    print(
      '[flutter_soloud] Building on $platformDisplayName ($arch) with Xiph '
      'libs [$xiphMode]',
    );

    final defines = <String, String?>{
      'FLUTTER_PLUGIN_IMPL': null,
      'SIGNALSMITH_USE_PFFFT': null,
      'WITH_MINIAUDIO': null,
      'WITH_NULL': null,
      // Force NDEBUG across all compilers (comment out for native debugging).
      'NDEBUG': null,
      if (os == OS.windows) ...{
        'NOMINMAX': null,
        '_CRT_SECURE_NO_WARNINGS': null,
      },
      if (noXiph) 'NO_XIPH_LIBS': null,
    };

    final flags = <String>[
      if (os != OS.windows) ...[
        '-fvisibility=hidden',
        '-Wno-unused-command-line-argument',
      ],
      // Force maximum optimization regardless of Flutter build mode.
      // For native debugging: comment out 'NDEBUG' above and replace the line
      // below with: if (os == OS.windows) ...['/Od', '/Zi', '/EHsc'] else ...['-O0', '-g'],
      if (os == OS.windows) ...['/Ox', '/EHsc'] else '-O3',
      if (os == OS.android) ...[
        '-Wl,--build-id=sha1',
        '-ffast-math',
        '-funroll-loops',
        '-fomit-frame-pointer',
        '-ffunction-sections',
        '-fdata-sections',
        '-Wl,--gc-sections',
        // Support Android 15 16k page size.
        '-Wl,-z,max-page-size=16384',
        // arm64: NEON is mandatory — no extra flags needed. On armeabi-v7a
        // enable NEON explicitly (hard-float is the default on modern NDKs;
        // -mfloat=softfp was removed from NDK r25+ clang).
        if (arch == Architecture.arm) '-mfpu=neon',
        if (arch == Architecture.ia32 || arch == Architecture.x64) ...[
          '-msse2',
          '-msse3',
        ],
      ],
      if (os != OS.windows) ...[
        '-fvisibility=hidden',
        '-Wno-unused-command-line-argument',
        '-fsigned-char',
        '-Wno-vla',
        '-Wno-vla-cxx-extension',
      ],
      if (os == OS.linux && arch == Architecture.x64) ...['-msse2', '-msse3'],
    ];

    final includes = [
      'src',
      'src/soloud/include',
      'src/soloud/src',
      'src/pffft',
      'src/wavpack/include',
      'src/wavpack/src',
      'src/native_decoder',
      ...xiph.includeDirs,
    ];

    // iOS 26.4+/Xcode 16+ duplicate-symbol workaround for miniaudio's
    // ma_ios_notification_handler (renames the ObjC class per plugin).
    final forcedIncludes = [
      if (os == OS.iOS)
        'ios/flutter_soloud/Sources/flutter_soloud/miniaudio_objc_prefix.h',
    ];

    // On Apple, miniaudio.h pulls in AVFoundation Objective-C headers, so the
    // SoLoud miniaudio backend must be compiled as Objective-C++. CBuilder
    // with `language: .cpp` force-feeds `-x c++` for every source, which
    // breaks that — so the backend is built separately as a static library
    // (no `-x` override: the .mm extension selects Objective-C++) and linked
    // into the plugin library below.
    if (isApple) {
      final objcBuilder = CBuilder.library(
        name: 'flutter_soloud_miniaudio_objc',
        language: Language.objectiveC,
        std: 'c++17',
        linkModePreference: LinkModePreference.static,
        sources: const [
          'src/soloud_miniaudio_objc.mm',
          'src/native_decoder/os_decoder_apple.mm',
          'src/audiobuffer/aac_stream_decoder_apple.mm',
        ],
        includes: includes,
        forcedIncludes: forcedIncludes,
        defines: defines,
        flags: flags,
      );
      await objcBuilder.run(input: input, output: output);
    }

    // WavPack is C, not C++. CBuilder with `language: .cpp` force-feeds
    // `-x c++` for every source, which breaks malloc/void* assignments in
    // the vendored encoder. Build it as a C static library and link it in.
    final wavPackSources = collectWavPackSources(input.packageRoot);
    if (wavPackSources.isNotEmpty) {
      final wavPackBuilder = CBuilder.library(
        name: 'flutter_soloud_wavpack',
        language: Language.c,
        std: 'c11',
        linkModePreference: LinkModePreference.static,
        sources: wavPackSources,
        includes: includes,
        defines: defines,
        flags: flags.where((flag) => flag != '-Wno-vla').toList(),
      );
      await wavPackBuilder.run(input: input, output: output);
    }

    final builder = CBuilder.library(
      name: _libName,
      assetName: _assetName,
      language: Language.cpp,
      std: 'c++17',
      sources: collectSources(input.packageRoot, os),
      includes: includes,
      forcedIncludes: forcedIncludes,
      defines: defines,
      flags: flags,
      // Link the C++ runtime statically on Android: the prebuilt Xiph
      // libraries don't need libc++_shared.so, and nothing else bundles it
      // into the APK now that the CMake/AGP wiring is gone.
      cppLinkStdLib: os == OS.android ? 'c++_static' : null,
      frameworks: isApple
          ? const ['Foundation', 'AudioToolbox', 'AVFAudio', 'CoreAudio']
          : const [],
      libraries: [
        ...xiph.libraries,
        if (wavPackSources.isNotEmpty) 'flutter_soloud_wavpack',
        if (isApple) 'flutter_soloud_miniaudio_objc',
        if (os == OS.android) ...['log', 'android', 'mediandk'],
        if (os == OS.windows) ...[
          'mfplat',
          'mfreadwrite',
          'mfuuid',
          'shlwapi',
          'ole32',
        ],
        if (os == OS.linux) ...['dl'],
      ],
      // '.' is the hook output directory, where the miniaudio ObjC++ and
      // WavPack static libraries were just built.
      libraryDirectories: [
        if (isApple || wavPackSources.isNotEmpty) '.',
        ...xiph.libraryDirectories,
      ],
    );
    await builder.run(input: input, output: output);

    // Bundle prebuilt shared Xiph libraries on platforms where they are not
    // statically linked into the plugin library.
    for (final asset in xiph.bundledAssets) {
      output.assets.code.add(asset);
    }
    for (final dependency in xiph.dependencies) {
      output.dependencies.add(dependency);
    }
  });
}

// ---------------------------------------------------------------------------
// Source collection
// ---------------------------------------------------------------------------

/// Collects the plugin sources (paths relative to [packageRoot]) for
/// [targetOS].
List<String> collectSources(Uri packageRoot, OS targetOS) {
  final rootPath = packageRoot.toFilePath().replaceAll(r'\', '/');
  final isApple = targetOS == OS.macOS || targetOS == OS.iOS;
  final sources = <String>[];

  void addDir(
    String rel, {
    bool recursive = false,
    List<String> extensions = const ['.cpp'],
    bool Function(String path)? exclude,
  }) {
    final dir = Directory.fromUri(packageRoot.resolve('src/$rel'));
    if (!dir.existsSync()) return;
    for (final entity in dir.listSync(recursive: recursive)) {
      if (entity is! File) continue;
      final path = entity.path.replaceAll(r'\', '/');
      if (!extensions.any(path.endsWith)) continue;
      if (exclude?.call(path) ?? false) continue;
      final relPath = path.startsWith(rootPath)
          ? path.substring(rootPath.length)
          : path;
      sources.add(relPath.startsWith('/') ? relPath.substring(1) : relPath);
    }
  }

  // Plugin sources (src/CMakeLists.txt PLUGIN_SOURCES). `flutter_soloud.cpp`
  // was the SwiftPM unity translation unit and is excluded on purpose.
  addDir('', exclude: (p) => p.endsWith('/flutter_soloud.cpp'));
  addDir(
    'audiobuffer/',
    exclude: (p) {
      if (p.endsWith('.mm')) return true; // Built by objcBuilder on Apple
      if (p.endsWith('aac_stream_decoder_android.cpp') &&
          targetOS != OS.android) {
        return true;
      }
      if (p.endsWith('aac_stream_decoder_windows.cpp') &&
          targetOS != OS.windows) {
        return true;
      }
      if (p.endsWith('aac_stream_decoder_linux.cpp') && targetOS != OS.linux) {
        return true;
      }
      if (p.endsWith('aac_stream_decoder_web.cpp')) return true;
      return false;
    },
  );
  addDir('filters/');
  addDir('mixeroutput/');
  addDir('synth/');
  addDir('waveform/');
  // Miniaudio input capture. WavPack C sources are built separately:
  // CBuilder language.cpp force-feeds `-x c++`.
  addDir('capture/');
  addDir(
    'native_decoder/',
    exclude: (p) {
      if (p.endsWith('.mm')) return true; // Built by objcBuilder on Apple
      // All Apple decoder logic is in os_decoder_apple.mm.
      if (isApple) return true;
      if (targetOS == OS.android) return !p.endsWith('os_decoder_android.cpp');
      if (targetOS == OS.windows) return !p.endsWith('os_decoder_windows.cpp');
      if (targetOS == OS.linux) return !p.endsWith('os_decoder_linux.cpp');
      return false;
    },
  );
  // pffft.c is C99; the toolchain compiles it as C based on its extension.
  addDir('pffft/', extensions: const ['.c']);

  // SoLoud engine (src/src.cmake TARGET_SOURCES).
  addDir('soloud/src/core/');
  addDir('soloud/src/filter/');
  addDir(
    'soloud/src/audiosource/',
    recursive: true,
    // openmpt is Windows-only with explicit opt-in; never built here.
    exclude: (p) => p.contains('/openmpt/'),
  );

  // Backends: null and miniaudio everywhere. On Apple, soloud_miniaudio.cpp
  // must be compiled as Objective-C++ (miniaudio.h uses AVFoundation), so it
  // is built separately from src/soloud_miniaudio_objc.mm by hook/build.dart
  // instead.
  addDir('soloud/src/backend/null/');
  if (!isApple) {
    addDir('soloud/src/backend/miniaudio/');
  }

  return sources..sort();
}

/// Vendored WavPack C encoder/decoder sources, compiled as C.
List<String> collectWavPackSources(Uri packageRoot) {
  final rootPath = packageRoot.toFilePath().replaceAll(r'\', '/');
  final dir = Directory.fromUri(packageRoot.resolve('src/wavpack/src'));
  if (!dir.existsSync()) return const [];
  final sources = <String>[];
  for (final entity in dir.listSync()) {
    if (entity is! File) continue;
    final path = entity.path.replaceAll(r'\', '/');
    if (!path.endsWith('.c')) continue;
    final relPath = path.startsWith(rootPath)
        ? path.substring(rootPath.length)
        : path;
    sources.add(relPath.startsWith('/') ? relPath.substring(1) : relPath);
  }
  return sources..sort();
}
