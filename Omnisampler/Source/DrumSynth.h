#pragma once
// Drum synth engines (from Strata Drums): kick, snare, tom, hats, cymbal, clap, perc/FM, noise, 808.
// One DrumVoice plays one hit of one SynthLayer. Real-time safe: no allocation, no locks.
#include "Model.h"
#include <cstdint>

namespace omni
{
    const char* drumModelName (DrumModel m);           // "Kick", "Snare", ...
    SynthLayer defaultSynthLayer (DrumModel m);        // a good-sounding starting point for each model

    class DrumVoice
    {
    public:
        // velocity01 is 0..1 (already curved by the caller is NOT assumed: apply layer.velSens yourself).
        void start (const SynthLayer& layer, double sampleRate, float velocity01, uint32_t seed) noexcept;
        // Writes (not adds) n mono samples to out. Returns false once the hit has died away
        // (then the rest of out is zero and isActive() is false).
        bool render (float* out, int n) noexcept;
        bool isActive() const noexcept { return active; }
        void stop() noexcept { active = false; }

    private:
        bool active = false;
        SynthLayer p;
        double sr = 44100.0;
        float vel = 1.0f;
        uint32_t rng = 1;
        // Free-form state for the implementation (oscillator phases, envelopes, filters).
        double t = 0.0;
        double phase[6] {};
        float env[4] {}, envMul[4] {};
        float flt[8] {};
        float clapTimer = 0.0f;
        int clapStage = 0;
        float dc = 0.0f;
        // Extra private state (all POD)
        float cf[16] {};                    // per-model coefficients / amounts (filter coeffs, mix levels)
        float dcX = 0.0f, outGain = 1.0f, trim = 1.0f, driveA = 0.0f, driveNorm = 1.0f;
        float rampPos = 1.0f, rampInc = 0.0f;           // tiny attack ramp (de-click)
        float pe = 0.0f, peMul = 1.0f, peAmt = 0.0f;    // pitch envelope: ratio = 1 + peAmt * pe
        float f0 = 100.0f, toneE = 0.5f, clk = 0.0f;
        int ctr = 0;
        float tick() noexcept;
        float white() noexcept;
    };
}
