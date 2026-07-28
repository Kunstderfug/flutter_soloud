#pragma once

#include "capture_models.h"

#include <cstdint>
#include <string>
#include <vector>

class CaptureMirror
{
public:
    bool prepare(const std::string &filePath, unsigned int format,
                 unsigned int bitsPerSample, unsigned int sampleRate,
                 unsigned int channels);
    bool encode(const float *samples, ma_uint32 frameCount);
    bool finish(bool deleteOutput);
    void reset();

    unsigned int format() const { return mFormat; }
    bool active() const { return mActive; }
    bool failed() const { return mFailed; }
    uint64_t frameCount() const { return mFrameCount; }

private:
    std::string mFilePath;
    unsigned int mFormat = captureMirrorNone;
    unsigned int mBitsPerSample = 0;
    unsigned int mChannels = 0;
    bool mActive = false;
    bool mFailed = false;
    void *mEncoder = nullptr;
    std::vector<int32_t> mIntBuffer;
    uint64_t mFrameCount = 0;
};
