#pragma once
// Omnisampler voice engine: zone selection (velocity + round robin), voices, envelopes, filters, chokes.
#include "Model.h"
#include "DrumSynth.h"
#include "Streamer.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <random>

namespace omni
{
    // Cytomic TPT state-variable filter.
    struct Svf
    {
        float ic1 = 0, ic2 = 0, g = 0, k = 1, a1 = 0, a2 = 0, a3 = 0;
        void set (float cutoff, float res, double sr)
        {
            g = std::tan (juce::MathConstants<float>::pi * juce::jlimit (20.0f, (float) sr * 0.45f, cutoff) / (float) sr);
            k = 2.0f - 1.95f * juce::jlimit (0.0f, 1.0f, res);
            a1 = 1.0f / (1.0f + g * (g + k)); a2 = g * a1; a3 = g * a2;
        }
        float process (float v0, FilterType t) noexcept
        {
            const float v3 = v0 - ic2;
            const float v1 = a1 * ic1 + a2 * v3;
            const float v2 = ic2 + a2 * ic1 + a3 * v3;
            ic1 = 2 * v1 - ic1; ic2 = 2 * v2 - ic2;
            switch (t)
            {
                case FilterType::LowPass:  return v2;
                case FilterType::HighPass: return v0 - k * v1 - v2;
                case FilterType::BandPass: return v1;
                default:                   return v0;
            }
        }
        void reset() { ic1 = ic2 = 0; }
    };

    struct Voice
    {
        bool active = false, released = false, fromPad = false, oneShot = false;
        int note = -1, channel = 1, padIndex = -1, chokeGroup = 0;
        uint64_t age = 0;
        SamplePtr sample;
        Zone zone;
        const Voicing* voicing = nullptr;   // points into the instrument this voice was started from
        InstrumentPtr owner;                // keeps that instrument alive while the voice plays
        double pos = 0, inc = 1;
        float gainL = 1, gainR = 1;
        juce::ADSR env;
        Svf filt[2];
        float fastFade = 1.0f;              // choke / steal fade-out
        bool fading = false;
        int output = 0;                     // stereo output (0 = main)
        // Drum-synth layer voice (no sample).
        bool synth = false;
        DrumVoice drum;
        // Streamed sample: frames past the head come from the Streamer slot, read by logical position.
        int slot = -1;
        double lpos = 0;
    };

    class Engine
    {
    public:
        static constexpr int kVoices = 64;

        void prepare (double sr, int maxBlock = 4096);
        // Streaming: the processor owns the Streamer. offline = rendering a bounce (wait for the disk).
        void setStreamer (Streamer* s) { streamer = s; }
        void setOffline (bool o) { offline = o; }
        void setInstrument (InstrumentPtr p) { inst = std::move (p); }
        // MIDI from a controller or the host: the channel decides whether Pads, Keys or both hear it.
        void handleMidi (const juce::MidiMessage&, int padsChannel, int keysChannel);
        // Notes from the on-screen pads (pads = true) or the on-screen keyboard: that section only.
        void handleLocal (const juce::MidiMessage&, bool pads);
        // Which sections a channel reaches (0 = "Any"). A section on "Any" leaves the other section's own channel alone.
        static void route (int ch, int padsChannel, int keysChannel, bool& toPads, bool& toKeys) noexcept
        {
            toPads = ch == padsChannel || (padsChannel == 0 && (keysChannel == 0 || ch != keysChannel));
            toKeys = ch == keysChannel || (keysChannel == 0 && (padsChannel == 0 || ch != padsChannel));
        }
        // Adds voices into `out`. Output k goes to channels busChannel[k], busChannel[k] + 1
        // (busChannel[k] < 0 = that output is off: its voices go to the main output, busChannel[0]).
        void render (juce::AudioBuffer<float>& out, int start, int n, const int* busChannel, int numBuses);
        void render (juce::AudioBuffer<float>& out, int start, int n) { const int main = 0; render (out, start, n, &main, 1); }
        void allNotesOff();
        int activeVoices() const;
        int activeVoicesFrom (bool pads) const { int n = 0; for (auto& v : voices) if (v.active && ! v.fading && v.fromPad == pads) ++n; return n; }
        std::atomic<uint32_t> padHits[kPads] {};   // for the editor's pad flashes

    private:
        void noteOn (int ch, int note, int vel, bool toPads, bool toKeys);
        void noteOff (int ch, int note, bool pads, bool keys);
        const Zone* pickZone (const std::vector<Zone>& zones, int note, int vel, bool keys, RRMode mode, int counterIndex);
        Voice* startVoice (const Zone&, const Voicing&, int note, int vel, int ch, bool fromPad, int padIndex, int choke, bool oneShot, float levelDb, int output);
        void startSynth (const SynthLayer&, const Pad&, int note, int vel, int ch, int padIndex);
        Voice& freeVoice();
        void endVoice (Voice&);
        void renderSample (Voice&, float* L, float* R, int n);
        void renderStreamed (Voice&, float* L, float* R, int n);

        InstrumentPtr inst;
        Voice voices[kVoices];
        double sampleRate = 44100.0;
        uint64_t ageCounter = 0;
        int rrCounter[kPads + 128] {};      // per pad, then per key
        int rrLast[kPads + 128] {};
        std::minstd_rand rng { 12345 };
        std::vector<const Zone*> candidates;
        std::vector<float> scratch;
        Streamer* streamer = nullptr;
        bool offline = false;
    };
}
