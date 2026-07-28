#pragma once

#include <cstddef>

/// Applies capture gain to a known sample block. Kept independent of devices so
/// the same transform used by the callback can be verified deterministically.
void applyCaptureGain(const float *input, size_t sampleCount, float linearGain,
                      float *output);
