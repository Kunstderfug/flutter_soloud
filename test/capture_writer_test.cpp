#include "../src/capture/capture_processing.h"
#include "../src/capture/capture_writer.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

namespace
{
    bool nearlyEqual(float actual, float expected, float tolerance = 0.0001f)
    {
        return std::fabs(actual - expected) <= tolerance;
    }
}

int main()
{
    // Exercise the exact gain transform used by the capture callback. +6 dB
    // should be approximately a 2x linear multiplier.
    const float input[] = {0.1f, -0.2f, 0.25f};
    float gained[3] = {};
    const float gain6Db = std::pow(10.0f, 6.0f / 20.0f);
    applyCaptureGain(input, 3, gain6Db, gained);
    assert(nearlyEqual(gained[0], input[0] * gain6Db));
    assert(nearlyEqual(gained[1], input[1] * gain6Db));
    assert(nearlyEqual(gained[2] / input[2], 2.0f, 0.01f));

    FILE *file = tmpfile();
    assert(file != nullptr);
    CaptureWriter writer;
    assert(writer.startForTesting(file, 1, 2, nullptr));

    const float first[] = {1.0f, 2.0f};
    const float overflow[] = {3.0f, 4.0f, 5.0f};
    const float laterWhileOverflowed[] = {6.0f};
    const float recovered[] = {7.0f};
    assert(writer.enqueue(first, 2));
    assert(!writer.enqueue(overflow, 3));
    assert(!writer.enqueue(laterWhileOverflowed, 1));
    writer.drainForTesting();
    assert(writer.enqueue(recovered, 1));
    writer.drainForTesting();

    const CaptureWriterStats stats = writer.stats();
    assert(stats.frameCount == 7);
    assert(stats.overflowFrames == 4);
    assert(stats.silenceFrames == 4);
    assert(!stats.failed);

    assert(fflush(file) == 0);
    assert(fseek(file, 0, SEEK_SET) == 0);
    std::vector<float> output(7);
    assert(fread(output.data(), sizeof(float), output.size(), file) ==
           output.size());
    assert(output[0] == 1.0f);
    assert(output[1] == 2.0f);
    for (size_t i = 2; i < 6; ++i)
        assert(output[i] == 0.0f);
    assert(output[6] == 7.0f);

    fclose(file);
    return 0;
}
