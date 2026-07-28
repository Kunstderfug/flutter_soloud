#pragma once

#include "soloud/src/backend/miniaudio/miniaudio.h"

#include <string>

bool prepareWavPackCaptureMirror(const std::string &filePath,
                                 unsigned int sampleRate,
                                 unsigned int channels,
                                 unsigned int bitsPerSample,
                                 void **outEncoder);
bool encodeWavPackCaptureMirror(void *encoder, const float *samples,
                                ma_uint32 frameCount);
bool finishWavPackCaptureMirror(void *encoder);
