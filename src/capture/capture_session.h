#pragma once

#include "capture_models.h"
#include "capture_mirror.h"
#include "capture_writer.h"
#include "enums.h"

#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

class Player;

class CaptureSession
{
public:
    explicit CaptureSession(Player &player);
    ~CaptureSession();

    static std::vector<CaptureDevice> listDevices();
    PlayerErrors start(const std::string &filePath, unsigned int sampleRate,
                       unsigned int channels, unsigned int bufferSizeFrames,
                       float inputGainDb, int captureDeviceID,
                       const std::string &mirrorFilePath,
                       unsigned int mirrorFormat,
                       unsigned int mirrorBitsPerSample,
                       CaptureStartInfo *info);
    PlayerErrors startAndPlay(const std::string &filePath,
                              unsigned int soundHash, unsigned int busId,
                              unsigned int sampleRate, unsigned int channels,
                              unsigned int bufferSizeFrames, float volume,
                              float pan, double startAtSeconds, bool looping,
                              double loopingStartAt, float inputGainDb,
                              int captureDeviceID,
                              const std::string &mirrorFilePath,
                              unsigned int mirrorFormat,
                              unsigned int mirrorBitsPerSample,
                              CapturePlaybackStartInfo *info);
    PlayerErrors stop(CaptureStopInfo *info);
    PlayerErrors cancel();
    bool isRecording() const;
    PlayerErrors clockSnapshot(CaptureClockInfo *info) const;
    PlayerErrors levelSnapshot(CaptureLevelInfo *info);

private:
    static void dataCallback(ma_device *device, void *output,
                             const void *input, ma_uint32 frameCount);
    static uint64_t nowHostTimeNanos();
    static void atomicMaxFloat(std::atomic<float> &target, float value);
    static bool writeWavHeader(FILE *file, unsigned int sampleRate,
                               unsigned int channels);
    static bool finalizeWav(FILE *file, uint64_t dataSizeBytes);
    void handleFrames(const void *input, ma_uint32 frameCount);
    void reset();

    Player &mPlayer;
    ma_device mDevice{};
    bool mDeviceInitialized = false;
    bool mRecording = false;
    FILE *mFile = nullptr;
    std::string mFilePath;
    unsigned int mSampleRate = 0;
    unsigned int mChannels = 0;
    uint64_t mSessionStartHostTimeNanos = 0;
    uint64_t mStartHostTimeNanos = 0;
    float mInputGain = 1.0f;
    std::vector<float> mGainBuffer;
    CaptureMirror mMirror;
    CaptureWriter mWriter;
    std::atomic<uint64_t> mFrameCount{0};
    std::atomic<uint64_t> mFirstInputHostTimeNanos{0};
    std::atomic<uint64_t> mFirstInputFrameIndex{0};
    std::atomic<float> mCurrentPeak{0.0f};
    std::atomic<float> mCurrentRms{0.0f};
    std::atomic<float> mPeakSinceLastRead{0.0f};
    std::atomic<float> mHeldPeak{0.0f};
};
