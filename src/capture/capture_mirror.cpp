#include "capture_mirror.h"
#include "wavpack_capture_encoder.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#if !defined(NO_XIPH_LIBS)
#include <FLAC/stream_encoder.h>
#endif

namespace
{
#if !defined(NO_XIPH_LIBS)
    int32_t floatToFlacPcm(float sample, unsigned int bitsPerSample)
    {
        if (!std::isfinite(sample))
            sample = 0.0f;
        const float clamped = std::clamp(sample, -1.0f, 1.0f);
        const int32_t maxValue =
            static_cast<int32_t>((uint32_t{1} << (bitsPerSample - 1)) - 1);
        const int32_t minValue =
            -static_cast<int32_t>(uint32_t{1} << (bitsPerSample - 1));
        return static_cast<int32_t>(std::clamp<int64_t>(
            static_cast<int64_t>(std::lrintf(clamped * maxValue)),
            minValue, maxValue));
    }
#endif

    unsigned int normalizeCaptureMirrorBits(unsigned int format,
                                            unsigned int bits)
    {
        if (format == captureMirrorWavPack)
            return bits == 16 || bits == 24 || bits == 32 ? bits : 24;
        if (format == captureMirrorFlac)
            return bits == 16 ? 16 : 24;
        return 0;
    }
}

bool CaptureMirror::prepare(const std::string &filePath, unsigned int format,
                            unsigned int bitsPerSample,
                            unsigned int sampleRate, unsigned int channels)
{
    reset();
    if (format == captureMirrorNone)
        return true;
    mFilePath = filePath;
    mFormat = format;
    mBitsPerSample = normalizeCaptureMirrorBits(format, bitsPerSample);
    mChannels = channels;
    if (filePath.empty() || mBitsPerSample == 0)
    {
        mFailed = true;
        return true;
    }

    if (format == captureMirrorFlac)
    {
#if defined(NO_XIPH_LIBS)
        mFailed = true;
#else
        auto *encoder = FLAC__stream_encoder_new();
        if (encoder == nullptr)
        {
            mFailed = true;
        }
        else
        {
            const bool configured =
                FLAC__stream_encoder_set_channels(encoder, channels) &&
                FLAC__stream_encoder_set_sample_rate(encoder, sampleRate) &&
                FLAC__stream_encoder_set_bits_per_sample(
                    encoder, mBitsPerSample) &&
                FLAC__stream_encoder_set_compression_level(encoder, 3);
            const auto status = configured
                ? FLAC__stream_encoder_init_file(
                      encoder, filePath.c_str(), nullptr, nullptr)
                : FLAC__STREAM_ENCODER_INIT_STATUS_ENCODER_ERROR;
            if (!configured ||
                status != FLAC__STREAM_ENCODER_INIT_STATUS_OK)
            {
                FLAC__stream_encoder_delete(encoder);
                mFailed = true;
            }
            else
            {
                mEncoder = encoder;
                mActive = true;
            }
        }
#endif
    }
    else if (format == captureMirrorWavPack)
    {
        if (prepareWavPackCaptureMirror(filePath, sampleRate, channels,
                                        mBitsPerSample, &mEncoder))
            mActive = true;
        else
            mFailed = true;
    }
    else
    {
        mFailed = true;
    }

    if (mFailed)
        remove(filePath.c_str());
    return true;
}

bool CaptureMirror::encode(const float *samples, ma_uint32 frameCount)
{
    if (!mActive || mEncoder == nullptr || samples == nullptr ||
        frameCount == 0)
        return true;
    bool succeeded = false;
    if (mFormat == captureMirrorFlac)
    {
#if !defined(NO_XIPH_LIBS)
        const size_t sampleCount =
            static_cast<size_t>(frameCount) * mChannels;
        mIntBuffer.resize(sampleCount);
        for (size_t i = 0; i < sampleCount; ++i)
            mIntBuffer[i] = floatToFlacPcm(samples[i], mBitsPerSample);
        succeeded = FLAC__stream_encoder_process_interleaved(
            static_cast<FLAC__StreamEncoder *>(mEncoder),
            mIntBuffer.data(), frameCount);
#endif
    }
    else if (mFormat == captureMirrorWavPack)
    {
        succeeded =
            encodeWavPackCaptureMirror(mEncoder, samples, frameCount);
    }
    if (succeeded)
        mFrameCount += frameCount;
    else
        mFailed = true;
    return succeeded;
}

bool CaptureMirror::finish(bool deleteOutput)
{
    bool succeeded = !mFailed;
    if (mEncoder != nullptr && mFormat == captureMirrorFlac)
    {
#if !defined(NO_XIPH_LIBS)
        auto *encoder = static_cast<FLAC__StreamEncoder *>(mEncoder);
        succeeded = FLAC__stream_encoder_finish(encoder) && succeeded;
        FLAC__stream_encoder_delete(encoder);
#endif
    }
    else if (mEncoder != nullptr && mFormat == captureMirrorWavPack)
    {
        succeeded = finishWavPackCaptureMirror(mEncoder) && succeeded;
    }
    mEncoder = nullptr;
    mActive = false;
    if ((deleteOutput || !succeeded) && !mFilePath.empty())
        remove(mFilePath.c_str());
    return succeeded && !deleteOutput;
}

void CaptureMirror::reset()
{
    mFilePath.clear();
    mFormat = captureMirrorNone;
    mBitsPerSample = 0;
    mChannels = 0;
    mActive = false;
    mFailed = false;
    mEncoder = nullptr;
    mIntBuffer.clear();
    mFrameCount = 0;
}
