#include "capture_writer.h"

#include <algorithm>
#include <chrono>
#include <cstring>

CaptureWriter::~CaptureWriter()
{
    stop();
}

bool CaptureWriter::start(FILE *file, unsigned int sampleRate,
                          unsigned int channels,
                          unsigned int bufferSizeFrames,
                          CaptureMirror *mirror)
{
    const uint64_t capacityFrames = std::max<uint64_t>(
        static_cast<uint64_t>(bufferSizeFrames) * 64,
        static_cast<uint64_t>(sampleRate) * 2);
    return initialize(file, channels, capacityFrames,
                      std::max(bufferSizeFrames, 4096u), mirror, true);
}

bool CaptureWriter::initialize(FILE *file, unsigned int channels,
                               uint64_t capacityFrames,
                               unsigned int silenceChunkFrames,
                               CaptureMirror *mirror, bool startThread)
{
    stop();
    if (file == nullptr || channels == 0 || capacityFrames == 0)
        return false;
    mFile = file;
    mChannels = channels;
    mMirror = mirror;
    mCapacitySamples = capacityFrames * channels;
    try
    {
        mRing.assign(static_cast<size_t>(mCapacitySamples), 0.0f);
        mSilence.assign(
            static_cast<size_t>(silenceChunkFrames) * channels, 0.0f);
        mReadSample.store(0);
        mWriteSample.store(0);
        mStopRequested.store(false);
        mFailed.store(false);
        mWrittenFrames.store(0);
        mOverflowFrames.store(0);
        mSilenceFrames.store(0);
        {
            std::lock_guard<std::mutex> lock(mOverflowMutex);
            mDropUntilSilenceDrained = false;
            mPendingSilenceFrames = 0;
        }
        if (startThread)
            mThread = std::thread(&CaptureWriter::writerLoop, this);
    }
    catch (...)
    {
        mRing.clear();
        mSilence.clear();
        mCapacitySamples = 0;
        return false;
    }
    return true;
}

void CaptureWriter::stop()
{
    mStopRequested.store(true);
    mCondition.notify_one();
    if (mThread.joinable())
        mThread.join();
}

bool CaptureWriter::enqueue(const float *samples, ma_uint32 frameCount)
{
    if (samples == nullptr || frameCount == 0 || mChannels == 0 ||
        mCapacitySamples == 0 || mRing.empty())
        return false;

    const uint64_t sampleCount =
        static_cast<uint64_t>(frameCount) * mChannels;
    std::lock_guard<std::mutex> overflowLock(mOverflowMutex);
    if (mDropUntilSilenceDrained)
    {
        mOverflowFrames.fetch_add(frameCount);
        mPendingSilenceFrames += frameCount;
        mCondition.notify_one();
        return false;
    }

    const uint64_t read = mReadSample.load(std::memory_order_acquire);
    const uint64_t write = mWriteSample.load(std::memory_order_relaxed);
    const uint64_t used = write - read;
    const uint64_t freeSamples =
        used >= mCapacitySamples ? 0 : mCapacitySamples - used;
    if (sampleCount > freeSamples)
    {
        mDropUntilSilenceDrained = true;
        mOverflowFrames.fetch_add(frameCount);
        mPendingSilenceFrames += frameCount;
        mCondition.notify_one();
        return false;
    }

    const uint64_t index = write % mCapacitySamples;
    const uint64_t first =
        std::min(sampleCount, mCapacitySamples - index);
    memcpy(&mRing[static_cast<size_t>(index)], samples,
           static_cast<size_t>(first) * sizeof(float));
    if (sampleCount > first)
        memcpy(mRing.data(), samples + first,
               static_cast<size_t>(sampleCount - first) * sizeof(float));
    mWriteSample.store(write + sampleCount, std::memory_order_release);
    mCondition.notify_one();
    return true;
}

bool CaptureWriter::drainAvailable()
{
    const uint64_t read = mReadSample.load(std::memory_order_relaxed);
    const uint64_t write = mWriteSample.load(std::memory_order_acquire);
    uint64_t samples = write - read;
    if (samples == 0 || mChannels == 0 || mCapacitySamples == 0)
        return false;
    const uint64_t index = read % mCapacitySamples;
    samples = std::min(samples, mCapacitySamples - index);
    samples -= samples % mChannels;
    if (samples == 0)
    {
        mReadSample.store(write, std::memory_order_release);
        return true;
    }
    writeFrames(&mRing[static_cast<size_t>(index)],
                static_cast<ma_uint32>(samples / mChannels));
    mReadSample.store(read + samples, std::memory_order_release);
    return true;
}

bool CaptureWriter::drainSilence()
{
    uint64_t pending = 0;
    {
        std::lock_guard<std::mutex> lock(mOverflowMutex);
        if (!mDropUntilSilenceDrained || mPendingSilenceFrames == 0)
            return false;
        pending = mPendingSilenceFrames;
        mPendingSilenceFrames = 0;
    }

    const ma_uint32 chunkCapacity = static_cast<ma_uint32>(
        std::max<uint64_t>(1, mSilence.size() / mChannels));
    uint64_t written = 0;
    while (written < pending)
    {
        const auto chunk = static_cast<ma_uint32>(
            std::min<uint64_t>(pending - written, chunkCapacity));
        if (!writeFrames(mSilence.data(), chunk))
            break;
        mSilenceFrames.fetch_add(chunk);
        written += chunk;
    }

    {
        std::lock_guard<std::mutex> lock(mOverflowMutex);
        if (written < pending && !mFailed.load())
            mPendingSilenceFrames += pending - written;
        if (mFailed.load())
            mPendingSilenceFrames = 0;
        if (mPendingSilenceFrames == 0)
            mDropUntilSilenceDrained = false;
    }
    return true;
}

void CaptureWriter::writerLoop()
{
    while (true)
    {
        if (drainAvailable())
            continue;
        if (drainSilence())
            continue;
        if (mStopRequested.load(std::memory_order_acquire))
            break;
        std::unique_lock<std::mutex> lock(mConditionMutex);
        mCondition.wait_for(lock, std::chrono::milliseconds(5));
    }
}

bool CaptureWriter::writeFrames(const float *samples, ma_uint32 frameCount)
{
    const size_t bytes =
        static_cast<size_t>(frameCount) * mChannels * sizeof(float);
    if (mFile == nullptr || fwrite(samples, 1, bytes, mFile) != bytes)
    {
        mFailed.store(true);
        return false;
    }
    if (mMirror != nullptr && mMirror->active() &&
        !mMirror->encode(samples, frameCount))
        mMirror->finish(true);
    mWrittenFrames.fetch_add(frameCount);
    return true;
}

CaptureWriterStats CaptureWriter::stats() const
{
    return {
        mWrittenFrames.load(),
        mOverflowFrames.load(),
        mSilenceFrames.load(),
        mFailed.load(),
    };
}

#if defined(FLUTTER_SOLOUD_CAPTURE_TESTING)
bool CaptureWriter::startForTesting(FILE *file, unsigned int channels,
                                    uint64_t capacityFrames,
                                    CaptureMirror *mirror)
{
    return initialize(file, channels, capacityFrames, 16, mirror, false);
}

void CaptureWriter::drainForTesting()
{
    while (drainAvailable() || drainSilence())
    {
    }
}
#endif
