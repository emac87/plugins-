#pragma once
// Omnisampler voice engine: zone selection (velocity + round robin), voices, envelopes, filters, chokes.
#include "Model.h"
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
    };

    class Engine
    {
    public:
        static constexpr int kVoices = 64;

        void prepare (double sr);
        void setInstrument (InstrumentPtr p) { inst = std::move (p); }
        void handleMidi (const juce::MidiMessage&, int padsChannel, int keysChannel);
        void render (juce::AudioBuffer<float>& out, int start, int n);
        void allNotesOff();
        int activeVoices() const;
        std::atomic<uint32_t> padHits[kPads] {};   // for the editor's pad flashes

    private:
        void noteOn (int ch, int note, int vel, int padsChannel, int keysChannel);
        void noteOff (int ch, int note);
        const Zone* pickZone (const std::vector<Zone>& zones, int note, int vel, bool keys, RRMode mode, int counterIndex);
        void startVoice (const Zone&, const Voicing&, int note, int vel, int ch, bool fromPad, int padIndex, int choke, bool oneShot);

        InstrumentPtr inst;
        Voice voices[kVoices];
        double sampleRate = 44100.0;
        uint64_t ageCounter = 0;
        int rrCounter[kPads + 128] {};      // per pad, then per key
        int rrLast[kPads + 128] {};
        std::minstd_rand rng { 12345 };
        std::vector<const Zone*> candidates;
    };
}
