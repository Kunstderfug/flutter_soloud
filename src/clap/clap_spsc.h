// A fixed-capacity single-producer single-consumer ring (plan Phase 3).
//
// Positions are free-running 64-bit counters; an element lives at
// `pos % capacity`. A boundary the consumer records is therefore always a
// position, never a wrapped index, and cannot be confused across laps.
//
// The producer and the consumer are each serialised by their caller (a mutex
// per side). Any thread may read the positions (acquire) to test for work.
// Storage is allocated once, in the constructor: pushing and popping never
// allocate.
#pragma once

#ifdef SOLOUD_ENABLE_CLAP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace soloud_clap
{
    template <typename T>
    class SpscRing
    {
    public:
        explicit SpscRing(size_t capacity)
            : mCapacity(capacity > 0 ? capacity : 1),
              mItems(new T[capacity > 0 ? capacity : 1]())
        {
        }
        SpscRing(const SpscRing &) = delete;
        SpscRing &operator=(const SpscRing &) = delete;

        size_t capacity() const noexcept { return mCapacity; }

        // --- producer -------------------------------------------------------
        /// False when the ring is full.
        bool tryPush(const T &item) noexcept
        {
            const uint64_t write = mWrite.load(std::memory_order_relaxed);
            const uint64_t read = mRead.load(std::memory_order_acquire);
            if (write - read >= mCapacity)
                return false;
            mItems[write % mCapacity] = item;
            mWrite.store(write + 1, std::memory_order_release);
            return true;
        }

        // --- consumer -------------------------------------------------------
        /// The oldest element, or null when the ring is empty.
        const T *peek() const noexcept
        {
            const uint64_t read = mRead.load(std::memory_order_relaxed);
            if (read == mWrite.load(std::memory_order_acquire))
                return nullptr;
            return &mItems[read % mCapacity];
        }
        /// The element at [pos] (readPos() <= pos < writePos()).
        const T &at(uint64_t pos) const noexcept
        {
            return mItems[pos % mCapacity];
        }
        /// Drops the oldest element; the ring must not be empty.
        void pop() noexcept
        {
            mRead.store(mRead.load(std::memory_order_relaxed) + 1,
                        std::memory_order_release);
        }

        // --- any thread ----------------------------------------------------
        uint64_t writePos() const noexcept
        {
            return mWrite.load(std::memory_order_acquire);
        }
        uint64_t readPos() const noexcept
        {
            return mRead.load(std::memory_order_acquire);
        }
        /// Elements written and not yet popped (a snapshot).
        uint64_t readable() const noexcept
        {
            const uint64_t read = mRead.load(std::memory_order_acquire);
            return mWrite.load(std::memory_order_acquire) - read;
        }
        bool empty() const noexcept { return readable() == 0; }

    private:
        const size_t mCapacity;
        std::unique_ptr<T[]> mItems;
        alignas(64) std::atomic<uint64_t> mWrite{0};
        alignas(64) std::atomic<uint64_t> mRead{0};
    };
} // namespace soloud_clap

#endif // SOLOUD_ENABLE_CLAP
