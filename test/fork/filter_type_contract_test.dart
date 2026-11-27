// Fork contract: every FilterType value, the fork's Classical Leveler
// included, can be activated as a global filter on the real engine, its
// parameters read, and deactivated.
//
// - A new FilterType value (from upstream or the fork) stops this file from
//   compiling until it gets an entry in [_global].
// - The native addFilter validates the Dart enum index against the native
//   filter list (src/enums.h), so a fork value whose index drifts from the
//   native one fails at activate() with filterNotFound, or reads the wrong
//   filter's parameter names. See FORK_MAINTENANCE.md.
@Tags(['device'])
library;

import 'package:flutter/foundation.dart' show kIsWeb;
import 'package:flutter_soloud/flutter_soloud.dart';
import 'package:flutter_soloud/src/bindings/soloud_controller.dart';
import 'package:flutter_soloud/src/enums.dart';
import 'package:flutter_soloud/src/filters/filters.dart';
import 'package:flutter_test/flutter_test.dart';

import '../support/sounds.dart';

/// The global filter of each type.
FilterBase _global(FiltersGlobal f, FilterType type) => switch (type) {
  FilterType.biquadResonantFilter => f.biquadResonantFilter,
  FilterType.echoFilter => f.echoFilter,
  FilterType.lofiFilter => f.lofiFilter,
  FilterType.flangerFilter => f.flangerFilter,
  FilterType.bassboostFilter => f.bassBoostFilter,
  FilterType.waveShaperFilter => f.waveShaperFilter,
  FilterType.robotizeFilter => f.robotizeFilter,
  FilterType.freeverbFilter => f.freeverbFilter,
  FilterType.pitchShiftFilter => f.pitchShiftFilter,
  FilterType.limiterFilter => f.limiterFilter,
  FilterType.compressorFilter => f.compressorFilter,
  FilterType.parametricEq => f.parametricEqFilter,
  FilterType.amplitudeModulatorFilter => f.amplitudeModulatorFilter,
  FilterType.customFilter => f.customFilter(0),
  FilterType.classicalLevelerFilter => f.classicalLevelerFilter,
};

/// The native name of each built-in filter's first parameter.
const _firstParamName = 'Wet';

void main() {
  setUpEngineTests();

  for (final type in FilterType.values) {
    test('$type: activate globally, read its parameters, deactivate', () {
      final filter = _global(SoLoud.instance.filters, type);
      expect(filter.filterType, type);
      expect(filter.isActive, isFalse);

      filter.activate();
      expect(filter.isActive, isTrue, reason: 'activate() must add it');

      final ffi = SoLoudController().soLoudFFI;
      if (type == FilterType.customFilter) {
        // Custom slots have their own native path and parameter table.
        final wet = (filter as CustomFilterGlobal).wet.value;
        expect(wet.isFinite, isTrue);
      } else {
        final names = ffi.getFilterParamNames(type);
        expect(names.error, PlayerErrors.noError);
        expect(names.names, isNotEmpty);
        expect(
          names.names.first,
          _firstParamName,
          reason: 'the native filter at index ${type.index} is not $type',
        );
        expect(
          names.names.length,
          type.numParameters,
          reason: 'the native filter at index ${type.index} is not $type',
        );
        for (var i = 0; i < names.names.length; i++) {
          final value = ffi.getFilterParams(type, i);
          expect(
            value.error,
            PlayerErrors.noError,
            reason: '$type parameter $i (${names.names[i]})',
          );
          expect(value.value.isFinite, isTrue, reason: '$type parameter $i');
        }
      }

      filter.deactivate();
      expect(filter.isActive, isFalse, reason: 'deactivate() must remove it');
    }, skip: kIsWeb ? 'the parameter-name probe uses the FFI' : false);
  }
}
