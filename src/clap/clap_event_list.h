// Fixed-capacity CLAP event lists for one plugin instance (plan round-1
// amendment 1). Events are copied into preallocated aligned slots, so pushing
// never allocates and the pointers handed to the plugin stay valid for the
// whole process()/flush() call.
//
// Input order is kept sorted by time as events are pushed (stable for equal
// times), so process() always sees sample-sorted input. Pointer-bearing events
// (SysEx) and events outside the core namespace are rejected in v1.
#pragma once

#ifdef SOLOUD_ENABLE_CLAP

#include <clap/events.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

namespace soloud_clap
{
    /// Whether [header] is an event the host carries: core namespace, a known
    /// type, not SysEx, and a size that fits [maxSize].
    inline bool isCarriedEvent(const clap_event_header_t *header,
                               size_t maxSize) noexcept
    {
        if (header == nullptr || header->space_id != CLAP_CORE_EVENT_SPACE_ID)
            return false;
        if (header->size < sizeof(clap_event_header_t) || header->size > maxSize)
            return false;
        switch (header->type)
        {
        case CLAP_EVENT_NOTE_ON:
        case CLAP_EVENT_NOTE_OFF:
        case CLAP_EVENT_NOTE_CHOKE:
        case CLAP_EVENT_NOTE_END:
        case CLAP_EVENT_NOTE_EXPRESSION:
        case CLAP_EVENT_PARAM_VALUE:
        case CLAP_EVENT_PARAM_MOD:
        case CLAP_EVENT_PARAM_GESTURE_BEGIN:
        case CLAP_EVENT_PARAM_GESTURE_END:
        case CLAP_EVENT_TRANSPORT:
        case CLAP_EVENT_MIDI:
        case CLAP_EVENT_MIDI2:
            return true;
        default:
            // Includes CLAP_EVENT_MIDI_SYSEX, whose buffer pointer would have to
            // outlive the call.
            return false;
        }
    }

    /// Storage shared by both lists: [kCapacity] slots of [kSlotSize] bytes.
    class EventSlots
    {
    public:
        static constexpr uint32_t kCapacity = 1024;
        /// Fits every carried core event (the transport event is the largest).
        static constexpr size_t kSlotSize = 192;

        EventSlots() : mSlots(new Slot[kCapacity]) {}

        void clear() noexcept { mCount = 0; }
        uint32_t size() const noexcept { return mCount; }

        /// Copies [header] into the next slot; null when full or not carried.
        clap_event_header_t *push(const clap_event_header_t *header) noexcept
        {
            if (mCount >= kCapacity || !isCarriedEvent(header, kSlotSize))
                return nullptr;
            Slot &slot = mSlots[mCount++];
            std::memcpy(slot.bytes, header, header->size);
            return reinterpret_cast<clap_event_header_t *>(slot.bytes);
        }

        const clap_event_header_t *slot(uint32_t index) const noexcept
        {
            return reinterpret_cast<const clap_event_header_t *>(
                mSlots[index].bytes);
        }

    private:
        struct Slot
        {
            alignas(alignof(std::max_align_t)) unsigned char bytes[kSlotSize];
        };
        std::unique_ptr<Slot[]> mSlots;
        uint32_t mCount = 0;
    };

    /// Host -> plugin events for one call, sorted by time.
    class InputEventList
    {
    public:
        static constexpr uint32_t kCapacity = EventSlots::kCapacity;

        InputEventList()
        {
            mVtable.ctx = this;
            mVtable.size = &InputEventList::sizeThunk;
            mVtable.get = &InputEventList::getThunk;
        }
        InputEventList(const InputEventList &) = delete;
        InputEventList &operator=(const InputEventList &) = delete;

        void clear() noexcept { mSlots.clear(); }

        /// Copies [header] in, keeping time order (after equal times). False
        /// when the list is full or the event is not carried.
        bool tryPush(const clap_event_header_t *header) noexcept
        {
            const uint32_t index = mSlots.size();
            if (mSlots.push(header) == nullptr)
                return false;
            const uint32_t time = header->time;
            uint32_t pos = index;
            while (pos > 0 && mSlots.slot(mOrder[pos - 1])->time > time)
            {
                mOrder[pos] = mOrder[pos - 1];
                --pos;
            }
            mOrder[pos] = static_cast<uint16_t>(index);
            return true;
        }

        uint32_t size() const noexcept { return mSlots.size(); }

        const clap_event_header_t *get(uint32_t index) const noexcept
        {
            return index < mSlots.size() ? mSlots.slot(mOrder[index]) : nullptr;
        }

        const clap_input_events_t *vtable() const noexcept { return &mVtable; }

    private:
        static uint32_t CLAP_ABI sizeThunk(const clap_input_events_t *list)
        {
            return static_cast<const InputEventList *>(list->ctx)->size();
        }
        static const clap_event_header_t *CLAP_ABI getThunk(
            const clap_input_events_t *list, uint32_t index)
        {
            return static_cast<const InputEventList *>(list->ctx)->get(index);
        }

        EventSlots mSlots;
        uint16_t mOrder[kCapacity] = {};
        clap_input_events_t mVtable{};
    };

    static_assert(EventSlots::kCapacity <= 65536,
                  "InputEventList indexes slots with uint16_t");

    /// Plugin -> host events for one call, in push order.
    class OutputEventList
    {
    public:
        static constexpr uint32_t kCapacity = EventSlots::kCapacity;

        OutputEventList()
        {
            mVtable.ctx = this;
            mVtable.try_push = &OutputEventList::tryPushThunk;
        }
        OutputEventList(const OutputEventList &) = delete;
        OutputEventList &operator=(const OutputEventList &) = delete;

        void clear() noexcept
        {
            mSlots.clear();
            mDropped = 0;
        }

        bool tryPush(const clap_event_header_t *header) noexcept
        {
            if (mSlots.push(header) != nullptr)
                return true;
            ++mDropped;
            return false;
        }

        uint32_t size() const noexcept { return mSlots.size(); }
        const clap_event_header_t *get(uint32_t index) const noexcept
        {
            return index < mSlots.size() ? mSlots.slot(index) : nullptr;
        }
        /// Events refused since the last clear().
        uint32_t dropped() const noexcept { return mDropped; }

        const clap_output_events_t *vtable() const noexcept { return &mVtable; }

    private:
        static bool CLAP_ABI tryPushThunk(const clap_output_events_t *list,
                                          const clap_event_header_t *event)
        {
            return static_cast<OutputEventList *>(list->ctx)->tryPush(event);
        }

        EventSlots mSlots;
        uint32_t mDropped = 0;
        clap_output_events_t mVtable{};
    };
} // namespace soloud_clap

#endif // SOLOUD_ENABLE_CLAP
