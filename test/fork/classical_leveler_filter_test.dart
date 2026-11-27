// The fork's Classical Leveler filter: its Dart parameter table matches the
// native filter. Moved out of test/common/filter_type_test.dart, which keeps
// only the fork's enum index (marked `// fork:`). See FORK_MAINTENANCE.md.
import 'package:flutter_soloud/src/bindings/flutter_soloud_ffigen.dart'
    as native;
import 'package:flutter_soloud/src/filters/classical_leveler_filter.dart';
import 'package:flutter_soloud/src/filters/filters.dart';
import 'package:test/test.dart';

void main() {
  test('classical leveler ranges, defaults and transport match native', () {
    expect(
      native.FilterType.fromValue(14),
      native.FilterType.ClassicalLevelerFilter,
    );
    expect(FilterType.classicalLevelerFilter.numParameters, 57);
    expect(ClassicalLeveler.values.length, 9);
    expect(ClassicalLeveler.values.map((p) => p.def).toList(), [
      0,
      50,
      -30,
      -60,
      4,
      500,
      0,
      -10,
      1.3,
    ]);
    const global = ClassicalLevelerGlobal();
    expect(global.filterType, FilterType.classicalLevelerFilter);
    expect(global.liftPercent.attributeId, 1);
    expect(global.thresholdLufs.min, -45);
    expect(global.floorLufs.max, -50);
    expect(global.fallTimeMs.max, 2000);
    expect(
      () => global.setLookAheadPlan(nowLufs: -40, lufs: [-40], leadSeconds: []),
      throwsArgumentError,
    );
    expect(
      () => global.setLookAheadPlan(
        nowLufs: double.nan,
        lufs: [-40],
        leadSeconds: [0],
      ),
      throwsArgumentError,
    );
  });
}
