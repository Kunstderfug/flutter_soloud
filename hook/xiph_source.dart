// Builds and links Xiph codec libraries from source.
// ignore_for_file: avoid_print

part of 'xiph.dart';

Future<XiphLink> _buildXiphFromSource(BuildInput input) async {
  final code = input.config.code;
  final os = code.targetOS;
  final arch = code.targetArchitecture;
  final packageRoot = input.packageRoot;
  final isApple = os == OS.macOS || os == OS.iOS;
  final String? appleSysroot;
  if (os == OS.iOS) {
    appleSysroot = code.iOS.targetSdk == IOSSdk.iPhoneSimulator
        ? 'iphonesimulator'
        : 'iphoneos';
  } else {
    appleSysroot = null;
  }

  final cacheDir = Directory.fromUri(
    packageRoot.resolve('.dart_tool/flutter_soloud/xiph'),
  );
  final sourcesDir = Directory('${cacheDir.path}/sources');
  final String targetSubdir;
  final String targetDesc;
  if (os == OS.iOS) {
    final isSim = code.iOS.targetSdk == IOSSdk.iPhoneSimulator;
    final sdkName = isSim ? 'simulator' : 'device';
    targetSubdir = 'ios/$sdkName/${arch.name}';
    targetDesc = 'iOS ($arch $sdkName)';
  } else {
    targetSubdir = '${os.name}/${arch.name}';
    targetDesc = '$os ($arch)';
  }
  final buildDir = Directory('${cacheDir.path}/build/$targetSubdir');
  final installDir = Directory('${cacheDir.path}/install/$targetSubdir');
  final installIncDir = Directory('${installDir.path}/include');
  final installLibDir = Directory('${installDir.path}/lib');
  final installLib64Dir = Directory('${installDir.path}/lib64');

  // 1. Check if complete build already exists in cache
  if (_isCompleteInstall(installDir, os)) {
    print(
      '[flutter_soloud] Using cached from-source Xiph libs for $targetDesc',
    );
    return _linkFromInstall(input, installDir, os, arch);
  }

  // 2. Check build tool availability
  final hasGit = _checkTool('git');
  final hasCmake = _checkTool('cmake');
  if (!hasGit || !hasCmake) {
    throw UnsupportedError(
      'CMake and Git are required to build Xiph libraries from source for '
      '$targetDesc. Please ensure cmake and git are installed and available '
      'on your PATH. Alternatively, install system libraries and set '
      '`hooks.user_defines.flutter_soloud.use_system_xiph_libs: true` '
      'in pubspec.yaml, or disable Xiph with `no_xiph_libs: true`.',
    );
  }

  // 3. Clone / checkout repositories
  await _ensureRepo(
    'ogg',
    'https://github.com/xiph/ogg',
    'db5c7a4',
    sourcesDir,
  );
  await _ensureRepo(
    'vorbis',
    'https://github.com/xiph/vorbis',
    '84c0236',
    sourcesDir,
  );
  await _ensureRepo(
    'opus',
    'https://github.com/xiph/opus',
    'c79a9bd',
    sourcesDir,
  );
  await _ensureRepo(
    'flac',
    'https://github.com/xiph/flac',
    '9547dbc',
    sourcesDir,
  );

  // 4. Build with CMake
  installDir.createSync(recursive: true);
  final archStr = _cmakeArch(arch);
  final osxDeploymentTarget = os == OS.iOS ? '12.0' : '10.13';

  final String? androidNdk;
  final String? androidAbi;
  final int? androidNdkApi;
  if (os == OS.android) {
    androidAbi = _androidAbi(arch);
    if (androidAbi == null) {
      throw UnsupportedError('Unsupported Android architecture: $arch');
    }
    androidNdkApi = code.android.targetNdkApi;
    androidNdk = _findAndroidNdk(input);
    if (androidNdk == null) {
      throw UnsupportedError(
        'Could not find Android NDK for building Xiph libraries from source. '
        'Please ensure ANDROID_NDK_HOME or ANDROID_HOME is set.',
      );
    }
  } else {
    androidNdk = null;
    androidAbi = null;
    androidNdkApi = null;
  }

  const androidLinkerFlags =
      '-DCMAKE_SHARED_LINKER_FLAGS='
      '-Wl,-z,max-page-size=16384,--gc-sections -flto';
  final androidFlags = [
    if (os == OS.android) ...[
      '-DCMAKE_TOOLCHAIN_FILE=$androidNdk/build/cmake/android.toolchain.cmake',
      '-DANDROID_ABI=$androidAbi',
      '-DANDROID_PLATFORM=android-$androidNdkApi',
      '-DCMAKE_C_FLAGS=-Os -flto -ffunction-sections -fdata-sections',
      androidLinkerFlags,
    ],
  ];

  final commonCmakePlatformFlags = [
    if (isApple) ...[
      '-DBUILD_SHARED_LIBS=OFF',
      '-DCMAKE_POSITION_INDEPENDENT_CODE=ON',
      '-DCMAKE_OSX_ARCHITECTURES=$archStr',
      '-DCMAKE_OSX_DEPLOYMENT_TARGET=$osxDeploymentTarget',
      if (os == OS.iOS) ...[
        '-DCMAKE_SYSTEM_NAME=iOS',
        '-DCMAKE_OSX_SYSROOT=$appleSysroot',
      ],
    ] else if (os == OS.windows) ...[
      '-DBUILD_SHARED_LIBS=ON',
      '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded',
      if (arch == Architecture.arm64) ...['-A', 'ARM64'],
    ] else if (os == OS.android) ...[
      '-DBUILD_SHARED_LIBS=ON',
      ...androidFlags,
    ] else ...[
      '-DBUILD_SHARED_LIBS=ON',
      '-DCMAKE_POSITION_INDEPENDENT_CODE=ON',
      '-DCMAKE_C_FLAGS=-O2 -flto -ffunction-sections -fdata-sections',
      '-DCMAKE_SHARED_LINKER_FLAGS=-Wl,--gc-sections -flto',
      if (os == OS.linux &&
          arch == Architecture.arm64 &&
          Platform.version.contains('x64')) ...[
        '-DCMAKE_SYSTEM_NAME=Linux',
        '-DCMAKE_SYSTEM_PROCESSOR=aarch64',
        '-DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc',
        '-DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++',
      ],
    ],
  ];

  void prepareBuildDir(String lib) {
    final cacheFile = File('${buildDir.path}/$lib/CMakeCache.txt');
    if (cacheFile.existsSync()) {
      cacheFile.deleteSync();
    }
  }

  // Build ogg
  print('[flutter_soloud] Configuring ogg with CMake ($archStr)...');
  prepareBuildDir('ogg');
  await _runCmake([
    '-S',
    '${sourcesDir.path}/ogg',
    '-B',
    '${buildDir.path}/ogg',
    '-DCMAKE_INSTALL_PREFIX=${installDir.path}',
    '-DCMAKE_BUILD_TYPE=Release',
    '-DCMAKE_POLICY_VERSION_MINIMUM=3.5',
    '-DINSTALL_DOCS=OFF',
    '-DBUILD_TESTING=OFF',
    ...commonCmakePlatformFlags,
  ]);
  print('[flutter_soloud] Compiling and installing ogg...');
  await _runCmake([
    '--build',
    '${buildDir.path}/ogg',
    '--config',
    'Release',
    '--target',
    'install',
  ]);

  // Build opus
  const opusAppleFlags =
      '-Os -fno-exceptions -fno-unwind-tables '
      '-fno-asynchronous-unwind-tables';
  print('[flutter_soloud] Configuring opus with CMake ($archStr)...');
  prepareBuildDir('opus');
  await _runCmake([
    '-S',
    '${sourcesDir.path}/opus',
    '-B',
    '${buildDir.path}/opus',
    '-DCMAKE_INSTALL_PREFIX=${installDir.path}',
    '-DCMAKE_BUILD_TYPE=Release',
    '-DCMAKE_POLICY_VERSION_MINIMUM=3.5',
    '-DOPUS_BUILD_PROGRAMS=OFF',
    '-DOPUS_BUILD_TESTING=OFF',
    '-DOPUS_STACK_PROTECTOR=OFF',
    '-DOPUS_CUSTOM_MODES=ON',
    if (isApple) ...[
      '-DOPUS_BUILD_SHARED_LIBRARY=OFF',
      '-DCMAKE_C_FLAGS=$opusAppleFlags',
    ],
    ...commonCmakePlatformFlags,
  ]);
  print('[flutter_soloud] Compiling and installing opus...');
  await _runCmake([
    '--build',
    '${buildDir.path}/opus',
    '--config',
    'Release',
    '--target',
    'install',
  ]);

  // Locate built ogg library for vorbis and flac
  final oggLib = isApple
      ? '${installLibDir.path}/libogg.a'
      : (os == OS.windows
            ? '${installLibDir.path}/ogg.lib'
            : (File('${installLib64Dir.path}/libogg.so').existsSync()
                  ? '${installLib64Dir.path}/libogg.so'
                  : '${installLibDir.path}/libogg.so'));

  // Build vorbis
  print('[flutter_soloud] Configuring vorbis with CMake ($archStr)...');
  prepareBuildDir('vorbis');
  await _runCmake([
    '-S',
    '${sourcesDir.path}/vorbis',
    '-B',
    '${buildDir.path}/vorbis',
    '-DCMAKE_INSTALL_PREFIX=${installDir.path}',
    '-DCMAKE_BUILD_TYPE=Release',
    '-DCMAKE_POLICY_VERSION_MINIMUM=3.5',
    '-DOGG_INCLUDE_DIR=${installIncDir.path}',
    '-DOGG_LIBRARY=$oggLib',
    '-DBUILD_TESTING=OFF',
    ...commonCmakePlatformFlags,
  ]);
  print('[flutter_soloud] Compiling and installing vorbis...');
  await _runCmake([
    '--build',
    '${buildDir.path}/vorbis',
    '--config',
    'Release',
    '--target',
    'install',
  ]);

  // Build flac
  print('[flutter_soloud] Configuring flac with CMake ($archStr)...');
  prepareBuildDir('flac');
  await _runCmake([
    '-S',
    '${sourcesDir.path}/flac',
    '-B',
    '${buildDir.path}/flac',
    '-DCMAKE_INSTALL_PREFIX=${installDir.path}',
    '-DCMAKE_BUILD_TYPE=Release',
    '-DCMAKE_POLICY_VERSION_MINIMUM=3.5',
    '-DOGG_INCLUDE_DIR=${installIncDir.path}',
    '-DOGG_LIBRARY=$oggLib',
    '-DBUILD_CXXLIBS=OFF',
    '-DBUILD_PROGRAMS=OFF',
    '-DBUILD_EXAMPLES=OFF',
    '-DBUILD_TESTING=OFF',
    '-DBUILD_DOCS=OFF',
    '-DINSTALL_MANPAGES=OFF',
    if (isApple) '-DWITH_OGG=ON',
    if (os == OS.iOS) ...['-DIconv_FOUND=OFF', '-DIntl_FOUND=OFF'],
    ...commonCmakePlatformFlags,
  ]);
  print('[flutter_soloud] Compiling and installing flac...');
  await _runCmake([
    '--build',
    '${buildDir.path}/flac',
    '--config',
    'Release',
    '--target',
    'install',
  ]);

  // Copy FLAC share headers if present
  final shareSrc = Directory('${sourcesDir.path}/flac/include/share');
  if (shareSrc.existsSync()) {
    _copyDirSync(shareSrc, Directory('${installIncDir.path}/share'));
  }
  // Remove FLAC++ C++ wrapper headers and binaries
  final flacppDir = Directory('${installIncDir.path}/FLAC++');
  if (flacppDir.existsSync()) {
    flacppDir.deleteSync(recursive: true);
  }
  for (final dir in [installLibDir, installLib64Dir]) {
    if (!dir.existsSync()) continue;
    for (final entity in dir.listSync()) {
      if (entity.path.contains('libFLAC++')) {
        try {
          entity.deleteSync();
        } catch (_) {}
      }
    }
  }

  // Strip symbols from built libraries to match prebuilt releases
  if (os == OS.android && androidNdk != null) {
    final llvmStrip = _findNdkStrip(androidNdk);
    if (llvmStrip != null) {
      for (final dir in [installLibDir, installLib64Dir]) {
        if (!dir.existsSync()) continue;
        for (final file in dir.listSync()) {
          if (file is File && file.path.endsWith('.so')) {
            await Process.run(llvmStrip, [file.path]);
          }
        }
      }
    }
  } else if (os == OS.linux) {
    final isCrossArm64 =
        arch == Architecture.arm64 && Platform.version.contains('x64');
    final stripTool = isCrossArm64 ? 'aarch64-linux-gnu-strip' : 'strip';
    for (final dir in [installLibDir, installLib64Dir]) {
      if (!dir.existsSync()) continue;
      for (final file in dir.listSync()) {
        if (file is File && file.path.contains('.so')) {
          await Process.run(stripTool, ['--strip-unneeded', file.path]);
        }
      }
    }
  } else if (isApple) {
    for (final dir in [installLibDir, installLib64Dir]) {
      if (!dir.existsSync()) continue;
      for (final file in dir.listSync()) {
        if (file is File && file.path.endsWith('.a')) {
          await Process.run('strip', ['-x', file.path]);
        }
      }
    }
  }

  return _linkFromInstall(input, installDir, os, arch);
}

// ---------------------------------------------------------------------------
// Build from source helpers
// ---------------------------------------------------------------------------

String _cmakeArch(Architecture arch) => switch (arch) {
  Architecture.arm64 => 'arm64',
  Architecture.x64 => 'x86_64',
  Architecture.arm => 'armv7-a',
  Architecture.ia32 => 'i686',
  Architecture.riscv64 => 'riscv64',
  _ => arch.name,
};

String? _findAndroidNdk(BuildInput input) {
  final envNdk =
      Platform.environment['ANDROID_NDK_HOME'] ??
      Platform.environment['ANDROID_NDK_ROOT'];
  if (envNdk != null &&
      Directory(envNdk).existsSync() &&
      File('$envNdk/build/cmake/android.toolchain.cmake').existsSync()) {
    return envNdk;
  }
  try {
    final cc = input.config.code.cCompiler?.compiler.toFilePath();
    if (cc != null && cc.contains('toolchains')) {
      final ndkPath = cc
          .substring(0, cc.indexOf('toolchains'))
          .replaceAll(RegExp(r'[/\\]$'), '');
      if (Directory(ndkPath).existsSync() &&
          File('$ndkPath/build/cmake/android.toolchain.cmake').existsSync()) {
        return ndkPath;
      }
    }
  } catch (_) {}

  final sdkRoot =
      Platform.environment['ANDROID_HOME'] ??
      Platform.environment['ANDROID_SDK_ROOT'];
  if (sdkRoot != null) {
    final ndkDir = Directory('$sdkRoot/ndk');
    if (ndkDir.existsSync()) {
      final versions = ndkDir.listSync().whereType<Directory>().toList();
      if (versions.isNotEmpty) {
        versions.sort((a, b) => a.path.compareTo(b.path));
        final candidate = versions.last.path;
        if (File(
          '$candidate/build/cmake/android.toolchain.cmake',
        ).existsSync()) {
          return candidate;
        }
      }
    }
  }
  return null;
}

String? _findNdkStrip(String ndkPath) {
  final prebuiltDir = Directory('$ndkPath/toolchains/llvm/prebuilt');
  if (!prebuiltDir.existsSync()) return null;
  final exe = Platform.isWindows ? 'llvm-strip.exe' : 'llvm-strip';
  for (final host in prebuiltDir.listSync()) {
    if (host is Directory) {
      final stripBin = File('${host.path}/bin/$exe');
      if (stripBin.existsSync()) {
        return stripBin.path;
      }
    }
  }
  return null;
}

bool _checkTool(String tool) {
  try {
    final res = Process.runSync(tool, ['--version']);
    return res.exitCode == 0;
  } catch (_) {
    return false;
  }
}

bool _isCompleteInstall(Directory installDir, OS os) {
  if (!installDir.existsSync()) return false;
  final inc = Directory('${installDir.path}/include');
  if (!File('${inc.path}/ogg/ogg.h').existsSync() ||
      !File('${inc.path}/vorbis/codec.h').existsSync() ||
      !File('${inc.path}/opus/opus.h').existsSync() ||
      !File('${inc.path}/FLAC/all.h').existsSync()) {
    return false;
  }
  final libDir = Directory('${installDir.path}/lib');
  final lib64Dir = Directory('${installDir.path}/lib64');
  bool hasLib(String name) =>
      File('${libDir.path}/$name').existsSync() ||
      File('${lib64Dir.path}/$name').existsSync();

  if (os == OS.macOS || os == OS.iOS) {
    return [
      'libogg.a',
      'libvorbis.a',
      'libvorbisfile.a',
      'libvorbisenc.a',
      'libopus.a',
      'libFLAC.a',
    ].every(hasLib);
  } else if (os == OS.windows) {
    return [
      'ogg.lib',
      'vorbis.lib',
      'vorbisfile.lib',
      'vorbisenc.lib',
      'opus.lib',
      'FLAC.lib',
    ].every(hasLib);
  } else {
    return [
      'libogg.so',
      'libvorbis.so',
      'libvorbisfile.so',
      'libvorbisenc.so',
      'libopus.so',
      'libFLAC.so',
    ].every(hasLib);
  }
}

Future<void> _ensureRepo(
  String name,
  String url,
  String commit,
  Directory parent,
) async {
  final repoDir = Directory('${parent.path}/$name');
  if (!repoDir.existsSync()) {
    parent.createSync(recursive: true);
    print('[flutter_soloud] Cloning $name ($commit)...');
    final cloneRes = await Process.run('git', ['clone', url, repoDir.path]);
    if (cloneRes.exitCode != 0) {
      throw ProcessException(
        'git',
        ['clone', url, repoDir.path],
        cloneRes.stderr.toString(),
        cloneRes.exitCode,
      );
    }
    await Process.run('git', [
      'checkout',
      commit,
    ], workingDirectory: repoDir.path);
  }
}

Future<void> _runCmake(List<String> args, {String? workingDir}) async {
  final res = await Process.run('cmake', args, workingDirectory: workingDir);
  if (res.exitCode != 0) {
    throw ProcessException(
      'cmake',
      args,
      '${res.stdout}\n${res.stderr}',
      res.exitCode,
    );
  }
}

void _copyDirSync(Directory src, Directory dst) {
  if (!src.existsSync()) return;
  dst.createSync(recursive: true);
  for (final entity in src.listSync(recursive: true)) {
    final relPath = entity.path.substring(src.path.length + 1);
    final targetPath = '${dst.path}/$relPath';
    if (entity is Directory) {
      Directory(targetPath).createSync(recursive: true);
    } else if (entity is File) {
      File(targetPath).parent.createSync(recursive: true);
      entity.copySync(targetPath);
    }
  }
}

/// Resolves the actual runtime SONAME file for [lib] in [dir] to avoid
/// bundling duplicate unversioned or full-versioned files (e.g., bundles only
/// `libFLAC.so.14` rather than `libFLAC.so`, `libFLAC.so.14`, and
/// `libFLAC.so.14.0.0`).
Future<File> _resolveLinuxSoFile(Directory dir, String lib) async {
  final unversioned = File('${dir.path}/lib$lib.so');
  if (!unversioned.existsSync()) return unversioned;
  try {
    final res = await Process.run('readelf', ['-d', unversioned.path]);
    if (res.exitCode == 0) {
      final out = res.stdout as String;
      final match = RegExp(
        r'\(SONAME\)\s+Library soname:\s+\[([^\]]+)\]',
      ).firstMatch(out);
      if (match != null) {
        final sonameFile = File('${dir.path}/${match.group(1)}');
        if (sonameFile.existsSync()) {
          return sonameFile;
        }
      }
    }
  } catch (_) {}
  return unversioned;
}

Future<XiphLink> _linkFromInstall(
  BuildInput input,
  Directory installDir,
  OS os,
  Architecture arch,
) async {
  final packageName = input.packageName;
  final libDir = Directory('${installDir.path}/lib');
  final lib64Dir = Directory('${installDir.path}/lib64');
  final binDir = Directory('${installDir.path}/bin');
  final libraryDirs = [
    if (libDir.existsSync()) libDir.path,
    if (lib64Dir.existsSync()) lib64Dir.path,
    if (binDir.existsSync()) binDir.path,
  ];
  final includeDirs = ['${installDir.path}/include'];
  final bundledAssets = <CodeAsset>[];
  final dependencies = <Uri>[];

  if (os == OS.linux) {
    for (final lib in _xiphLibs) {
      final dir = File('${libDir.path}/lib$lib.so').existsSync()
          ? libDir
          : lib64Dir;
      final file = await _resolveLinuxSoFile(dir, lib);
      if (file.existsSync()) {
        dependencies.add(file.uri);
        bundledAssets.add(
          CodeAsset(
            package: packageName,
            name: 'xiph/${file.uri.pathSegments.last}',
            linkMode: DynamicLoadingBundled(),
            file: file.uri,
          ),
        );
      }
    }
  } else if (os == OS.android) {
    final abi = _androidAbi(arch);
    if (abi == null) {
      throw UnsupportedError('Unsupported Android architecture: $arch');
    }
    for (final dir in [libDir, lib64Dir]) {
      if (!dir.existsSync()) continue;
      for (final file in dir.listSync()) {
        if (file is File && file.path.endsWith('.so')) {
          final fileName = file.path.split(Platform.pathSeparator).last;
          if (_xiphLibs.any((lib) => fileName == 'lib$lib.so')) {
            dependencies.add(file.uri);
            bundledAssets.add(
              CodeAsset(
                package: packageName,
                name: 'xiph/$abi/$fileName',
                linkMode: DynamicLoadingBundled(),
                file: file.uri,
              ),
            );
          }
        }
      }
    }
  } else if (os == OS.windows) {
    for (final dir in [binDir, libDir]) {
      if (!dir.existsSync()) continue;
      for (final file in dir.listSync()) {
        if (file is File) {
          if (file.path.endsWith('.lib')) {
            dependencies.add(file.uri);
          } else if (file.path.endsWith('.dll')) {
            dependencies.add(file.uri);
            bundledAssets.add(
              CodeAsset(
                package: packageName,
                name: 'xiph/${file.path.split(Platform.pathSeparator).last}',
                linkMode: DynamicLoadingBundled(),
                file: file.uri,
              ),
            );
          }
        }
      }
    }
  } else {
    // macOS / iOS: static libraries
    if (libDir.existsSync()) {
      for (final file in libDir.listSync()) {
        if (file is File && file.path.endsWith('.a')) {
          dependencies.add(file.uri);
        }
      }
    }
  }

  return XiphLink._(
    libraries: _xiphLibs,
    libraryDirectories: libraryDirs,
    includeDirs: includeDirs,
    bundledAssets: bundledAssets,
    dependencies: dependencies,
  );
}
