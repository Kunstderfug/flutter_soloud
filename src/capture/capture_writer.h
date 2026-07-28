#pragma once

#include "capture_mirror.h"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

struct CaptureWriterStats
{
    uint64_t frameCount = 0;
    uint64_t overflowFrames = 0;
    uint64_t silenceFrames = 0;
    bool failed = false;
};

class CaptureWriter
{
public:
    ~CaptureWriter();

    bool start(FILE *file, unsigned int sampleRate, unsigned int channels,
               unsigned int bufferSizeFrames, CaptureMirror *mirror);
    void stop();
    bool enqueue(const float *samples, ma_uint32 frameCount);
    CaptureWriterStats stats() const;

#if defined(FLUTTER_SOLOUD_CAPTURE_TESTING)
    bool startForTesting(FILE *file, unsigned int channels,
                         uint64_t capacityFrames, CaptureMirror *mirror);
    void drainForTesting();
#endif

private:
    bool initialize(FILE *file, unsigned int channels,
                    uint64_t capacityFrames, unsigned int silenceChunkFrames,
                    CaptureMirror *mirror, bool startThread);
    void writerLoop();
    bool drainAvailable();
    bool drainSilence();
    bool writeFrames(const float *samples, ma_uint32 frameCount);

    FILE *mFile = nullptr;
    unsigned int mChannels = 0;
    CaptureMirror *mMirror = nullptr;
    std::thread mThread;
    std::mutex mConditionMutex;
    std::condition_variable mCondition;
    std::vector<float> mRing;
    std::vector<float> mSilence;
    uint64_t mCapacitySamples = 0;
    std::atomic<uint64_t> mReadSample{0};
    std::atomic<uint64_t> mWriteSample{0};
    std::atomic<bool> mStopRequested{false};
    std::atomic<bool> mFailed{false};
    std::atomic<uint64_t> mWrittenFrames{0};
    std::atomic<uint64_t> mOverflowFrames{0};
    std::atomic<uint64_t> mSilenceFrames{0};

    // Protects the overflow epoch. Once an overflow happens, later buffers are
    // dropped until the writer has emitted equivalent silence. This preserves
    // capture chronology without blocking the realtime callback on disk I/O.
    std::mutex mOverflowMutex;
    bool mDropUntilSilenceDrained = false;
    uint64_t mPendingSilenceFrames = 0;
};
