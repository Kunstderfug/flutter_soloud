#include "wavpack_capture_encoder.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>
#include <vector>

#if !defined(NO_WAVPACK_LIBS)
#if defined(__APPLE__)
#define ChunkHeader WavpackChunkHeader
#endif
#include "wavpack/include/wavpack.h"
#if defined(__APPLE__)
#undef ChunkHeader
#endif

namespace
{
    struct WavPackCaptureEncoderState
    {
        WavpackContext *context = nullptr;
        FILE *file = nullptr;
        unsigned int channels = 0;
        unsigned int bitsPerSample = 0;
        bool isFloat = false;
        bool failed = false;
        std::vector<int32_t> sampleBuffer;
    };

    int32_t floatToWavPackPcm(float sample, unsigned int bitsPerSample)
    {
        if (!std::isfinite(sample))
            sample = 0.0f;
        const float clamped = std::clamp(sample, -1.0f, 1.0f);
        const int32_t maxValue =
            static_cast<int32_t>((uint32_t{1} << (bitsPerSample - 1)) - 1);
        const int32_t minValue =
            -static_cast<int32_t>(uint32_t{1} << (bitsPerSample - 1));
        const int64_t scaled =
            static_cast<int64_t>(std::lrintf(clamped * maxValue));
        return static_cast<int32_t>(
            std::clamp<int64_t>(scaled, minValue, maxValue));
    }

    int writeWavPackCaptureBlock(void *id, void *data, int32_t length)
    {
        auto *encoder = static_cast<WavPackCaptureEncoderState *>(id);
        if (encoder == nullptr || encoder->file == nullptr || data == nullptr ||
            length <= 0 || encoder->failed)
            return 0;
        if (fwrite(data, 1, static_cast<size_t>(length), encoder->file) !=
            static_cast<size_t>(length))
        {
            encoder->failed = true;
            return 0;
        }
        return 1;
    }

    uint32_t wavPackCaptureChannelMask(unsigned int channels)
    {
        if (channels == 1)
            return 0x4;
        if (channels == 2)
            return 0x3;
        return channels >= 32 ? 0 : (uint32_t{1} << channels) - 1;
    }
}

bool prepareWavPackCaptureMirror(const std::string &filePath,
                                 unsigned int sampleRate,
                                 unsigned int channels,
                                 unsigned int bitsPerSample,
                                 void **outEncoder)
{
    if (outEncoder == nullptr || filePath.empty() || sampleRate == 0 ||
        channels == 0 ||
        (bitsPerSample != 16 && bitsPerSample != 24 && bitsPerSample != 32))
        return false;

    auto *encoder = new (std::nothrow) WavPackCaptureEncoderState();
    if (encoder == nullptr)
        return false;
    encoder->channels = channels;
    encoder->bitsPerSample = bitsPerSample;
    encoder->isFloat = bitsPerSample == 32;
    encoder->file = fopen(filePath.c_str(), "wb");
    if (encoder->file == nullptr)
    {
        delete encoder;
        return false;
    }
    encoder->context =
        WavpackOpenFileOutput(writeWavPackCaptureBlock, encoder, nullptr);
    if (encoder->context == nullptr)
    {
        fclose(encoder->file);
        delete encoder;
        return false;
    }

    WavpackConfig config;
    memset(&config, 0, sizeof(config));
    config.sample_rate = static_cast<int32_t>(sampleRate);
    config.num_channels = static_cast<int>(channels);
    config.bits_per_sample = static_cast<int>(bitsPerSample);
    config.bytes_per_sample = static_cast<int>((bitsPerSample + 7) / 8);
    config.channel_mask =
        static_cast<int32_t>(wavPackCaptureChannelMask(channels));
    if (encoder->isFloat)
        config.float_norm_exp = 127;
    if (!WavpackSetConfiguration64(encoder->context, &config, -1, nullptr) ||
        !WavpackPackInit(encoder->context))
    {
        WavpackCloseFile(encoder->context);
        fclose(encoder->file);
        delete encoder;
        return false;
    }
    *outEncoder = encoder;
    return true;
}

bool encodeWavPackCaptureMirror(void *opaque, const float *samples,
                                ma_uint32 frameCount)
{
    auto *encoder = static_cast<WavPackCaptureEncoderState *>(opaque);
    if (encoder == nullptr || encoder->context == nullptr ||
        samples == nullptr || frameCount == 0 || encoder->channels == 0)
        return false;
    const size_t sampleCount =
        static_cast<size_t>(frameCount) * encoder->channels;
    try
    {
        encoder->sampleBuffer.resize(sampleCount);
    }
    catch (...)
    {
        encoder->failed = true;
        return false;
    }
    for (size_t i = 0; i < sampleCount; ++i)
    {
        if (encoder->isFloat)
        {
            float sample = std::isfinite(samples[i]) ? samples[i] : 0.0f;
            sample = std::clamp(sample, -1.0f, 1.0f);
            memcpy(&encoder->sampleBuffer[i], &sample, sizeof(sample));
        }
        else
        {
            encoder->sampleBuffer[i] =
                floatToWavPackPcm(samples[i], encoder->bitsPerSample);
        }
    }
    if (!WavpackPackSamples(encoder->context, encoder->sampleBuffer.data(),
                            frameCount))
    {
        encoder->failed = true;
        return false;
    }
    return true;
}

bool finishWavPackCaptureMirror(void *opaque)
{
    auto *encoder = static_cast<WavPackCaptureEncoderState *>(opaque);
    if (encoder == nullptr)
        return false;
    bool succeeded = !encoder->failed;
    if (encoder->context != nullptr)
        succeeded = WavpackFlushSamples(encoder->context) && succeeded;
    if (encoder->file != nullptr && fflush(encoder->file) != 0)
        succeeded = false;
    if (encoder->context != nullptr)
        WavpackCloseFile(encoder->context);
    if (encoder->file != nullptr && fclose(encoder->file) != 0)
        succeeded = false;
    delete encoder;
    return succeeded;
}
#else
bool prepareWavPackCaptureMirror(const std::string &, unsigned int,
                                 unsigned int, unsigned int, void **)
{
    return false;
}
bool encodeWavPackCaptureMirror(void *, const float *, ma_uint32)
{
    return false;
}
bool finishWavPackCaptureMirror(void *)
{
    return false;
}
#endif
