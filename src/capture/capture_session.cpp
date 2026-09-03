#include "capture_session.h"

#include "capture_processing.h"
#include "player.h"
#include "soloud.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace SoLoud
{
    extern ma_context context;
}

namespace
{
    bool writeCaptureBytes(FILE *file, const void *data, size_t size)
    {
        return file != nullptr && fwrite(data, 1, size, file) == size;
    }

    bool writeCaptureU16le(FILE *file, uint16_t value)
    {
        const unsigned char bytes[2] = {
            static_cast<unsigned char>(value & 0xff),
            static_cast<unsigned char>((value >> 8) & 0xff),
        };
        return writeCaptureBytes(file, bytes, sizeof(bytes));
    }

    bool writeCaptureU32le(FILE *file, uint32_t value)
    {
        const unsigned char bytes[4] = {
            static_cast<unsigned char>(value & 0xff),
            static_cast<unsigned char>((value >> 8) & 0xff),
            static_cast<unsigned char>((value >> 16) & 0xff),
            static_cast<unsigned char>((value >> 24) & 0xff),
        };
        return writeCaptureBytes(file, bytes, sizeof(bytes));
    }

    uint32_t clampCaptureWavSize(uint64_t value)
    {
        return value > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(value);
    }
}

CaptureSession::CaptureSession(Player &player) : mPlayer(player)
{
}

CaptureSession::~CaptureSession()
{
    cancel();
}

std::vector<CaptureDevice> CaptureSession::listDevices()
{
    ma_context context;
    ma_device_info *playbackInfos;
    ma_uint32 playbackCount;
    ma_device_info *captureInfos;
    ma_uint32 captureCount;
    std::vector<CaptureDevice> result;
    if (ma_context_init(nullptr, 0, nullptr, &context) != MA_SUCCESS)
        return result;
    if (ma_context_get_devices(&context, &playbackInfos, &playbackCount,
                               &captureInfos, &captureCount) != MA_SUCCESS)
    {
        ma_context_uninit(&context);
        return result;
    }
    for (ma_uint32 i = 0; i < captureCount; ++i)
    {
        CaptureDevice device;
        device.name = strdup(captureInfos[i].name);
        device.isDefault = captureInfos[i].isDefault;
        device.id = i;
        device.deviceId = captureInfos[i].id;
        result.push_back(device);
    }
    ma_context_uninit(&context);
    return result;
}

uint64_t CaptureSession::nowHostTimeNanos()
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

void CaptureSession::atomicMaxFloat(std::atomic<float> &target, float value)
{
    float current = target.load();
    while (value > current && !target.compare_exchange_weak(current, value))
    {
    }
}

bool CaptureSession::writeWavHeader(FILE *file, unsigned int sampleRate,
                                    unsigned int channels)
{
    if (file == nullptr || sampleRate == 0 || channels == 0)
        return false;
    const uint16_t blockAlign =
        static_cast<uint16_t>(channels * sizeof(float));
    return writeCaptureBytes(file, "RIFF", 4) &&
           writeCaptureU32le(file, 36) &&
           writeCaptureBytes(file, "WAVE", 4) &&
           writeCaptureBytes(file, "fmt ", 4) &&
           writeCaptureU32le(file, 16) && writeCaptureU16le(file, 3) &&
           writeCaptureU16le(file, static_cast<uint16_t>(channels)) &&
           writeCaptureU32le(file, sampleRate) &&
           writeCaptureU32le(file, sampleRate * blockAlign) &&
           writeCaptureU16le(file, blockAlign) &&
           writeCaptureU16le(file, 32) &&
           writeCaptureBytes(file, "data", 4) &&
           writeCaptureU32le(file, 0);
}

bool CaptureSession::finalizeWav(FILE *file, uint64_t dataSizeBytes)
{
    if (file == nullptr)
        return false;
    bool succeeded = fseek(file, 4, SEEK_SET) == 0;
    succeeded =
        writeCaptureU32le(file, clampCaptureWavSize(dataSizeBytes + 36)) &&
        succeeded;
    succeeded = fseek(file, 40, SEEK_SET) == 0 && succeeded;
    succeeded =
        writeCaptureU32le(file, clampCaptureWavSize(dataSizeBytes)) &&
        succeeded;
    succeeded = fseek(file, 0, SEEK_END) == 0 && succeeded;
    succeeded = fflush(file) == 0 && succeeded;
    return succeeded;
}

void CaptureSession::dataCallback(ma_device *device, void *output,
                                  const void *input, ma_uint32 frameCount)
{
    (void)output;
    if (device != nullptr && device->pUserData != nullptr)
        static_cast<CaptureSession *>(device->pUserData)
            ->handleFrames(input, frameCount);
}

void CaptureSession::handleFrames(const void *input, ma_uint32 frameCount)
{
    if (!mRecording || mFile == nullptr || input == nullptr || frameCount == 0)
        return;
    uint64_t expected = 0;
    const uint64_t currentFrame = mFrameCount.load();
    if (mFirstInputHostTimeNanos.compare_exchange_strong(
            expected, nowHostTimeNanos()))
        mFirstInputFrameIndex.store(currentFrame);

    const size_t sampleCount = static_cast<size_t>(frameCount) * mChannels;
    const float *samples = static_cast<const float *>(input);
    if (std::fabs(mInputGain - 1.0f) >= 0.0001f)
    {
        mGainBuffer.resize(sampleCount);
        applyCaptureGain(samples, sampleCount, mInputGain, mGainBuffer.data());
        samples = mGainBuffer.data();
    }

    float peak = 0.0f;
    double sumSquares = 0.0;
    for (size_t i = 0; i < sampleCount; ++i)
    {
        peak = std::max(peak, std::fabs(samples[i]));
        sumSquares += static_cast<double>(samples[i]) * samples[i];
    }
    mCurrentPeak.store(peak);
    mCurrentRms.store(static_cast<float>(
        std::sqrt(sumSquares / static_cast<double>(sampleCount))));
    atomicMaxFloat(mPeakSinceLastRead, peak);
    atomicMaxFloat(mHeldPeak, peak);
    mWriter.enqueue(samples, frameCount);
    mFrameCount.fetch_add(frameCount);
}

PlayerErrors CaptureSession::start(
    const std::string &filePath, unsigned int sampleRate,
    unsigned int channels, unsigned int bufferSizeFrames, float inputGainDb,
    int captureDeviceID, const std::string &mirrorFilePath,
    unsigned int mirrorFormat, unsigned int mirrorBitsPerSample,
    CaptureStartInfo *info)
{
    if (!mPlayer.mInited)
        return backendNotInited;
    if (mRecording)
        return playerAlreadyInited;
    if (filePath.empty() || sampleRate == 0 || channels == 0 || channels > 2 ||
        bufferSizeFrames == 0 || info == nullptr)
        return invalidParameter;

    FILE *file = fopen(filePath.c_str(), "wb");
    if (file == nullptr)
        return fileLoadFailed;
    if (!writeWavHeader(file, sampleRate, channels))
    {
        fclose(file);
        remove(filePath.c_str());
        return fileLoadFailed;
    }

    ma_device_config config = ma_device_config_init(ma_device_type_capture);
    config.capture.format = ma_format_f32;
    config.capture.channels = channels;
    config.sampleRate = sampleRate;
    config.periodSizeInFrames = bufferSizeFrames;
    config.dataCallback = dataCallback;
    config.pUserData = this;
    ma_device_id selectedId;
    if (captureDeviceID >= 0)
    {
        auto devices = listDevices();
        if (captureDeviceID >= static_cast<int>(devices.size()))
        {
            for (auto &device : devices)
                free(device.name);
            fclose(file);
            remove(filePath.c_str());
            return invalidParameter;
        }
        selectedId = devices[captureDeviceID].deviceId;
        for (auto &device : devices)
            free(device.name);
        config.capture.pDeviceID = &selectedId;
    }

#if defined(MA_HAS_COREAUDIO) || defined(__ANDROID__)
    const ma_result initResult =
        ma_device_init(&SoLoud::context, &config, &mDevice);
#else
    const ma_result initResult = ma_device_init(nullptr, &config, &mDevice);
#endif
    if (initResult != MA_SUCCESS)
    {
        fclose(file);
        remove(filePath.c_str());
        return unknownError;
    }

    mDeviceInitialized = true;
    mFile = file;
    mFilePath = filePath;
    mSampleRate = mDevice.sampleRate;
    mChannels = mDevice.capture.channels;
    mInputGain = std::pow(10.0f, inputGainDb / 20.0f);
    if (std::fabs(mInputGain - 1.0f) >= 0.0001f)
        mGainBuffer.resize(static_cast<size_t>(bufferSizeFrames) * channels);
    mMirror.prepare(mirrorFilePath, mirrorFormat, mirrorBitsPerSample,
                    mSampleRate, mChannels);
    if (!mWriter.start(file, mSampleRate, mChannels, bufferSizeFrames,
                       &mMirror))
    {
        ma_device_uninit(&mDevice);
        mMirror.finish(true);
        fclose(file);
        remove(filePath.c_str());
        reset();
        return unknownError;
    }

    mSessionStartHostTimeNanos = nowHostTimeNanos();
    mRecording = true;
    if (ma_device_start(&mDevice) != MA_SUCCESS)
    {
        mRecording = false;
        ma_device_uninit(&mDevice);
        mWriter.stop();
        mMirror.finish(true);
        fclose(file);
        remove(filePath.c_str());
        reset();
        return unknownError;
    }

    mStartHostTimeNanos = nowHostTimeNanos();
    info->sampleRate = mSampleRate;
    info->channels = mChannels;
    info->sessionStartHostTimeNanos = mSessionStartHostTimeNanos;
    info->captureStartHostTimeNanos = mStartHostTimeNanos;
    info->mirrorFormat = mMirror.format();
    info->mirrorActive = mMirror.active() && !mMirror.failed();
    return noError;
}

PlayerErrors CaptureSession::startAndPlay(
    const std::string &filePath, unsigned int soundHash, unsigned int busId,
    unsigned int sampleRate, unsigned int channels,
    unsigned int bufferSizeFrames, float volume, float pan,
    double startAtSeconds, bool looping, double loopingStartAt,
    float inputGainDb, int captureDeviceID,
    const std::string &mirrorFilePath, unsigned int mirrorFormat,
    unsigned int mirrorBitsPerSample, CapturePlaybackStartInfo *info)
{
    if (info == nullptr)
        return nullPointer;
    if (startAtSeconds < 0 || loopingStartAt < 0)
        return invalidParameter;
    CaptureStartInfo captureInfo;
    PlayerErrors result =
        start(filePath, sampleRate, channels, bufferSizeFrames, inputGainDb,
              captureDeviceID, mirrorFilePath, mirrorFormat,
              mirrorBitsPerSample, &captureInfo);
    if (result != noError)
        return result;
    unsigned int handle = 0;
    result = mPlayer.play(soundHash, handle, busId, volume, pan, true, looping,
                          loopingStartAt);
    if (result != noError)
    {
        cancel();
        return result;
    }
    if (startAtSeconds > 0)
    {
        result = mPlayer.seek(handle, static_cast<float>(startAtSeconds));
        if (result != noError)
        {
            mPlayer.stop(handle);
            cancel();
            return result;
        }
    }
    mPlayer.setPause(handle, false);
    info->handle = handle;
    info->sampleRate = captureInfo.sampleRate;
    info->channels = captureInfo.channels;
    info->sessionStartHostTimeNanos =
        captureInfo.sessionStartHostTimeNanos;
    info->captureStartHostTimeNanos =
        captureInfo.captureStartHostTimeNanos;
    info->playbackStartHostTimeNanos = nowHostTimeNanos();
    info->mirrorFormat = captureInfo.mirrorFormat;
    info->mirrorActive = captureInfo.mirrorActive;
    return noError;
}

PlayerErrors CaptureSession::stop(CaptureStopInfo *info)
{
    if (!mRecording || !mDeviceInitialized || mFile == nullptr)
        return invalidParameter;
    if (info == nullptr)
        return nullPointer;

    const bool deviceStopFailed = ma_device_stop(&mDevice) != MA_SUCCESS;
    const uint64_t stopTime = nowHostTimeNanos();
    ma_device_uninit(&mDevice);
    mWriter.stop();
    const CaptureWriterStats writerStats = mWriter.stats();
    const uint64_t dataBytes =
        writerStats.frameCount * mChannels * sizeof(float);
    const unsigned int mirrorFormat = mMirror.format();
    const uint64_t mirrorFrames = mMirror.frameCount();
    bool mirrorSucceeded =
        mirrorFormat != captureMirrorNone && mMirror.active() &&
        !mMirror.failed() && mirrorFrames == writerStats.frameCount;
    if (mirrorFormat != captureMirrorNone)
        mirrorSucceeded =
            mMirror.finish(!mirrorSucceeded) && mirrorSucceeded;
    const bool wavFinalized = finalizeWav(mFile, dataBytes);
    const bool wavClosed = fclose(mFile) == 0;

    info->sampleRate = mSampleRate;
    info->channels = mChannels;
    info->frameCount = writerStats.frameCount;
    info->sessionStartHostTimeNanos = mSessionStartHostTimeNanos;
    info->captureStartHostTimeNanos = mStartHostTimeNanos;
    info->firstInputBufferHostTimeNanos =
        mFirstInputHostTimeNanos.load();
    info->firstInputBufferFrameIndex = mFirstInputFrameIndex.load();
    info->captureStopHostTimeNanos = stopTime;
    info->mirrorFormat = mirrorFormat;
    info->mirrorSucceeded = mirrorSucceeded;
    info->mirrorFrameCount = mirrorFrames;
    info->writerOverflowFrames = writerStats.overflowFrames;
    info->writerSilenceFrames = writerStats.silenceFrames;
    info->writerFailed = writerStats.failed || deviceStopFailed ||
                         !wavFinalized || !wavClosed;
    reset();
    return noError;
}

PlayerErrors CaptureSession::cancel()
{
    if (!mRecording && !mDeviceInitialized && mFile == nullptr)
        return noError;
    const std::string path = mFilePath;
    if (mDeviceInitialized)
    {
        ma_device_stop(&mDevice);
        ma_device_uninit(&mDevice);
    }
    mWriter.stop();
    if (mFile != nullptr)
        fclose(mFile);
    mMirror.finish(true);
    reset();
    if (!path.empty())
        remove(path.c_str());
    return noError;
}

bool CaptureSession::isRecording() const
{
    return mRecording;
}

PlayerErrors CaptureSession::clockSnapshot(CaptureClockInfo *info) const
{
    if (!mRecording || info == nullptr)
        return invalidParameter;
    info->hostTimeNanos = nowHostTimeNanos();
    info->sessionStartHostTimeNanos = mSessionStartHostTimeNanos;
    info->sampleRate = mSampleRate;
    info->inputDeviceFrame = mFrameCount.load();
    return noError;
}

PlayerErrors CaptureSession::levelSnapshot(CaptureLevelInfo *info)
{
    if (!mRecording || info == nullptr)
        return invalidParameter;
    const float peak = mCurrentPeak.load();
    const float intervalPeak = mPeakSinceLastRead.exchange(0.0f);
    info->currentPeak = peak;
    info->currentRms = mCurrentRms.load();
    info->peakSinceLastRead = intervalPeak > 0.0f ? intervalPeak : peak;
    info->heldPeak = mHeldPeak.load();
    info->frameCount = mFrameCount.load();
    return noError;
}

void CaptureSession::reset()
{
    mWriter.stop();
    mDeviceInitialized = false;
    mRecording = false;
    mFile = nullptr;
    mFilePath.clear();
    mSampleRate = 0;
    mChannels = 0;
    mSessionStartHostTimeNanos = 0;
    mStartHostTimeNanos = 0;
    mInputGain = 1.0f;
    mGainBuffer.clear();
    mFrameCount.store(0);
    mFirstInputHostTimeNanos.store(0);
    mFirstInputFrameIndex.store(0);
    mCurrentPeak.store(0.0f);
    mCurrentRms.store(0.0f);
    mPeakSinceLastRead.store(0.0f);
    mHeldPeak.store(0.0f);
    mMirror.reset();
}

std::vector<CaptureDevice> Player::listCaptureDevices()
{
    return CaptureSession::listDevices();
}

PlayerErrors Player::startCapture(
    const std::string &filePath, unsigned int sampleRate,
    unsigned int channels, unsigned int bufferSizeFrames, float inputGainDb,
    int captureDeviceID, const std::string &mirrorFilePath,
    unsigned int mirrorFormat, unsigned int mirrorBitsPerSample,
    CaptureStartInfo *info)
{
    return mCaptureSession->start(
        filePath, sampleRate, channels, bufferSizeFrames, inputGainDb,
        captureDeviceID, mirrorFilePath, mirrorFormat, mirrorBitsPerSample,
        info);
}

PlayerErrors Player::startCaptureAndPlay(
    const std::string &filePath, unsigned int soundHash, unsigned int busId,
    unsigned int sampleRate, unsigned int channels,
    unsigned int bufferSizeFrames, float volume, float pan,
    double startAtSeconds, bool looping, double loopingStartAt,
    float inputGainDb, int captureDeviceID,
    const std::string &mirrorFilePath, unsigned int mirrorFormat,
    unsigned int mirrorBitsPerSample, CapturePlaybackStartInfo *info)
{
    return mCaptureSession->startAndPlay(
        filePath, soundHash, busId, sampleRate, channels, bufferSizeFrames,
        volume, pan, startAtSeconds, looping, loopingStartAt, inputGainDb,
        captureDeviceID, mirrorFilePath, mirrorFormat, mirrorBitsPerSample,
        info);
}

PlayerErrors Player::stopCapture(CaptureStopInfo *info)
{
    return mCaptureSession->stop(info);
}

PlayerErrors Player::cancelCapture()
{
    return mCaptureSession->cancel();
}

bool Player::isCaptureRecording() const
{
    return mCaptureSession->isRecording();
}

PlayerErrors Player::getCaptureClockSnapshot(CaptureClockInfo *info) const
{
    return mCaptureSession->clockSnapshot(info);
}

PlayerErrors Player::getCaptureLevelSnapshot(CaptureLevelInfo *info)
{
    return mCaptureSession->levelSnapshot(info);
}
