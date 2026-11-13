#pragma once
// Disk streaming for long samples. A streamed Sample keeps its first frames in memory (the "head");
// when a voice needs frames past the head, a background thread reads them from disk into that
// voice's ring buffer. The voice reads by a "logical" frame index that counts every frame it has
// played (loops included), so the reader thread and the voice agree on the order of frames even
// when the zone loops.
//
// Threads: open()/close() and read() are called on the audio thread (lock-free); the reader thread
// does all file I/O and frees samples.
#include "Model.h"
#include <atomic>

namespace omni
{
    class Streamer : private juce::Thread
    {
    public:
        static constexpr int kSlots = 64;
        static constexpr int kRing = 1 << 16;      // frames per voice ring (~1.5 s at 44.1 kHz), allocated on first use
        static constexpr int kChunk = 4096;        // frames read per disk access

        Streamer();
        ~Streamer() override;

        // Audio thread. Starts streaming `s` for a voice that begins at file frame `start` and may loop.
        // Returns a slot index, or -1 if none is free or the zone never needs the disk.
        int open (const SamplePtr& s, int start, bool loop, int loopStart, int loopEnd) noexcept;
        void close (int slot) noexcept;

        // Audio thread. File frame for a logical frame (applies the loop).
        static int fileFrame (int64_t logical, int start, bool loop, int loopStart, int loopEnd) noexcept
        {
            int64_t f = start + logical;
            if (loop && f >= loopEnd && loopEnd > loopStart)
                f = loopStart + (f - loopStart) % (loopEnd - loopStart);
            return (int) f;
        }

        // Audio thread. Frame `logical` of channel `ch` from the ring; false if not there yet (underrun).
        bool read (int slot, int64_t logical, int ch, float& out) const noexcept;
        // Audio thread. Tells the reader how far the voice has got (frames before this can be overwritten).
        void setReadPos (int slot, int64_t logical) noexcept { slots[slot].readPos.store (logical, std::memory_order_release); }
        // For offline rendering: true once frames up to `logical` are in the ring.
        bool ready (int slot, int64_t logical) const noexcept { return slots[slot].written.load (std::memory_order_acquire) > logical; }
        void wake() noexcept { notify(); }

        std::atomic<int> underruns { 0 };

    private:
        void run() override;
        bool fill (int slot);

        enum State : int { Idle, Requested, Active, Closing };
        struct Slot
        {
            std::atomic<int> state { Idle };
            // Set by open() before state becomes Requested; read by the reader thread.
            SamplePtr sample;
            int start = 0, loopStart = 0, loopEnd = 0;
            bool loop = false;
            int64_t first = 0;                     // first logical frame the ring holds
            std::atomic<int64_t> written { 0 };    // logical frames [first, written) are in the ring
            std::atomic<int64_t> readPos { 0 };
            juce::AudioBuffer<float> ring;
            // Reader-thread only.
            std::unique_ptr<juce::AudioFormatReader> reader;
            juce::File readerFile;
        };
        Slot slots[kSlots];
        juce::AudioFormatManager formats;
        juce::AudioBuffer<float> temp;
    };
}
