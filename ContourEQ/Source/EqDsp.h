#pragma once
// Filter design for Contour EQ: RBJ biquads plus Butterworth cascades for the cut slopes.
// Pure C++, no JUCE, so the response maths is shared by the audio path and the display.
#include <algorithm>
#include <cmath>
#include <complex>

namespace contour
{
    constexpr double kPi = 3.14159265358979323846;
    constexpr int kMaxBands = 24;
    constexpr int kMaxSections = 8;   // 96 dB/oct = 16th order = 8 biquads

    enum class BandType { Bell, LowShelf, HighShelf, LowCut, HighCut, Notch, BandPass, TiltShelf, FlatTilt };
    constexpr int kNumTypes = 9;
    inline const char* typeName (int t)
    {
        static const char* n[] = { "Bell", "Low Shelf", "High Shelf", "Low Cut", "High Cut", "Notch", "Band Pass", "Tilt Shelf", "Flat Tilt" };
        return n[std::clamp (t, 0, kNumTypes - 1)];
    }

    enum class Placement { Stereo, Left, Right, Mid, Side };
    inline const char* placementName (int p)
    {
        static const char* n[] = { "Stereo", "Left", "Right", "Mid", "Side" };
        return n[std::clamp (p, 0, 4)];
    }

    constexpr int kNumSlopes = 8;
    inline int slopeDb (int index) { static const int s[] = { 6, 12, 18, 24, 36, 48, 72, 96 }; return s[std::clamp (index, 0, kNumSlopes - 1)]; }

    inline bool typeUsesGain (BandType t)
    {
        return t == BandType::Bell || t == BandType::LowShelf || t == BandType::HighShelf
            || t == BandType::TiltShelf || t == BandType::FlatTilt;
    }
    inline bool typeUsesSlope (BandType t) { return t == BandType::LowCut || t == BandType::HighCut; }

    struct Coeffs { double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; };

    struct BandSpec
    {
        BandType type = BandType::Bell;
        double freq = 1000.0, gainDb = 0.0, q = 0.707;
        int slopeIndex = 1;
    };

    struct BandDesign
    {
        Coeffs sec[kMaxSections];
        int numSections = 1;
    };

    // ---- RBJ cookbook -------------------------------------------------------------------
    inline Coeffs normalise (double b0, double b1, double b2, double a0, double a1, double a2)
    {
        return { b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0 };
    }

    inline Coeffs peak (double f, double q, double gDb, double sr)
    {
        double A = std::pow (10.0, gDb / 40.0), w = 2 * kPi * f / sr, al = std::sin (w) / (2 * q), c = std::cos (w);
        return normalise (1 + al * A, -2 * c, 1 - al * A, 1 + al / A, -2 * c, 1 - al / A);
    }
    inline Coeffs lowShelf (double f, double q, double gDb, double sr)
    {
        double A = std::pow (10.0, gDb / 40.0), w = 2 * kPi * f / sr, c = std::cos (w), al = std::sin (w) / (2 * q), s = 2 * std::sqrt (A) * al;
        return normalise (A * ((A + 1) - (A - 1) * c + s), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - s),
                          (A + 1) + (A - 1) * c + s, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - s);
    }
    inline Coeffs highShelf (double f, double q, double gDb, double sr)
    {
        double A = std::pow (10.0, gDb / 40.0), w = 2 * kPi * f / sr, c = std::cos (w), al = std::sin (w) / (2 * q), s = 2 * std::sqrt (A) * al;
        return normalise (A * ((A + 1) + (A - 1) * c + s), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - s),
                          (A + 1) - (A - 1) * c + s, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - s);
    }
    inline Coeffs lowPass (double f, double q, double sr)
    {
        double w = 2 * kPi * f / sr, c = std::cos (w), al = std::sin (w) / (2 * q);
        return normalise ((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + al, -2 * c, 1 - al);
    }
    inline Coeffs highPass (double f, double q, double sr)
    {
        double w = 2 * kPi * f / sr, c = std::cos (w), al = std::sin (w) / (2 * q);
        return normalise ((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al);
    }
    inline Coeffs bandPass (double f, double q, double sr)
    {
        double w = 2 * kPi * f / sr, c = std::cos (w), al = std::sin (w) / (2 * q);
        return normalise (al, 0, -al, 1 + al, -2 * c, 1 - al);
    }
    inline Coeffs notch (double f, double q, double sr)
    {
        double w = 2 * kPi * f / sr, c = std::cos (w), al = std::sin (w) / (2 * q);
        return normalise (1, -2 * c, 1, 1 + al, -2 * c, 1 - al);
    }
    inline Coeffs lowPass1 (double f, double sr)
    {
        double K = std::tan (kPi * f / sr);
        return { K / (1 + K), K / (1 + K), 0, (K - 1) / (K + 1), 0 };
    }
    inline Coeffs highPass1 (double f, double sr)
    {
        double K = std::tan (kPi * f / sr);
        return { 1 / (1 + K), -1 / (1 + K), 0, (K - 1) / (K + 1), 0 };
    }
    inline Coeffs scaled (Coeffs c, double gain) { c.b0 *= gain; c.b1 *= gain; c.b2 *= gain; return c; }

    // ---- band design ---------------------------------------------------------------------
    inline BandDesign designBand (const BandSpec& spec, double sr)
    {
        BandDesign d;
        const double f = std::clamp (spec.freq, 5.0, sr * 0.495);
        const double q = std::clamp (spec.q, 0.025, 40.0);
        const double g = spec.gainDb;
        d.numSections = 1;

        switch (spec.type)
        {
            case BandType::Bell:      d.sec[0] = peak (f, q, g, sr); break;
            case BandType::LowShelf:  d.sec[0] = lowShelf (f, q, g, sr); break;
            case BandType::HighShelf: d.sec[0] = highShelf (f, q, g, sr); break;
            case BandType::Notch:     d.sec[0] = notch (f, q, sr); break;
            case BandType::BandPass:  d.sec[0] = bandPass (f, q, sr); break;
            case BandType::TiltShelf:
                // High shelf with the level pivoting around the corner: lows at -g/2, highs at +g/2.
                d.sec[0] = scaled (highShelf (f, q, g, sr), std::pow (10.0, -g / 40.0));
                break;
            case BandType::FlatTilt:
            {
                // Three wide shelves spread over 6 octaves give a gentle, even tilt through the pivot.
                d.numSections = 3;
                const double fs[] = { f / 8.0, f, f * 8.0 };
                for (int i = 0; i < 3; ++i)
                    d.sec[i] = highShelf (std::clamp (fs[i], 5.0, sr * 0.495), 0.4, g / 3.0, sr);
                d.sec[0] = scaled (d.sec[0], std::pow (10.0, -g / 40.0));
                break;
            }
            case BandType::LowCut:
            case BandType::HighCut:
            {
                const bool hp = spec.type == BandType::LowCut;
                const int order = slopeDb (spec.slopeIndex) / 6;
                const int pairs = order / 2;
                int s = 0;
                if (order % 2 == 1)
                    d.sec[s++] = hp ? highPass1 (f, sr) : lowPass1 (f, sr);
                for (int k = 0; k < pairs; ++k)
                {
                    // Butterworth pole pairs; the last (highest-Q) pair takes the user's resonance.
                    double theta = kPi * (2.0 * k + 1.0) / (2.0 * order);
                    double qk = 1.0 / (2.0 * std::cos (theta));
                    if (order % 2 == 1)
                        qk = 1.0 / (2.0 * std::cos (kPi * (k + 1.0) / order));
                    if (k == pairs - 1) qk *= q / 0.7071;
                    d.sec[s++] = hp ? highPass (f, qk, sr) : lowPass (f, qk, sr);
                }
                d.numSections = std::max (1, s);
                break;
            }
        }
        return d;
    }

    inline std::complex<double> response (const Coeffs& c, double w)
    {
        const std::complex<double> z1 = std::polar (1.0, -w), z2 = z1 * z1;
        return (c.b0 + c.b1 * z1 + c.b2 * z2) / (1.0 + c.a1 * z1 + c.a2 * z2);
    }

    inline double magnitudeDb (const BandDesign& d, double freq, double sr)
    {
        const double w = 2 * kPi * std::min (freq, sr * 0.4999) / sr;
        double mag = 1.0;
        for (int i = 0; i < d.numSections; ++i)
            mag *= std::abs (response (d.sec[i], w));
        return 20.0 * std::log10 (std::max (mag, 1e-9));
    }

    // Transposed direct form II biquad, double state.
    struct Biquad
    {
        double z1 = 0, z2 = 0;
        inline float process (float xIn, const Coeffs& c) noexcept
        {
            const double x = xIn;
            const double y = c.b0 * x + z1;
            z1 = c.b1 * x - c.a1 * y + z2;
            z2 = c.b2 * x - c.a2 * y;
            return (float) y;
        }
        void reset() { z1 = z2 = 0; }
    };

}
