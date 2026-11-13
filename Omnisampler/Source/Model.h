#pragma once
// Omnisampler data model: samples, zones, pads, the keys map, and the whole instrument.
// An Instrument is treated as immutable once handed to the audio thread; edits make a copy.
#include <juce_audio_formats/juce_audio_formats.h>
#include <memory>
#include <vector>

namespace omni
{
    constexpr int kPads = 64;          // 4 banks of 16
    constexpr int kPadsPerBank = 16;
    constexpr int kOutputs = 16;       // stereo outputs: 0 = main, 1..15 = Out 2..16
    constexpr int kMaxSynthLayers = 4; // drum-synth layers per pad

    struct Sample
    {
        juce::AudioBuffer<float> data;  // 1 or 2 channels
        double sampleRate = 44100.0;
        juce::File file;
        float peakDb = -100.0f, loudDb = -100.0f;   // loudDb: loudest 10 ms RMS (used for velocity mapping)
        int metaRoot = -1;                          // from WAV/AIFF metadata, -1 if none
        int metaLoopStart = -1, metaLoopEnd = -1;

        // Disk streaming: long files keep only their first `data.getNumSamples()` frames in memory
        // and the rest is read from disk while a voice plays (see Streamer.h).
        bool streamed = false;
        int totalFrames = 0;                        // whole file length (== data length when not streamed)
        int numChannels = 1;
        juce::AudioBuffer<float> overview;          // streamed only: 1 frame per kOverviewStep frames, max |x|, for drawing
        static constexpr int kOverviewStep = 256;

        int length() const noexcept { return streamed ? totalFrames : data.getNumSamples(); }
        // For waveform drawing: a buffer plus how many file frames each of its frames stands for.
        const juce::AudioBuffer<float>& drawData() const noexcept { return streamed ? overview : data; }
        int drawStep() const noexcept { return streamed ? kOverviewStep : 1; }
    };
    using SamplePtr = std::shared_ptr<const Sample>;

    enum class RRMode { Sequential, Random, RandomNoRepeat };
    enum class FilterType { Off, LowPass, HighPass, BandPass };

    struct Zone
    {
        SamplePtr sample;
        int loKey = 0, hiKey = 127, root = 60;
        int loVel = 1, hiVel = 127;
        int rrPos = 0;                  // 0 = not a round robin; 1..n position in the cycle
        float tuneCents = 0.0f, gainDb = 0.0f, pan = 0.0f;
        bool loop = false;
        int loopStart = 0, loopEnd = 0;
        int start = 0;                  // sample start offset
    };

    struct Envelope { float attack = 0.001f, decay = 0.3f, sustain = 1.0f, release = 0.25f; };
    struct FilterSettings { FilterType type = FilterType::Off; float cutoff = 18000.0f, resonance = 0.2f; };

    // Shared playback settings for a pad or for the keys section.
    struct Voicing
    {
        float volumeDb = 0.0f, pan = 0.0f, tuneSemis = 0.0f;
        Envelope env;
        FilterSettings filter;
        float velSens = 0.8f;           // 0 = velocity ignored, 1 = full range
        RRMode rr = RRMode::Sequential;
    };

    // A drum-synth layer (the Strata Drums engines). Plays alongside a pad's samples.
    enum class DrumModel { Kick, Snare, Tom, ClosedHat, OpenHat, Cymbal, Clap, Perc, Noise, Bass808, NumModels };
    struct SynthLayer
    {
        DrumModel model = DrumModel::Kick;
        bool on = true;
        float levelDb = 0.0f, pan = 0.0f;
        float tuneSemis = 0.0f;         // relative to the model's own base pitch
        float decay = 0.4f;             // seconds, main amplitude decay (0.02..4)
        float tone = 0.5f;              // 0..1, dark..bright (model-specific)
        float pitchEnv = 0.5f;          // 0..1, amount of pitch drop at the start (kick/tom/808/perc)
        float pitchDecay = 0.05f;       // seconds
        float click = 0.3f;             // 0..1, transient/click layer level
        float noise = 0.2f;             // 0..1, noise amount (snare wires, hat/cymbal body, clap)
        float drive = 0.0f;             // 0..1, saturation
        float velSens = 0.8f;           // 0 = velocity ignored, 1 = full range
    };

    struct Pad
    {
        juce::String name;
        int note = 36;
        std::vector<Zone> zones;        // velocity layers and round robins
        Voicing voicing;
        int chokeGroup = 0;             // 0 = none
        bool oneShot = true;
        std::vector<SynthLayer> synths; // up to kMaxSynthLayers, played with the samples
        float sampleLevelDb = 0.0f;     // level of the sample layers against the synth layers
        int output = 0;                 // 0 = main, 1..15 = Out 2..16

        bool isEmpty() const { return zones.empty() && synths.empty(); }
    };

    struct KeysMap
    {
        juce::String name;
        std::vector<Zone> zones;
        Voicing voicing;
        int output = 0;
    };

    struct Instrument
    {
        Instrument()
        {
            for (int i = 0; i < kPads; ++i)
            {
                pads[i].note = 36 + i;
                pads[i].voicing.env = { 0.0005f, 0.5f, 1.0f, 0.3f };
            }
            keys.voicing.env = { 0.002f, 0.4f, 1.0f, 0.35f };
        }
        Pad pads[kPads];
        KeysMap keys;
    };
    using InstrumentPtr = std::shared_ptr<const Instrument>;

    inline juce::String noteName (int n)
    {
        static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        return juce::String (names[((n % 12) + 12) % 12]) + juce::String (n / 12 - 1);   // 60 = C4
    }
}
