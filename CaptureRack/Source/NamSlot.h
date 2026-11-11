#pragma once
// One NAM slot: model holder, sample-rate conversion and noise gate.
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include <memory>
#include <vector>

namespace nam { class DSP; }

namespace rack
{
    // Runs a model at its own sample rate when the host runs at another.
    class Resampled
    {
    public:
        void prepare (double hostRate, double modelRate, int maxBlock);
        // In-place, mono. Adds a small fixed latency when rates differ.
        void process (nam::DSP& model, float* io, int n) noexcept;
        bool needed() const { return active; }

    private:
        bool active = false;
        double speedUp = 1.0, speedDown = 1.0;   // input samples consumed per output sample
        juce::LagrangeInterpolator up, down;
        std::vector<float> inFifo, mid, modelIn, modelOut, outFifo, tmp;
        size_t inCount = 0, midCount = 0, outCount = 0;
    };

    class NoiseGate
    {
    public:
        void prepare (double sr) { sampleRate = sr; env = 0; gain = 1; hold = 0; }
        // Detects on the input; writes the gain per sample so it can be applied after the model too
        // (models can hiss on silence).
        void process (float* io, int n, float thresholdDb, float* gainsOut) noexcept
        {
            if (thresholdDb <= -99.0f) { gain = 1.0f; for (int i = 0; i < n; ++i) gainsOut[i] = 1.0f; return; }
            const float open = juce::Decibels::decibelsToGain (thresholdDb);
            const float close = open * 0.5f;   // 6 dB hysteresis
            const float envRel = std::exp (-1.0f / (0.02f * (float) sampleRate));
            const float gAtt = 1.0f - std::exp (-1.0f / (0.001f * (float) sampleRate));
            const float gRel = 1.0f - std::exp (-1.0f / (0.06f * (float) sampleRate));
            const int holdSamples = (int) (0.03 * sampleRate);
            for (int i = 0; i < n; ++i)
            {
                const float a = std::abs (io[i]);
                env = a > env ? a : env * envRel;
                if (env > open) { target = 1.0f; hold = holdSamples; }
                else if (env < close && hold <= 0) target = 0.0f;
                if (hold > 0) --hold;
                gain += (target > gain ? gAtt : gRel) * (target - gain);
                io[i] *= gain;
                gainsOut[i] = gain;
            }
        }
        float currentGain() const { return gain; }

    private:
        double sampleRate = 48000;
        float env = 0, gain = 1, target = 1;
        int hold = 0;
    };

    // Model + its runtime state. Created on the message thread, swapped into the audio thread.
    struct LoadedModel
    {
        LoadedModel();
        ~LoadedModel();   // defined where nam::DSP is complete
        std::unique_ptr<nam::DSP> dsp;
        Resampled resampler;
        juce::File file;
        juce::String name, info, author;
        double loudnessDb = 0.0;
        bool hasLoudness = false;
        std::vector<float> scratch;
    };

    // Loads a .nam file and prepares it for the given host rate. Returns nullptr and sets error on failure.
    std::unique_ptr<LoadedModel> loadModel (const juce::File& f, double hostRate, int maxBlock, juce::String& error);
    void processModel (LoadedModel& m, float* io, int n) noexcept;
}
