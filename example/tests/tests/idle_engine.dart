import 'package:flutter_soloud/flutter_soloud.dart';

import 'common.dart';

/// Verifies that removing the final mixing bus lets the audio device idle.
Future<OutputBuffer> testIdleEngineAfterMixingBusDispose() async {
  final output = OutputBuffer();
  await initialize();

  try {
    final bus = SoLoud.instance.createMixingBus(name: 'Idle regression bus');
    final handle = bus.playOnEngine();
    assert(!handle.isError, 'Mixing bus should start on the engine');
    await delay(100);
    assert(bus.isActive, 'Mixing bus should be active before disposal');

    bus.dispose();

    // Native idle pausing is deliberately deferred by 500 ms to coalesce
    // rapid stop/start bursts. Sample after that window, then verify the
    // engine clock remains stopped instead of mixing silent buffers.
    await delay(800);
    final stoppedAt = SoLoud.instance.getEngineTime();
    await delay(300);
    final sampledAt = SoLoud.instance.getEngineTime();
    final advance = sampledAt - stoppedAt;
    output.writeln(
      'Engine clock advanced ${advance.inMicroseconds}us while idle',
    );
    assert(
      advance <= const Duration(milliseconds: 20),
      'Engine clock continued advancing after the final mixing bus was '
      'disposed: '
      '${advance.inMicroseconds}us',
    );
  } finally {
    deinit();
  }

  return output;
}
