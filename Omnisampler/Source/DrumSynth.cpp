#include "DrumSynth.h"
#include <algorithm>
#include <cmath>

namespace omni
{
    namespace
    {
        constexpr double kPi = 3.14159265358979323846;
        constexpr double kTwoPi = 2.0 * kPi;
        constexpr double kLn1000 = 6.907755278982137;   // -60 dB
        constexpr float kEndLevel = 3.2e-5f;            // -90 dB

        // cf[] slots
        constexpr int kSvfA = 0;    // 0..3  : a1 a2 a3 k
        constexpr int kLp = 8;      // one-pole low-pass coefficient
        constexpr int kClickAmt = 9;
        constexpr int kNoiseAmt = 10;
        constexpr int kBodyAmt = 11;
        constexpr int kScale = 12;  // metallic frequency scale / clap gap
        constexpr int kAux = 13;
        constexpr int kDcR = 15;

        inline float clamp01 (float x) noexcept { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }
        inline float expMul (double seconds, double sr) noexcept   // time constant -> per-sample multiplier
        {
            return (float) std::exp (-1.0 / (std::max (seconds, 1.0e-5) * sr));
        }
        inline float t60Mul (double seconds, double sr) noexcept   // time to -60 dB -> per-sample multiplier
        {
            return (float) std::exp (-kLn1000 / (std::max (seconds, 1.0e-4) * sr));
        }
        inline float onePole (double fc, double sr) noexcept
        {
            return (float) (1.0 - std::exp (-kTwoPi * std::min (fc, sr * 0.45) / sr));
        }
        inline void svfSet (float* c, double fc, double q, double sr) noexcept
        {
            fc = std::clamp (fc, 20.0, sr * 0.45);
            const double g = std::tan (kPi * fc / sr);
            const double k = 1.0 / q;
            const double a1 = 1.0 / (1.0 + g * (g + k));
            c[0] = (float) a1;
            c[1] = (float) (g * a1);
            c[2] = (float) (g * g * a1);
            c[3] = (float) k;
        }
        // TPT state variable filter; s[0], s[1] are the states.
        inline void svfRun (const float* c, float* s, float x, float& lo, float& bp, float& hi) noexcept
        {
            const float v3 = x - s[1];
            const float v1 = c[0] * s[0] + c[1] * v3;
            const float v2 = s[1] + c[1] * s[0] + c[2] * v3;
            s[0] = 2.0f * v1 - s[0];
            s[1] = 2.0f * v2 - s[1];
            lo = v2; bp = v1; hi = x - c[3] * v1 - v2;
        }
        inline float polyBlep (float t, float dt) noexcept
        {
            if (t < dt)         { t /= dt; return t + t - t * t - 1.0f; }
            if (t > 1.0f - dt)  { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
            return 0.0f;
        }
        inline float polySquare (float ph, float dt) noexcept
        {
            float v = ph < 0.5f ? 1.0f : -1.0f;
            v += polyBlep (ph, dt);
            float p2 = ph + 0.5f; if (p2 >= 1.0f) p2 -= 1.0f;
            v -= polyBlep (p2, dt);
            return v;
        }
        inline float wrap1 (float x) noexcept { return x >= 1.0f ? x - 1.0f : x; }
        inline double wrap1d (double x) noexcept { return x >= 1.0 ? x - std::floor (x) : x; }

        constexpr float kMetalRatios[6] = { 205.3f, 304.4f, 369.6f, 522.7f, 540.0f, 800.0f };   // 808 hat oscillators

        // Per-model output trims (measured, so a full-velocity default hit peaks near 0.9).
        constexpr float kTrim[10] = { 0.70f, 0.50f, 0.65f, 4.20f, 4.20f, 3.00f, 0.52f, 0.65f, 0.70f, 0.80f };
    }

    const char* drumModelName (DrumModel m)
    {
        switch (m)
        {
            case DrumModel::Kick:      return "Kick";
            case DrumModel::Snare:     return "Snare";
            case DrumModel::Tom:       return "Tom";
            case DrumModel::ClosedHat: return "Closed Hat";
            case DrumModel::OpenHat:   return "Open Hat";
            case DrumModel::Cymbal:    return "Cymbal";
            case DrumModel::Clap:      return "Clap";
            case DrumModel::Perc:      return "Perc";
            case DrumModel::Noise:     return "Noise";
            case DrumModel::Bass808:   return "808";
            case DrumModel::NumModels: break;
        }
        return "?";
    }

    SynthLayer defaultSynthLayer (DrumModel m)
    {
        SynthLayer s;
        s.model = m;
        s.on = true;
        s.levelDb = 0.0f; s.pan = 0.0f; s.tuneSemis = 0.0f; s.velSens = 0.8f;
        auto set = [&s] (float decay, float tone, float pitchEnv, float pitchDecay, float click, float noise, float drive)
        {
            s.decay = decay; s.tone = tone; s.pitchEnv = pitchEnv; s.pitchDecay = pitchDecay;
            s.click = click; s.noise = noise; s.drive = drive;
        };
        switch (m)
        {
            case DrumModel::Kick:      set (0.45f, 0.50f, 0.60f, 0.040f, 0.40f, 0.10f, 0.15f); break;
            case DrumModel::Snare:     set (0.25f, 0.50f, 0.20f, 0.020f, 0.35f, 0.60f, 0.10f); break;
            case DrumModel::Tom:       set (0.40f, 0.40f, 0.35f, 0.080f, 0.25f, 0.10f, 0.00f); break;
            case DrumModel::ClosedHat: set (0.06f, 0.60f, 0.00f, 0.050f, 0.50f, 0.25f, 0.00f); break;
            case DrumModel::OpenHat:   set (0.50f, 0.60f, 0.00f, 0.050f, 0.30f, 0.30f, 0.00f); break;
            case DrumModel::Cymbal:    set (2.00f, 0.65f, 0.00f, 0.050f, 0.30f, 0.35f, 0.00f); break;
            case DrumModel::Clap:      set (0.35f, 0.50f, 0.00f, 0.050f, 0.50f, 0.30f, 0.10f); break;
            case DrumModel::Perc:      set (0.30f, 0.45f, 0.40f, 0.060f, 0.30f, 0.10f, 0.00f); break;
            case DrumModel::Noise:     set (0.25f, 0.55f, 0.00f, 0.050f, 0.20f, 0.20f, 0.00f); break;
            case DrumModel::Bass808:   set (1.50f, 0.25f, 0.30f, 0.100f, 0.20f, 0.05f, 0.20f); break;
            case DrumModel::NumModels: break;
        }
        return s;
    }

    float DrumVoice::white() noexcept
    {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        return (float) (int32_t) rng * (1.0f / 2147483648.0f);
    }

    void DrumVoice::start (const SynthLayer& layer, double sampleRate, float velocity01, uint32_t seed) noexcept
    {
        p = layer;
        sr = sampleRate > 1000.0 ? sampleRate : 44100.0;
        vel = clamp01 (velocity01);
        p.decay = std::clamp (p.decay, 0.02f, 4.0f);
        p.tone = clamp01 (p.tone);
        p.pitchEnv = clamp01 (p.pitchEnv);
        p.pitchDecay = std::clamp (p.pitchDecay, 0.002f, 2.0f);
        p.click = clamp01 (p.click);
        p.noise = clamp01 (p.noise);
        p.drive = clamp01 (p.drive);
        p.velSens = clamp01 (p.velSens);
        p.tuneSemis = std::clamp (p.tuneSemis, -48.0f, 48.0f);

        rng = seed != 0 ? seed : 0x9E3779B9u;
        for (int i = 0; i < 8; ++i) white();

        t = 0.0; clapTimer = 0.0f; clapStage = 0; dc = 0.0f; dcX = 0.0f; ctr = 0;
        for (auto& x : phase) x = 0.0;
        for (auto& x : env) x = 0.0f;
        for (auto& x : envMul) x = 1.0f;
        for (auto& x : flt) x = 0.0f;
        for (auto& x : cf) x = 0.0f;

        const double tune = std::exp2 ((double) p.tuneSemis / 12.0);
        const float velGain = 1.0f - p.velSens + p.velSens * vel * vel;
        outGain = velGain * std::pow (10.0f, p.levelDb * 0.05f);
        toneE = clamp01 (p.tone + 0.2f * p.velSens * (vel - 0.5f));       // harder hits are a little brighter
        clk = clamp01 (p.click * (1.0f + p.velSens * (vel - 0.5f)));      // ... and clickier
        driveA = p.drive > 0.001f ? p.drive * 6.0f : 0.0f;
        driveNorm = driveA > 0.0f ? 0.9f / std::tanh (0.9f * driveA) : 1.0f;
        trim = kTrim[std::clamp (static_cast<int> (p.model), 0, 9)];
        cf[kDcR] = (float) (1.0 - kTwoPi * 5.0 / sr);

        env[0] = 1.0f;
        envMul[0] = t60Mul (p.decay, sr);
        pe = 1.0f;
        peMul = (float) std::exp (-3.0 / ((double) p.pitchDecay * sr));
        peAmt = 0.0f;

        const bool ramped = p.model != DrumModel::Kick && p.model != DrumModel::Bass808 && p.model != DrumModel::Clap;
        rampPos = ramped ? 0.0f : 1.0f;
        rampInc = (float) (1.0 / (0.00025 * sr));

        switch (p.model)
        {
            case DrumModel::Kick:
                f0 = (float) (50.0 * tune);
                peAmt = (float) (std::exp2 (4.0 * (double) p.pitchEnv) - 1.0);
                env[1] = 1.0f; envMul[1] = expMul (0.0025, sr);        // click
                env[2] = 1.0f; envMul[2] = expMul (0.012, sr);         // noise thump
                cf[kLp] = onePole (1500.0 + (double) toneE * 11000.0, sr);
                cf[kClickAmt] = clk * 1.0f;
                cf[kNoiseAmt] = p.noise * 0.7f;
                break;

            case DrumModel::Snare:
                f0 = (float) (185.0 * tune);
                peAmt = (float) (std::exp2 (1.2 * (double) p.pitchEnv) - 1.0);
                env[1] = 1.0f; envMul[1] = t60Mul (std::clamp ((double) p.decay * 0.5, 0.03, 0.35), sr);   // bodies
                env[2] = 1.0f; envMul[2] = expMul (0.0012, sr);                                          // click
                svfSet (&cf[kSvfA], 900.0 + (double) toneE * 700.0, 0.7, sr);                           // wires high-pass
                cf[kLp] = onePole (1800.0 + (double) toneE * 6000.0, sr);
                cf[kNoiseAmt] = 0.15f + p.noise * 0.85f;
                cf[kBodyAmt] = 0.9f - 0.3f * p.noise;
                cf[kClickAmt] = clk * 0.7f;
                break;

            case DrumModel::Tom:
                f0 = (float) (110.0 * tune);
                peAmt = (float) (std::exp2 (2.0 * (double) p.pitchEnv) - 1.0);
                env[1] = 1.0f; envMul[1] = t60Mul ((double) p.decay * 0.4, sr);    // membrane overtone
                env[2] = 1.0f; envMul[2] = expMul (0.0018, sr);                     // click
                env[3] = 1.0f; envMul[3] = expMul (0.02, sr);                       // stick noise
                cf[kLp] = onePole (2000.0 + (double) toneE * 8000.0, sr);
                cf[kClickAmt] = clk * 1.2f;
                cf[kNoiseAmt] = p.noise * 0.8f;
                break;

            case DrumModel::ClosedHat:
            case DrumModel::OpenHat:
            case DrumModel::Cymbal:
            {
                const bool cym = p.model == DrumModel::Cymbal;
                cf[kScale] = (float) (tune * (cym ? 1.5 : 1.0));
                svfSet (&cf[kSvfA], 7000.0 * (0.75 + 0.5 * (double) toneE), 0.9, sr);                  // metal band-pass
                svfSet (&cf[4], (cym ? 1800.0 : 3000.0) + (double) toneE * (cym ? 4500.0 : 6000.0), 0.7, sr);            // body high-pass
                const double strike = p.model == DrumModel::ClosedHat ? 0.008 : (cym ? 0.08 : 0.02);
                env[1] = 1.0f; envMul[1] = expMul (strike, sr);
                cf[kNoiseAmt] = p.noise * 0.9f;
                cf[kClickAmt] = clk * 2.0f;
                break;
            }

            case DrumModel::Clap:
            {
                svfSet (&cf[kSvfA], 800.0 + (double) toneE * 2200.0, 1.4, sr);
                env[0] = 1.0f; envMul[0] = 1.0f;                       // tail envelope; starts decaying at the last burst
                envMul[1] = expMul (0.004, sr);                        // burst envelope
                cf[kLp] = onePole (3000.0, sr);
                cf[kClickAmt] = 0.9f + clk * 0.6f;
                cf[kNoiseAmt] = p.noise * 0.5f;
                cf[kScale] = (float) (0.0105 * sr);                    // gap between bursts (samples)
                cf[kAux] = 0.0f;                                       // next trigger time (samples)
                break;
            }

            case DrumModel::Perc:
                f0 = (float) (400.0 * tune);
                peAmt = (float) (std::exp2 (2.0 * (double) p.pitchEnv) - 1.0);
                env[1] = 1.0f; envMul[1] = t60Mul ((double) p.decay * 0.5, sr);    // index envelope
                env[2] = 1.0f; envMul[2] = expMul (0.002, sr);                      // click
                env[3] = 1.0f; envMul[3] = expMul (0.01, sr);                       // noise burst
                cf[kLp] = onePole (6000.0, sr);
                cf[kClickAmt] = clk * 0.9f;
                cf[kNoiseAmt] = p.noise * 1.0f;
                cf[kAux] = toneE * 8.0f;                                            // modulation index
                break;

            case DrumModel::Noise:
            {
                peAmt = (float) (std::exp2 (3.0 * (double) p.pitchEnv) - 1.0);
                cf[kAux] = (float) (0.6 + 4.0 * (double) p.noise);                  // resonance Q
                cf[kScale] = (float) (150.0 * std::exp2 (7.0 * (double) toneE) * tune);   // cutoff
                svfSet (&cf[kSvfA], (double) cf[kScale] * (1.0 + (double) peAmt), (double) cf[kAux], sr);
                env[1] = 1.0f; envMul[1] = expMul (0.001, sr);
                cf[kClickAmt] = clk * 1.0f;
                break;
            }

            case DrumModel::Bass808:
                f0 = (float) (45.0 * tune);
                peAmt = (float) (std::exp2 (1.5 * (double) p.pitchEnv) - 1.0);
                env[1] = 1.0f; envMul[1] = expMul (0.004, sr);                      // click
                env[2] = 1.0f; envMul[2] = expMul (0.015, sr);                      // air
                cf[kLp] = onePole (1200.0 + (double) toneE * 5000.0, sr);
                cf[kClickAmt] = clk * 1.2f;
                cf[kNoiseAmt] = p.noise * 0.5f;
                break;

            case DrumModel::NumModels: break;
        }
        active = true;
    }

    float DrumVoice::tick() noexcept
    {
        const float isr = (float) (1.0 / sr);
        float y = 0.0f;

        switch (p.model)
        {
            case DrumModel::Kick:
            {
                const float f = f0 * (1.0f + peAmt * pe);
                pe *= peMul;
                phase[0] = wrap1d (phase[0] + (double) (f * isr));
                const float w = (float) (kTwoPi * phase[0]);
                const float body = std::sin (w + toneE * 1.2f * pe * std::sin (w));
                const float n = white();
                flt[0] += cf[kLp] * (n - flt[0]);
                y = body * env[0] + flt[0] * (env[1] * cf[kClickAmt] + env[2] * cf[kNoiseAmt]) * 2.0f;
                break;
            }

            case DrumModel::Snare:
            {
                const float r = 1.0f + peAmt * pe;
                pe *= peMul;
                phase[0] = wrap1d (phase[0] + (double) (f0 * r * isr));
                phase[1] = wrap1d (phase[1] + (double) (f0 * 1.78f * r * isr));
                const float body = (0.8f * std::sin ((float) (kTwoPi * phase[0])) + 0.45f * std::sin ((float) (kTwoPi * phase[1])))
                                   * env[1] * cf[kBodyAmt];
                const float n = white();
                float lo, bp, hi;
                svfRun (&cf[kSvfA], &flt[0], n, lo, bp, hi);
                flt[4] += cf[kLp] * (hi - flt[4]);
                const float wires = flt[4] * env[0] * cf[kNoiseAmt] * 1.6f;
                y = body + wires + (n + hi) * 0.5f * env[2] * cf[kClickAmt];
                break;
            }

            case DrumModel::Tom:
            {
                const float r = 1.0f + peAmt * pe;
                pe *= peMul;
                phase[0] = wrap1d (phase[0] + (double) (f0 * r * isr));
                phase[1] = wrap1d (phase[1] + (double) (f0 * 1.58f * r * isr));
                const float w = (float) (kTwoPi * phase[0]);
                const float body = std::sin (w + toneE * 0.6f * pe * std::sin (w)) * env[0]
                                   + toneE * 0.35f * std::sin ((float) (kTwoPi * phase[1])) * env[1];
                const float n = white();
                flt[0] += cf[kLp] * (n - flt[0]);
                y = body + flt[0] * (env[2] * cf[kClickAmt] + env[3] * cf[kNoiseAmt]) * 2.0f;
                break;
            }

            case DrumModel::ClosedHat:
            case DrumModel::OpenHat:
            case DrumModel::Cymbal:
            {
                float metal = 0.0f;
                for (int i = 0; i < 6; ++i)
                {
                    const float fr = kMetalRatios[i] * cf[kScale];
                    const float dt = std::min (fr * isr, 0.25f);
                    phase[i] = wrap1d (phase[i] + (double) dt);
                    metal += polySquare ((float) phase[i], dt);
                }
                metal *= (1.0f / 6.0f);
                float lo, bp, hi;
                svfRun (&cf[kSvfA], &flt[0], metal, lo, bp, hi);
                const float n = white();
                float lo2, bp2, hi2;
                svfRun (&cf[4], &flt[2], bp * 1.5f + n * cf[kNoiseAmt] * 0.3f, lo2, bp2, hi2);
                const float shape = (1.0f + cf[kClickAmt] * env[1]) / (1.0f + cf[kClickAmt]);
                y = hi2 * env[0] * shape;
                break;
            }

            case DrumModel::Clap:
            {
                if (clapStage < 4 && clapTimer >= cf[kAux])
                {
                    env[1] = 1.0f;
                    if (clapStage == 3) envMul[0] = t60Mul (p.decay, sr);    // the tail begins with the last burst
                    ++clapStage;
                    cf[kAux] += cf[kScale] * (0.85f + 0.3f * (white() * 0.5f + 0.5f));
                }
                clapTimer += 1.0f;
                const float n = white();
                float lo, bp, hi;
                svfRun (&cf[kSvfA], &flt[0], n, lo, bp, hi);
                flt[4] += cf[kLp] * (n - flt[4]);
                const float tail = clapStage >= 4 ? env[0] : 0.0f;
                y = bp * 1.8f * (env[1] * cf[kClickAmt] + tail * 0.7f) + (n - flt[4]) * cf[kNoiseAmt] * tail * 0.5f;
                break;
            }

            case DrumModel::Perc:
            {
                const float r = 1.0f + peAmt * pe;
                pe *= peMul;
                const float fc = f0 * r;
                phase[0] = wrap1d (phase[0] + (double) (fc * isr));
                phase[1] = wrap1d (phase[1] + (double) (fc * 1.4142f * isr));
                const float idx = cf[kAux] * env[1];
                const float mod = std::sin ((float) (kTwoPi * phase[1]));
                const float body = std::sin ((float) (kTwoPi * phase[0]) + idx * mod) * env[0];
                const float n = white();
                flt[0] += cf[kLp] * (n - flt[0]);
                y = body + flt[0] * (env[2] * cf[kClickAmt] + env[3] * cf[kNoiseAmt]) * 1.5f;
                break;
            }

            case DrumModel::Noise:
            {
                if (peAmt > 0.001f && (ctr & 15) == 0)
                    svfSet (&cf[kSvfA], (double) cf[kScale] * (1.0 + (double) (peAmt * pe)), (double) cf[kAux], sr);
                pe *= peMul;
                ++ctr;
                const float n = white();
                float lo, bp, hi;
                svfRun (&cf[kSvfA], &flt[0], n, lo, bp, hi);
                const float q = cf[kAux];
                y = lo * (1.0f / std::sqrt (q)) * 1.6f * env[0] + n * env[1] * cf[kClickAmt];
                break;
            }

            case DrumModel::Bass808:
            {
                const float f = f0 * (1.0f + peAmt * pe);
                pe *= peMul;
                phase[0] = wrap1d (phase[0] + (double) (f * isr));
                const float w = (float) (kTwoPi * phase[0]);
                const float body = (std::sin (w) + toneE * 0.3f * std::sin (3.0f * w)) * env[0];
                const float n = white();
                flt[0] += cf[kLp] * (n - flt[0]);
                y = body + flt[0] * (env[1] * cf[kClickAmt] + env[2] * cf[kNoiseAmt]) * 2.0f;
                break;
            }

            case DrumModel::NumModels: break;
        }

        for (int i = 0; i < 4; ++i)
            env[i] *= envMul[i];
        return y;
    }

    bool DrumVoice::render (float* out, int n) noexcept
    {
        if (! active)
        {
            for (int i = 0; i < n; ++i) out[i] = 0.0f;
            return false;
        }
        const float R = cf[kDcR];
        for (int i = 0; i < n; ++i)
        {
            if (! active) { out[i] = 0.0f; continue; }

            float x = tick() * trim;
            if (rampPos < 1.0f) { x *= rampPos; rampPos += rampInc; }
            if (driveA > 0.0f)
                x = std::tanh (driveA * x) * driveNorm;
            // DC blocker (5 Hz)
            const float yv = x - dcX + R * dc;
            dcX = x; dc = yv;
            float o = yv * outGain;
            const float a = std::fabs (o);
            if (a > 0.95f)   // gentle safety above 0.95, never beyond ~1.2
                o = std::copysign (0.95f + 0.25f * std::tanh ((a - 0.95f) * 4.0f), o);
            out[i] = o;
            t += 1.0 / sr;

            if (env[0] < kEndLevel)
                active = false;
        }
        return active;
    }
}
