#pragma once
// Spectrum analyser taps and Match EQ curve fitting for Contour EQ.
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <vector>
#include "EqDsp.h"

namespace contour
{
    constexpr int kGridPoints = 256;   // log-spaced 20 Hz .. 20 kHz
    inline double gridFreq (int i) { return 20.0 * std::pow (1000.0, i / double (kGridPoints - 1)); }
    using Grid = std::array<float, kGridPoints>;

    // Turns one FFT frame into per-grid-point power (mean) and peak (max), in linear power.
    struct GridMapper
    {
        static constexpr int fftOrder = 13, fftSize = 1 << fftOrder;

        GridMapper() : fft (fftOrder), window (fftSize, juce::dsp::WindowingFunction<float>::hann, false)
        {
            data.resize (fftSize * 2);
        }

        // frame: fftSize samples, oldest first.
        void analyse (const float* frame, double sr, std::array<double, kGridPoints>& meanPow, std::array<double, kGridPoints>& peakPow)
        {
            std::fill (data.begin(), data.end(), 0.0f);
            std::copy (frame, frame + fftSize, data.begin());
            window.multiplyWithWindowingTable (data.data(), fftSize);
            fft.performFrequencyOnlyForwardTransform (data.data(), true);
            const double norm = 4.0 / fftSize;   // Hann coherent gain 0.5, one-sided spectrum
            const double binHz = sr / fftSize;
            for (int i = 0; i < kGridPoints; ++i)
            {
                const double f = gridFreq (i);
                const double fLo = i > 0 ? std::sqrt (f * gridFreq (i - 1)) : f / 1.02;
                const double fHi = i < kGridPoints - 1 ? std::sqrt (f * gridFreq (i + 1)) : f * 1.02;
                int k0 = (int) std::floor (fLo / binHz), k1 = (int) std::ceil (fHi / binHz);
                k0 = juce::jlimit (1, fftSize / 2 - 1, k0);
                k1 = juce::jlimit (k0, fftSize / 2 - 1, k1);
                if (k1 - k0 <= 1)
                {
                    // Narrower than a bin: interpolate.
                    const double kf = juce::jlimit (1.0, fftSize / 2.0 - 2.0, f / binHz);
                    const int k = (int) kf; const double t = kf - k;
                    const double a = (1 - t) * data[(size_t) k] + t * data[(size_t) k + 1];
                    meanPow[(size_t) i] = peakPow[(size_t) i] = (a * norm) * (a * norm);
                }
                else
                {
                    double sum = 0, mx = 0;
                    for (int k = k0; k <= k1; ++k)
                    {
                        const double p = (data[(size_t) k] * norm) * (data[(size_t) k] * norm);
                        sum += p; mx = std::max (mx, p);
                    }
                    meanPow[(size_t) i] = sum / (k1 - k0 + 1);
                    peakPow[(size_t) i] = mx;
                }
            }
        }

        juce::dsp::FFT fft;
        juce::dsp::WindowingFunction<float> window;
        std::vector<float> data;
    };

    // Audio thread pushes a mono sum; the editor's timer calls tick() to analyse it.
    class SpectrumTap
    {
    public:
        SpectrumTap() : fifo (kFifoSize)
        {
            ring.resize (kFifoSize);
            history.assign (GridMapper::fftSize, 0.0f);
            display.fill (-140.0f);
        }

        void setSampleRate (double sr) { sampleRate.store (sr); }

        void push (const float* l, const float* r, int n) noexcept
        {
            int s1, n1, s2, n2;
            fifo.prepareToWrite (n, s1, n1, s2, n2);
            for (int i = 0; i < n1; ++i) ring[(size_t) (s1 + i)] = r ? 0.5f * (l[i] + r[i]) : l[i];
            for (int i = 0; i < n2; ++i) ring[(size_t) (s2 + i)] = r ? 0.5f * (l[n1 + i] + r[n1 + i]) : l[n1 + i];
            fifo.finishedWrite (n1 + n2);
        }

        // Message thread. Returns true when the display changed.
        bool tick (float decayDb = 1.6f)
        {
            const int avail = fifo.getNumReady();
            if (avail <= 0)
            {
                for (auto& d : display) d = std::max (-140.0f, d - decayDb);
                return false;
            }
            int s1, n1, s2, n2;
            fifo.prepareToRead (avail, s1, n1, s2, n2);
            auto append = [this] (const float* src, int n)
            {
                const int N = (int) history.size();
                if (n >= N) { std::copy (src + n - N, src + n, history.begin()); return; }
                std::move (history.begin() + n, history.end(), history.begin());
                std::copy (src, src + n, history.end() - n);
            };
            append (ring.data() + s1, n1);
            if (n2 > 0) append (ring.data() + s2, n2);
            fifo.finishedRead (n1 + n2);

            std::array<double, kGridPoints> mean {}, peak {};
            mapper.analyse (history.data(), sampleRate.load(), mean, peak);
            for (int i = 0; i < kGridPoints; ++i)
            {
                const float db = (float) (10.0 * std::log10 (peak[(size_t) i] + 1e-14));
                auto& d = display[(size_t) i];
                d = db > d ? d + 0.7f * (db - d) : std::max (db, d - decayDb);
            }
            if (capturing)
            {
                for (int i = 0; i < kGridPoints; ++i) captureSum[(size_t) i] += mean[(size_t) i];
                ++captureFrames;
            }
            return true;
        }

        const Grid& getDisplay() const { return display; }

        void startCapture() { captureSum.fill (0.0); captureFrames = 0; capturing = true; }
        void stopCapture() { capturing = false; }
        bool isCapturing() const { return capturing; }
        int getCapturedFrames() const { return captureFrames; }
        bool getCapture (Grid& out) const
        {
            if (captureFrames == 0) return false;
            for (int i = 0; i < kGridPoints; ++i)
                out[(size_t) i] = (float) (10.0 * std::log10 (captureSum[(size_t) i] / captureFrames + 1e-14));
            return true;
        }

    private:
        static constexpr int kFifoSize = 1 << 16;
        juce::AbstractFifo fifo;
        std::vector<float> ring, history;
        std::atomic<double> sampleRate { 48000.0 };
        GridMapper mapper;
        Grid display;
        bool capturing = false;
        std::array<double, kGridPoints> captureSum {};
        int captureFrames = 0;
    };

    // Average spectrum of a whole audio file (mono sum), for Match EQ references.
    inline bool fileSpectrum (const juce::AudioBuffer<float>& buf, double sr, Grid& out)
    {
        const int N = GridMapper::fftSize;
        if (buf.getNumSamples() < N) return false;
        GridMapper mapper;
        std::vector<float> frame ((size_t) N);
        std::array<double, kGridPoints> sum {}, mean {}, peak {};
        int frames = 0;
        for (int start = 0; start + N <= buf.getNumSamples(); start += N / 2)
        {
            for (int i = 0; i < N; ++i)
            {
                float s = 0;
                for (int c = 0; c < buf.getNumChannels(); ++c) s += buf.getSample (c, start + i);
                frame[(size_t) i] = s / (float) buf.getNumChannels();
            }
            mapper.analyse (frame.data(), sr, mean, peak);
            for (int i = 0; i < kGridPoints; ++i) sum[(size_t) i] += mean[(size_t) i];
            ++frames;
        }
        for (int i = 0; i < kGridPoints; ++i)
            out[(size_t) i] = (float) (10.0 * std::log10 (sum[(size_t) i] / frames + 1e-14));
        return true;
    }

    // ---- Match EQ ---------------------------------------------------------------------------
    struct MatchBand { BandType type; double freq, gainDb, q; };

    inline Grid smoothGrid (const Grid& in, double octaves)
    {
        const double stepOct = std::log2 (1000.0) / (kGridPoints - 1);
        const int half = std::max (0, (int) std::round (octaves * 0.5 / stepOct));
        Grid out {};
        for (int i = 0; i < kGridPoints; ++i)
        {
            double sum = 0; int n = 0;
            for (int j = std::max (0, i - half); j <= std::min (kGridPoints - 1, i + half); ++j) { sum += in[(size_t) j]; ++n; }
            out[(size_t) i] = (float) (sum / n);
        }
        return out;
    }

    // Target curve = reference minus source, smoothed and level-normalised, times amount.
    inline Grid matchTarget (const Grid& refDb, const Grid& srcDb, double amount, double smoothOct)
    {
        Grid diff {};
        auto r = smoothGrid (refDb, smoothOct), s = smoothGrid (srcDb, smoothOct);
        double mean = 0; int n = 0;
        for (int i = 0; i < kGridPoints; ++i)
        {
            diff[(size_t) i] = r[(size_t) i] - s[(size_t) i];
            const double f = gridFreq (i);
            if (f >= 100.0 && f <= 8000.0) { mean += diff[(size_t) i]; ++n; }
        }
        mean /= std::max (1, n);
        for (auto& d : diff) d = (float) juce::jlimit (-24.0, 24.0, (d - mean) * amount);
        return diff;
    }

    inline std::vector<MatchBand> fitMatch (const Grid& target, int maxBands)
    {
        constexpr double sr = 48000.0;
        std::vector<MatchBand> bands;
        std::vector<Grid> resp;
        Grid residual = target;

        auto respOf = [] (const MatchBand& b)
        {
            BandSpec s; s.type = b.type; s.freq = b.freq; s.gainDb = b.gainDb; s.q = b.q;
            auto d = designBand (s, sr);
            Grid g {};
            for (int i = 0; i < kGridPoints; ++i) g[(size_t) i] = (float) magnitudeDb (d, gridFreq (i), sr);
            return g;
        };

        for (int b = 0; b < maxBands; ++b)
        {
            int peakI = 0; float peakV = 0;
            for (int i = 0; i < kGridPoints; ++i)
                if (std::abs (residual[(size_t) i]) > std::abs (peakV)) { peakV = residual[(size_t) i]; peakI = i; }
            if (std::abs (peakV) < 0.5f) break;

            int lo = peakI, hi = peakI;
            while (lo > 0 && residual[(size_t) lo - 1] * peakV > 0 && std::abs (residual[(size_t) lo - 1]) > std::abs (peakV) * 0.5f) --lo;
            while (hi < kGridPoints - 1 && residual[(size_t) hi + 1] * peakV > 0 && std::abs (residual[(size_t) hi + 1]) > std::abs (peakV) * 0.5f) ++hi;

            MatchBand mb;
            mb.gainDb = juce::jlimit (-18.0, 18.0, (double) peakV);
            if (lo == 0 && peakI < kGridPoints / 6)
            {
                mb.type = BandType::LowShelf; mb.freq = gridFreq (hi); mb.q = 0.7;
            }
            else if (hi == kGridPoints - 1 && peakI > kGridPoints * 5 / 6)
            {
                mb.type = BandType::HighShelf; mb.freq = gridFreq (lo); mb.q = 0.7;
            }
            else
            {
                const double bw = std::max (0.15, std::log2 (gridFreq (hi) / gridFreq (lo)));
                const double p = std::pow (2.0, bw);
                mb.type = BandType::Bell; mb.freq = gridFreq (peakI);
                mb.q = juce::jlimit (0.3, 8.0, std::sqrt (p) / (p - 1.0));
            }
            auto g = respOf (mb);
            for (int i = 0; i < kGridPoints; ++i) residual[(size_t) i] -= g[(size_t) i];
            bands.push_back (mb);
            resp.push_back (g);
        }

        // Refinement: least-squares re-fit of each band's gain against what the others leave,
        // using the band's own curve shape (dB response scales almost linearly with gain).
        for (int pass = 0; pass < 6; ++pass)
            for (size_t b = 0; b < bands.size(); ++b)
            {
                for (int i = 0; i < kGridPoints; ++i) residual[(size_t) i] += resp[b][(size_t) i];
                if (std::abs (bands[b].gainDb) > 1e-3)
                {
                    double num = 0, den = 0;
                    for (int i = 0; i < kGridPoints; ++i)
                    {
                        const double shape = resp[b][(size_t) i] / bands[b].gainDb;
                        num += residual[(size_t) i] * shape; den += shape * shape;
                    }
                    if (den > 1e-9) bands[b].gainDb = juce::jlimit (-18.0, 18.0, num / den);
                }
                resp[b] = respOf (bands[b]);
                for (int i = 0; i < kGridPoints; ++i) residual[(size_t) i] -= resp[b][(size_t) i];
            }

        bands.erase (std::remove_if (bands.begin(), bands.end(), [] (const MatchBand& m) { return std::abs (m.gainDb) < 0.25; }), bands.end());
        return bands;
    }
}
