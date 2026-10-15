// Xiph codec library selection and prebuilt/system linking.
// ignore_for_file: avoid_print

import 'dart:io';

import 'package:code_assets/code_assets.dart';
import 'package:hooks/hooks.dart';

part 'xiph_source.dart';

// ---------------------------------------------------------------------------
// Prebuilt Xiph library wiring
// ---------------------------------------------------------------------------

/// Xiph library basenames, in link order (vorbisfile/vorbisenc before vorbis,
/// everything before ogg).
const _xiphLibs = ['FLAC', 'opus', 'vorbisfile', 'vorbisenc', 'vorbis', 'ogg'];

/// Android ABI names matching the directories in xiph/prebuild/android/.
String? _androidAbi(Architecture arch) => switch (arch) {
  Architecture.arm64 => 'arm64-v8a',
  Architecture.arm => 'armeabi-v7a',
  Architecture.x64 => 'x86_64',
  Architecture.ia32 => 'x86',
  _ => null,
};

/// How the Xiph libraries are linked for one target.
final class XiphLink {
  XiphLink._({
    required this.libraries,
    required this.libraryDirectories,
    required this.includeDirs,
    required this.bundledAssets,
    required this.dependencies,
  });

  factory XiphLink.empty() => XiphLink._(
    libraries: const [],
    libraryDirectories: const [],
    includeDirs: const [],
    bundledAssets: const [],
    dependencies: const [],
  );

  /// Links against prebuilt Xiph libraries.
  ///
  /// Searches for prebuilt files in the local cache directory
  /// `.dart_tool/flutter_soloud/xiph/prebuild/`. If missing, automatically
  /// downloads the corresponding archive from GitHub releases and extracts it.
  static Future<XiphLink> forPrebuild(BuildInput input) async {
    final code = input.config.code;
    final os = code.targetOS;
    final arch = code.targetArchitecture;
    final packageRoot = input.packageRoot;
    final packageName = input.packageName;

    final tag =
        (input.userDefines['prebuild_tag'] as String?)?.trim() ??
        _defaultPrebuildTag;

    final includeDir = await _ensurePrebuildInclude(packageRoot, tag);

    switch (os) {
      case OS.macOS:
        final dir = await _ensurePrebuildPlatform(
          packageRoot: packageRoot,
          os: os,
          arch: arch,
          subDir: 'macos',
          archiveName: 'xiph-macos.tar.gz',
          isZip: false,
          tag: tag,
          validator: (Directory d) => _xiphLibs.every(
            (String lib) => File('${d.path}/lib$lib.a').existsSync(),
          ),
        );
        return XiphLink._(
          libraries: _xiphLibs,
          libraryDirectories: [dir.path],
          includeDirs: [includeDir.path],
          bundledAssets: const [],
          dependencies: [
            for (final lib in _xiphLibs) File('${dir.path}/lib$lib.a').uri,
          ],
        );

      case OS.iOS:
        final suffix = switch (code.iOS.targetSdk) {
          IOSSdk.iPhoneSimulator => 'simulator',
          _ => 'device',
        };
        final names = [for (final lib in _xiphLibs) '${lib}_iOS-$suffix'];
        final dir = await _ensurePrebuildPlatform(
          packageRoot: packageRoot,
          os: os,
          arch: arch,
          subDir: 'ios',
          archiveName: 'xiph-ios.tar.gz',
          isZip: false,
          tag: tag,
          validator: (Directory d) => names.every(
            (String name) => File('${d.path}/lib$name.a').existsSync(),
          ),
        );
        return XiphLink._(
          libraries: names,
          libraryDirectories: [dir.path],
          includeDirs: [includeDir.path],
          bundledAssets: const [],
          dependencies: [
            for (final name in names) File('${dir.path}/lib$name.a').uri,
          ],
        );

      case OS.android:
        final abi = _androidAbi(code.targetArchitecture);
        if (abi == null) {
          throw UnsupportedError(
            'Unsupported Android architecture: ${code.targetArchitecture}',
          );
        }
        final dir = await _ensurePrebuildPlatform(
          packageRoot: packageRoot,
          os: os,
          arch: arch,
          subDir: 'android/$abi',
          archiveName: 'xiph-android.tar.gz',
          isZip: false,
          tag: tag,
          validator: (Directory d) => _xiphLibs.every(
            (String lib) => File('${d.path}/lib$lib.so').existsSync(),
          ),
        );
        return XiphLink._(
          libraries: _xiphLibs,
          libraryDirectories: [dir.path],
          includeDirs: [includeDir.path],
          bundledAssets: [
            for (final lib in _xiphLibs)
              CodeAsset(
                package: packageName,
                name: 'xiph/$abi/lib$lib.so',
                linkMode: DynamicLoadingBundled(),
                file: File('${dir.path}/lib$lib.so').uri,
              ),
          ],
          dependencies: [
            for (final lib in _xiphLibs) File('${dir.path}/lib$lib.so').uri,
          ],
        );

      case OS.windows:
        if (code.targetArchitecture != Architecture.x64 &&
            code.targetArchitecture != Architecture.arm64) {
          throw UnsupportedError(
            '[flutter_soloud] Bundled prebuilt Windows libraries only support '
            'x64 and arm64. For ${code.targetArchitecture.name}, either '
            'install system libraries and set `windows_use_system_libs: true` '
            'in pubspec.yaml, or build from source with '
            '`windows_force_build_libs: true`.',
          );
        }
        final archStr = arch == Architecture.arm64 ? 'arm64' : 'x64';
        final dir = await _ensurePrebuildPlatform(
          packageRoot: packageRoot,
          os: os,
          arch: arch,
          subDir: 'windows/$archStr',
          archiveName: 'xiph-windows-$archStr.zip',
          isZip: true,
          tag: tag,
          validator: (Directory d) => _xiphLibs.every(
            (String lib) =>
                File('${d.path}/$lib.dll').existsSync() &&
                File('${d.path}/$lib.lib').existsSync(),
          ),
        );
        return XiphLink._(
          libraries: _xiphLibs,
          libraryDirectories: [dir.path],
          includeDirs: [includeDir.path],
          bundledAssets: [
            for (final lib in _xiphLibs)
              CodeAsset(
                package: packageName,
                name: 'xiph/$lib.dll',
                linkMode: DynamicLoadingBundled(),
                file: File('${dir.path}/$lib.dll').uri,
              ),
          ],
          dependencies: [
            for (final lib in _xiphLibs) ...[
              File('${dir.path}/$lib.lib').uri,
              File('${dir.path}/$lib.dll').uri,
            ],
          ],
        );

      case OS.linux:
        if (code.targetArchitecture != Architecture.x64 &&
            code.targetArchitecture != Architecture.arm64) {
          throw UnsupportedError(
            '[flutter_soloud] Bundled prebuilt Linux libraries only support '
            'x64 and arm64. For ${code.targetArchitecture.name}, either '
            'install system libraries and set `linux_use_system_libs: true` '
            'in pubspec.yaml, or build from source with '
            '`linux_force_build_libs: true`.',
          );
        }
        final archStr = arch == Architecture.arm64 ? 'arm64' : 'x64';
        final dir = await _ensurePrebuildPlatform(
          packageRoot: packageRoot,
          os: os,
          arch: arch,
          subDir: 'linux/$archStr',
          archiveName: 'xiph-linux-$archStr.tar.gz',
          isZip: false,
          tag: tag,
          validator: (Directory d) => _xiphLibs.every(
            (String lib) => File('${d.path}/lib$lib.so').existsSync(),
          ),
        );

        // Bundle each library by its runtime SONAME (e.g., libFLAC.so.14)
        // so references resolve properly without duplicating symlinks (fixes
        // issue #559).
        final soFiles = <File>[
          for (final lib in _xiphLibs) await _resolveLinuxSoFile(dir, lib),
        ];

        return XiphLink._(
          libraries: _xiphLibs,
          libraryDirectories: [dir.path],
          includeDirs: [includeDir.path],
          bundledAssets: [
            for (final file in soFiles)
              CodeAsset(
                package: packageName,
                name: 'xiph/${file.uri.pathSegments.last}',
                linkMode: DynamicLoadingBundled(),
                file: file.uri,
              ),
          ],
          dependencies: [for (final file in soFiles) file.uri],
        );

      default:
        throw UnsupportedError('Unsupported OS for bundled Xiph: $os');
    }
  }

  static const String _prebuildRepo = 'alnitak/flutter_soloud_prebuilds';
  static const String _defaultPrebuildTag = 'latest';
  static const String _prebuildVersionFileName =
      'flutter_soloud_prebuild_version.txt';

  static String _baseUrlForTag(String tag) => tag == 'latest'
      ? 'https://github.com/$_prebuildRepo/releases/latest/download'
      : 'https://github.com/$_prebuildRepo/releases/download/$tag';

  static File _getVersionFile(Directory dir) {
    final file = File('${dir.path}/$_prebuildVersionFileName');
    if (file.existsSync()) return file;
    return File('${dir.path}/version.txt');
  }

  static bool _hasHeaders(Directory dir, [String targetTag = 'latest']) {
    if (!dir.existsSync()) return false;
    if (targetTag != 'latest') {
      final versionFile = _getVersionFile(dir);
      if (versionFile.existsSync()) {
        final v = versionFile.readAsStringSync().trim();
        if (v != targetTag && 'v$v' != targetTag) return false;
      }
    }
    return File('${dir.path}/ogg/ogg.h').existsSync() &&
        File('${dir.path}/ogg/config_types.h').existsSync() &&
        File('${dir.path}/vorbis/codec.h').existsSync() &&
        File('${dir.path}/opus/opus.h').existsSync() &&
        File('${dir.path}/FLAC/all.h').existsSync();
  }

  static Future<Directory> _ensurePrebuildInclude(
    Uri packageRoot, [
    String tag = _defaultPrebuildTag,
  ]) async {
    final cacheDir = Directory.fromUri(
      packageRoot.resolve('.dart_tool/flutter_soloud/xiph/prebuild/include'),
    );
    if (_hasHeaders(cacheDir, tag)) return cacheDir;

    print(
      '[flutter_soloud] Downloading Xiph headers ($tag) from '
      '$_prebuildRepo...',
    );
    if (cacheDir.existsSync()) {
      cacheDir.deleteSync(recursive: true);
    }
    final resolvedTag = await _downloadAndExtract(
      '${_baseUrlForTag(tag)}/xiph-include.tar.gz',
      cacheDir,
      isZip: false,
    );
    final tagToWrite = resolvedTag ?? tag;
    File(
      '${cacheDir.path}/$_prebuildVersionFileName',
    ).writeAsStringSync('$tagToWrite\n');

    File.fromUri(
        packageRoot.resolve(
          '.dart_tool/flutter_soloud/xiph/prebuild/$_prebuildVersionFileName',
        ),
      )
      ..parent.createSync(recursive: true)
      ..writeAsStringSync('$tagToWrite\n');
    return cacheDir;
  }

  static Future<Directory> _ensurePrebuildPlatform({
    required Uri packageRoot,
    required OS os,
    required Architecture arch,
    required String subDir,
    required String archiveName,
    required bool isZip,
    required bool Function(Directory dir) validator,
    String tag = _defaultPrebuildTag,
  }) async {
    final cacheDir = Directory.fromUri(
      packageRoot.resolve('.dart_tool/flutter_soloud/xiph/prebuild/$subDir'),
    );
    final versionFile = _getVersionFile(cacheDir);
    if (tag != 'latest' && versionFile.existsSync()) {
      final v = versionFile.readAsStringSync().trim();
      if (v != tag && 'v$v' != tag) {
        if (cacheDir.existsSync()) {
          cacheDir.deleteSync(recursive: true);
        }
      }
    }
    if (validator(cacheDir)) return cacheDir;

    print(
      '[flutter_soloud] Downloading prebuilt Xiph libraries ($tag) '
      'for $os ($arch)...',
    );
    final extractTarget = (os == OS.android)
        ? Directory.fromUri(
            packageRoot.resolve(
              '.dart_tool/flutter_soloud/xiph/prebuild/android',
            ),
          )
        : cacheDir;
    final resolvedTag = await _downloadAndExtract(
      '${_baseUrlForTag(tag)}/$archiveName',
      extractTarget,
      isZip: isZip,
    );
    final tagToWrite = resolvedTag ?? tag;
    File(
      '${cacheDir.path}/$_prebuildVersionFileName',
    ).writeAsStringSync('$tagToWrite\n');

    if (!validator(cacheDir)) {
      throw StateError(
        'Downloaded prebuilt Xiph libraries for $os ($arch) from '
        '${_baseUrlForTag(tag)}/$archiveName, but validation failed in '
        '${cacheDir.path}.',
      );
    }
    return cacheDir;
  }

  static Future<String?> _downloadAndExtract(
    String url,
    Directory destination, {
    required bool isZip,
  }) async {
    destination.createSync(recursive: true);
    final tempFile = File(
      '${destination.path}/dl_${DateTime.now().millisecondsSinceEpoch}.${isZip ? 'zip' : 'tar.gz'}',
    );

    String? resolvedTag;
    final client = HttpClient();
    try {
      final uri = Uri.parse(url);
      final request = await client.getUrl(uri);
      final response = await request.close();
      if (response.statusCode != HttpStatus.ok) {
        throw HttpException(
          'Failed to download prebuilt libraries from $url '
          '(HTTP ${response.statusCode}). Ensure you have an internet '
          'connection or build from source using '
          '`<platform>_force_build_libs: true`.',
          uri: uri,
        );
      }
      for (final r in response.redirects) {
        final match = RegExp(
          '/releases/download/([^/]+)/',
        ).firstMatch(r.location.toString());
        if (match != null) {
          resolvedTag = match.group(1);
          break;
        }
      }
      await response.pipe(tempFile.openWrite());
    } finally {
      client.close();
    }

    try {
      if (isZip && Platform.isWindows) {
        final tarRes = await Process.run('tar', [
          '-xf',
          tempFile.path,
          '-C',
          destination.path,
        ]);
        if (tarRes.exitCode != 0) {
          final cmd =
              'Expand-Archive -Path "${tempFile.path}" '
              '-DestinationPath "${destination.path}" -Force';
          final psRes = await Process.run('powershell', [
            '-NoProfile',
            '-Command',
            cmd,
          ]);
          if (psRes.exitCode != 0) {
            throw ProcessException(
              'powershell',
              ['Expand-Archive'],
              psRes.stderr.toString(),
              psRes.exitCode,
            );
          }
        }
      } else {
        final res = await Process.run('tar', [
          '-xzf',
          tempFile.path,
          '-C',
          destination.path,
        ]);
        if (res.exitCode != 0) {
          throw ProcessException(
            'tar',
            ['-xzf', tempFile.path],
            res.stderr.toString(),
            res.exitCode,
          );
        }
      }
    } finally {
      if (tempFile.existsSync()) {
        tempFile.deleteSync();
      }
    }
    return resolvedTag;
  }

  /// Links against system-installed Xiph libraries (apt, brew, pacman, etc.).
  static Future<XiphLink> forSystem(BuildInput input) async {
    final code = input.config.code;
    final os = code.targetOS;
    final includeDirs = <String>[];
    final libDirs = <String>[];

    // Query pkg-config if available
    try {
      final res = await Process.run('pkg-config', [
        '--cflags-only-I',
        '--libs-only-L',
        'ogg',
        'vorbis',
        'vorbisfile',
        'vorbisenc',
        'opus',
        'flac',
      ]);
      if (res.exitCode == 0) {
        final out = (res.stdout as String).trim();
        for (final token in out.split(RegExp(r'\s+'))) {
          if (token.startsWith('-I')) {
            final dir = token.substring(2);
            if (Directory(dir).existsSync() && !includeDirs.contains(dir)) {
              includeDirs.add(dir);
            }
          } else if (token.startsWith('-L')) {
            final dir = token.substring(2);
            if (Directory(dir).existsSync() && !libDirs.contains(dir)) {
              libDirs.add(dir);
            }
          }
        }
      }
    } catch (_) {}

    // Add standard OS fallback paths
    if (os == OS.linux) {
      const fallbackIncludes = [
        '/usr/include',
        '/usr/include/opus',
        '/usr/local/include',
        '/usr/local/include/opus',
      ];
      for (final inc in fallbackIncludes) {
        if (Directory(inc).existsSync() && !includeDirs.contains(inc)) {
          includeDirs.add(inc);
        }
      }
      const fallbackLibs = [
        '/usr/lib',
        '/usr/local/lib',
        '/usr/lib64',
        '/usr/lib/aarch64-linux-gnu',
        '/usr/lib/x86_64-linux-gnu',
        '/usr/lib/arm-linux-gnueabihf',
        '/usr/lib/riscv64-linux-gnu',
      ];
      for (final lib in fallbackLibs) {
        if (Directory(lib).existsSync() && !libDirs.contains(lib)) {
          libDirs.add(lib);
        }
      }
    } else if (os == OS.macOS) {
      const fallbackIncludes = [
        '/opt/homebrew/include',
        '/opt/homebrew/include/opus',
        '/usr/local/include',
        '/usr/local/include/opus',
      ];
      for (final inc in fallbackIncludes) {
        if (Directory(inc).existsSync() && !includeDirs.contains(inc)) {
          includeDirs.add(inc);
        }
      }
      const fallbackLibs = ['/opt/homebrew/lib', '/usr/local/lib'];
      for (final lib in fallbackLibs) {
        if (Directory(lib).existsSync() && !libDirs.contains(lib)) {
          libDirs.add(lib);
        }
      }
    } else if (os == OS.windows) {
      final vcpkgRoot = Platform.environment['VCPKG_ROOT'] ?? r'C:\vcpkg';
      final vcpkgTriplet = code.targetArchitecture == Architecture.arm64
          ? 'arm64-windows'
          : 'x64-windows';
      for (final root in [
        vcpkgRoot,
        r'C:\tools\vcpkg',
        r'C:\Program Files\Xiph',
        r'C:\Xiph',
      ]) {
        final inc = '$root\\installed\\$vcpkgTriplet\\include';
        final lib = '$root\\installed\\$vcpkgTriplet\\lib';
        if (Directory(inc).existsSync()) includeDirs.add(inc);
        if (Directory(lib).existsSync()) libDirs.add(lib);
        final directInc = '$root\\include';
        final directLib = '$root\\lib';
        if (Directory(directInc).existsSync()) includeDirs.add(directInc);
        if (Directory(directLib).existsSync()) libDirs.add(directLib);
      }
    }

    return XiphLink._(
      libraries: _xiphLibs,
      libraryDirectories: libDirs,
      includeDirs: includeDirs,
      bundledAssets: const [],
      dependencies: const [],
    );
  }

  /// Checks whether system-installed Xiph libraries and headers are available.
  static Future<bool> hasSystemLibs(OS os) async {
    if (os != OS.linux && os != OS.macOS && os != OS.windows) return false;

    // 1. Check via pkg-config if available
    try {
      final res = await Process.run('pkg-config', [
        '--exists',
        'ogg',
        'vorbis',
        'vorbisfile',
        'vorbisenc',
        'opus',
        'flac',
      ]);
      if (res.exitCode == 0) return true;
    } catch (_) {}

    // 2. Check filesystem search paths
    final includeDirs = <String>[];
    final libDirs = <String>[];
    if (os == OS.linux) {
      const fallbackIncludes = [
        '/usr/include',
        '/usr/include/opus',
        '/usr/local/include',
        '/usr/local/include/opus',
      ];
      for (final inc in fallbackIncludes) {
        if (Directory(inc).existsSync()) includeDirs.add(inc);
      }
      const fallbackLibs = [
        '/usr/lib',
        '/usr/local/lib',
        '/usr/lib64',
        '/usr/lib/aarch64-linux-gnu',
        '/usr/lib/x86_64-linux-gnu',
        '/usr/lib/arm-linux-gnueabihf',
        '/usr/lib/riscv64-linux-gnu',
      ];
      for (final lib in fallbackLibs) {
        if (Directory(lib).existsSync()) libDirs.add(lib);
      }
    } else if (os == OS.macOS) {
      const fallbackIncludes = [
        '/opt/homebrew/include',
        '/opt/homebrew/include/opus',
        '/usr/local/include',
        '/usr/local/include/opus',
      ];
      for (final inc in fallbackIncludes) {
        if (Directory(inc).existsSync()) includeDirs.add(inc);
      }
      const fallbackLibs = ['/opt/homebrew/lib', '/usr/local/lib'];
      for (final lib in fallbackLibs) {
        if (Directory(lib).existsSync()) libDirs.add(lib);
      }
    } else if (os == OS.windows) {
      final vcpkgRoot = Platform.environment['VCPKG_ROOT'] ?? r'C:\vcpkg';
      for (final root in [
        vcpkgRoot,
        r'C:\tools\vcpkg',
        r'C:\Program Files\Xiph',
        r'C:\Xiph',
      ]) {
        final inc = '$root\\installed\\x64-windows\\include';
        final lib = '$root\\installed\\x64-windows\\lib';
        if (Directory(inc).existsSync()) includeDirs.add(inc);
        if (Directory(lib).existsSync()) libDirs.add(lib);
        final directInc = '$root\\include';
        final directLib = '$root\\lib';
        if (Directory(directInc).existsSync()) includeDirs.add(directInc);
        if (Directory(directLib).existsSync()) libDirs.add(directLib);
      }
    }

    bool hasHeader(String relative) =>
        includeDirs.any((d) => File('$d/$relative').existsSync());
    bool hasAnyHeader(List<String> relatives) => relatives.any(hasHeader);

    final hasHeaders =
        hasHeader('ogg/ogg.h') &&
        hasHeader('vorbis/codec.h') &&
        hasAnyHeader(['opus.h', 'opus/opus.h']) &&
        hasHeader('FLAC/all.h');

    if (!hasHeaders) return false;

    bool hasLibrary(String name) => libDirs.any((d) {
      if (os == OS.windows) {
        return File('$d/$name.lib').existsSync();
      }
      return File('$d/lib$name.so').existsSync() ||
          File('$d/lib$name.a').existsSync() ||
          File('$d/lib$name.dylib').existsSync();
    });

    return [
      'ogg',
      'vorbis',
      'vorbisfile',
      'vorbisenc',
      'opus',
      'FLAC',
    ].every((lib) => hasLibrary(lib) || hasLibrary(lib.toLowerCase()));
  }

  /// Builds Xiph libraries from source by cloning git repositories into
  /// `.dart_tool/flutter_soloud/xiph/sources/` and building with CMake into
  /// `.dart_tool/flutter_soloud/xiph/install/<os>/<arch>/` (or `install/ios/<simulator|device>/<arch>/` on iOS).
  static Future<XiphLink> fromSource(BuildInput input) =>
      _buildXiphFromSource(input);

  /// Library names passed to the linker (`-l<name>`).
  final List<String> libraries;

  /// Directories searched for [libraries]. Absolute paths.
  final List<String> libraryDirectories;

  /// Xiph header directories, relative to the package root or absolute paths.
  final List<String> includeDirs;

  /// Extra code assets to bundle (prebuilt shared libraries).
  final List<CodeAsset> bundledAssets;

  /// Prebuilt files consumed by the build, for cache invalidation.
  final List<Uri> dependencies;
}
