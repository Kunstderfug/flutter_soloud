#include "capture_processing.h"

void applyCaptureGain(const float *input, size_t sampleCount, float linearGain,
                      float *output)
{
    if (input == nullptr || output == nullptr)
        return;
    for (size_t i = 0; i < sampleCount; ++i)
        output[i] = input[i] * linearGain;
}
