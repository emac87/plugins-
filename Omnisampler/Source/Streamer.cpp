#include "Streamer.h"

namespace omni
{
    using namespace juce;

    Streamer::Streamer() : Thread ("Omnisampler disk streaming")
    {
        formats.registerBasicFormats();
        temp.setSize (2, kChunk);
        startThread (Thread::Priority::high);
    }

    Streamer::~Streamer()
    {
        signalThreadShouldExit();
        notify();
        stopThread (4000);
    }

    int Streamer::open (const SamplePtr& s, int start, bool loop, int loopStart, int loopEnd) noexcept
    {
        if (s == nullptr || ! s->streamed) return -1;
        const int head = s->data.getNumSamples();
        loop = loop && loopEnd > loopStart + 4 && loopEnd <= s->totalFrames;
        if (loop && loopEnd <= head) return -1;            // the loop never leaves the head
        if (! loop && start + 1 >= s->totalFrames) return -1;
        for (int i = 0; i < kSlots; ++i)
        {
            auto& sl = slots[i];
            int expected = Idle;
            // Only this (audio) thread moves Idle -> Requested, so a plain check then store is enough,
            // but compare_exchange keeps it obviously safe.
            if (sl.state.load (std::memory_order_acquire) != Idle) continue;
            sl.sample = s;                                  // refcount bump only; frees happen on the reader thread
            sl.start = start; sl.loop = loop; sl.loopStart = loopStart; sl.loopEnd = loopEnd;
            sl.first = start >= head ? 0 : (int64_t) (head - start);
            sl.written.store (sl.first, std::memory_order_relaxed);
            sl.readPos.store (0, std::memory_order_relaxed);
            if (sl.state.compare_exchange_strong (expected, Requested, std::memory_order_acq_rel))
            {
                notify();
                return i;
            }
        }
        return -1;
    }

    void Streamer::close (int slot) noexcept
    {
        if (slot < 0 || slot >= kSlots) return;
        slots[slot].state.store (Closing, std::memory_order_release);
        notify();
    }

    bool Streamer::read (int slot, int64_t logical, int ch, float& out) const noexcept
    {
        const auto& sl = slots[slot];
        if (sl.state.load (std::memory_order_acquire) != Active) return false;
        const int64_t w = sl.written.load (std::memory_order_acquire);
        if (logical < sl.first || logical >= w || logical < w - kRing) return false;
        const int idx = (int) ((logical - sl.first) % kRing);
        out = sl.ring.getSample (jmin (ch, sl.ring.getNumChannels() - 1), idx);
        return true;
    }

    // Reader thread: reads up to one chunk into a slot's ring. Returns true if it read anything.
    bool Streamer::fill (int i)
    {
        auto& sl = slots[i];
        const auto& s = *sl.sample;
        const int64_t w = sl.written.load (std::memory_order_relaxed);
        const int64_t rp = jmax (sl.first, sl.readPos.load (std::memory_order_acquire));
        int n = (int) jmin<int64_t> (kChunk, (int64_t) kRing - kChunk / 2 - (w - rp));
        if (n < kChunk / 4) return false;                   // ring full enough
        const int f = fileFrame (w, sl.start, sl.loop, sl.loopStart, sl.loopEnd);
        const int segEnd = sl.loop ? sl.loopEnd : s.totalFrames;
        n = jmin (n, segEnd - f);
        if (n <= 0) return false;                            // end of file
        if (sl.reader == nullptr || sl.readerFile != s.file)
        {
            sl.reader.reset (formats.createReaderFor (s.file));
            sl.readerFile = s.file;
            if (sl.reader == nullptr) return false;
        }
        sl.reader->read (&temp, 0, n, f, true, true);
        const int chans = sl.ring.getNumChannels();
        int idx = (int) ((w - sl.first) % kRing);
        for (int done = 0; done < n;)
        {
            const int part = jmin (n - done, kRing - idx);
            for (int c = 0; c < chans; ++c)
                sl.ring.copyFrom (c, idx, temp, jmin (c, temp.getNumChannels() - 1), done, part);
            done += part;
            idx = (idx + part) % kRing;
        }
        sl.written.store (w + n, std::memory_order_release);
        return true;
    }

    void Streamer::run()
    {
        while (! threadShouldExit())
        {
            bool busy = false;
            for (int i = 0; i < kSlots; ++i)
            {
                auto& sl = slots[i];
                const int st = sl.state.load (std::memory_order_acquire);
                if (st == Requested)
                {
                    const int chans = jmax (1, sl.sample->numChannels);
                    if (sl.ring.getNumChannels() != chans || sl.ring.getNumSamples() != kRing)
                        sl.ring.setSize (chans, kRing);
                    sl.state.store (Active, std::memory_order_release);
                    busy = fill (i) || busy;
                }
                else if (st == Active)
                {
                    busy = fill (i) || busy;
                }
                else if (st == Closing)
                {
                    sl.sample.reset();                         // may free the sample here, off the audio thread
                    sl.state.store (Idle, std::memory_order_release);
                }
            }
            if (! busy) wait (3);
        }
    }
}
