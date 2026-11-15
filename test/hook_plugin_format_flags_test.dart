// Plugin-format switches of the build hook (hook/build.dart): which formats
// `enable_vst3` / `enable_clap` (or their environment variables) compile in
// for each target OS, and which CLAP sources the hook then builds.
@TestOn('vm')
library;

import 'dart:io';

import 'package:code_assets/code_assets.dart';
import 'package:test/test.dart';

import '../hook/build.dart' as hook;

final _vst3Platforms = {OS.macOS, OS.windows, OS.linux};
final _clapPlatforms = {OS.macOS};

bool _enabled(
  OS os,
  Set<OS> supported, {
  Object? define,
  String? environment,
}) => hook.pluginFormatEnabled(
  os: os,
  supported: supported,
  define: define,
  environment: environment,
);

void main() {
  group('pluginFormatEnabled', () {
    test('is off by default', () {
      for (final os in _vst3Platforms) {
        expect(_enabled(os, _vst3Platforms), isFalse, reason: '$os');
      }
    });

    test('takes a bool or bool string define', () {
      expect(_enabled(OS.linux, _vst3Platforms, define: true), isTrue);
      expect(_enabled(OS.linux, _vst3Platforms, define: 'TRUE'), isTrue);
      expect(_enabled(OS.linux, _vst3Platforms, define: '1'), isTrue);
      expect(_enabled(OS.linux, _vst3Platforms, define: false), isFalse);
      expect(_enabled(OS.linux, _vst3Platforms, define: '0'), isFalse);
    });

    test('takes platform names as a list or comma-separated string', () {
      expect(
        _enabled(OS.macOS, _vst3Platforms, define: ['macos']),
        isTrue,
      );
      expect(
        _enabled(OS.windows, _vst3Platforms, define: ['macos']),
        isFalse,
      );
      expect(
        _enabled(OS.windows, _vst3Platforms, define: ' MacOS , windows'),
        isTrue,
      );
      expect(_enabled(OS.linux, _vst3Platforms, define: 'macos'), isFalse);
    });

    test('a define wins over the environment', () {
      expect(
        _enabled(OS.macOS, _vst3Platforms, define: false, environment: '1'),
        isFalse,
      );
      expect(
        _enabled(OS.macOS, _vst3Platforms, define: 'windows', environment: '1'),
        isFalse,
      );
      expect(_enabled(OS.macOS, _vst3Platforms, environment: 'true'), isTrue);
      expect(_enabled(OS.macOS, _vst3Platforms, environment: 'no'), isFalse);
    });

    test('never enables a platform outside the supported set', () {
      for (final os in [OS.iOS, OS.android]) {
        expect(_enabled(os, _vst3Platforms, define: true), isFalse);
        expect(
          _enabled(os, _vst3Platforms, define: [os.name], environment: '1'),
          isFalse,
        );
      }
      // CLAP is macOS-only for now.
      for (final os in [OS.windows, OS.linux]) {
        expect(
          _enabled(os, _clapPlatforms, define: [os.name], environment: '1'),
          isFalse,
        );
      }
      expect(_enabled(OS.macOS, _clapPlatforms, define: true), isTrue);
    });
  });

  group('CLAP sources', () {
    final root = Directory.current.uri;
    List<String> clap(List<String> sources) =>
        sources.where((s) => s.startsWith('src/clap/')).toList();

    test('are built only with enableClap, never the vendored SDK', () {
      expect(clap(hook.collectSources(root, OS.macOS)), isEmpty);
      final sources = clap(
        hook.collectSources(root, OS.macOS, enableClap: true),
      );
      expect(sources, contains('src/clap/clap_host.cpp'));
      expect(sources.where((s) => s.contains('/third_party/')), isEmpty);
    });

    test('bindings for CLAP are always built', () {
      expect(
        hook.collectSources(root, OS.macOS),
        contains('src/clap_bindings.cpp'),
      );
    });

    test('vendored CLAP headers are present', () {
      expect(
        File('${hook.clapIncludeDir}/clap/clap.h').existsSync(),
        isTrue,
      );
      expect(
        hook.collectClapObjcSources(root).where(
          (s) => s.contains('/third_party/'),
        ),
        isEmpty,
      );
    });
  });
}
