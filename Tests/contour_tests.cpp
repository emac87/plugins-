#include "TestCommon.h"
#include "PluginProcessor.h"
#include "PluginEditor.h"

using namespace contour;

static double gainAt (ContourProcessor& p, double freq, float amp = 0.1f, bool sideOnly = false)
{
    const double sr = 48000; const int block = 512, total = 48000;
    p.prepareToPlay (sr, block);
    juce::AudioBuffer<float> buf (2, block);
    juce::MidiBuffer midi;
    double inS = 0, outS = 0; int counted = 0;
    for (int start = 0; start < total; start += block)
    {
        for (int i = 0; i < block; ++i)
        {
            const float s = amp * (float) std::sin (2 * kPi * freq * (start + i) / sr);
            buf.setSample (0, i, s); buf.setSample (1, i, sideOnly ? -s : s);
        }
        double in = 0; for (int i = 0; i < block; ++i) in += buf.getSample (0, i) * buf.getSample (0, i);
        p.processBlock (buf, midi);
        if (start > total / 2) { inS += in; for (int i = 0; i < block; ++i) outS += buf.getSample (0, i) * buf.getSample (0, i); ++counted; }
    }
    return 10 * std::log10 (outS / inS);
}

static void resetBands (ContourProcessor& p) { for (int b = 0; b < kMaxBands; ++b) setParam (p.apvts, ContourProcessor::pid (b, "on"), 0); }

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    juce::File out (argc > 1 ? argv[1] : "/tmp/test-output"); out.createDirectory();
    ContourProcessor p;
    std::printf ("Contour EQ\n");

    CHECK (std::abs (gainAt (p, 1000)) < 0.1, "flat when no bands");
    setParam (p.apvts, "b1_on", 1); setParam (p.apvts, "b1_type", 0); setParam (p.apvts, "b1_freq", 1000); setParam (p.apvts, "b1_gain", 12); setParam (p.apvts, "b1_q", 1);
    const double g1k = gainAt (p, 1000), g100 = gainAt (p, 100);
    std::printf ("    bell +12 @1k: %.2f dB at 1k, %.2f dB at 100 Hz\n", g1k, g100);
    CHECK (std::abs (g1k - 12) < 0.3, "bell gives +12 dB at its centre");
    CHECK (std::abs (g100) < 0.6, "bell leaves 100 Hz alone");
    setParam (p.apvts, "gainscale", 50);
    CHECK (std::abs (gainAt (p, 1000) - 6) < 0.3, "gain scale 50% halves it");
    setParam (p.apvts, "gainscale", 100);

    resetBands (p);
    setParam (p.apvts, "b2_on", 1); setParam (p.apvts, "b2_type", 3); setParam (p.apvts, "b2_freq", 200); setParam (p.apvts, "b2_slope", 3); setParam (p.apvts, "b2_q", 0.707f);
    const double c50 = gainAt (p, 50), c200 = gainAt (p, 200), c2k = gainAt (p, 2000);
    std::printf ("    low cut 24 dB/oct @200: 50 Hz %.1f, 200 Hz %.1f, 2 kHz %.2f\n", c50, c200, c2k);
    CHECK (c50 < -44 && c50 > -52, "24 dB/oct low cut: two octaves down is about -48 dB");
    CHECK (std::abs (c200 + 3) < 0.5, "cut is -3 dB at its corner");
    CHECK (std::abs (c2k) < 0.2, "cut passes the highs");
    setParam (p.apvts, "b2_slope", 7);
    CHECK (gainAt (p, 100) < -80, "96 dB/oct cut is steep");

    resetBands (p);
    setParam (p.apvts, "b3_on", 1); setParam (p.apvts, "b3_type", 0); setParam (p.apvts, "b3_freq", 1000); setParam (p.apvts, "b3_gain", 10); setParam (p.apvts, "b3_place", 4);
    CHECK (std::abs (gainAt (p, 1000)) < 0.1, "side-only band leaves a mono signal alone");
    CHECK (std::abs (gainAt (p, 1000, 0.1f, true) - 10) < 0.3, "side-only band boosts the side signal");

    resetBands (p);
    setParam (p.apvts, "b4_on", 1); setParam (p.apvts, "b4_type", 0); setParam (p.apvts, "b4_freq", 1000); setParam (p.apvts, "b4_gain", 0);
    setParam (p.apvts, "b4_dyn", 1); setParam (p.apvts, "b4_thr", -40); setParam (p.apvts, "b4_range", -9);
    const double loud = gainAt (p, 1000, 0.5f), quiet = gainAt (p, 1000, 0.0005f);
    std::printf ("    dynamic band: loud %.2f dB, quiet %.2f dB\n", loud, quiet);
    CHECK (loud < -8 && loud > -9.5, "dynamic band cuts loud signal by its range");
    CHECK (std::abs (quiet) < 0.3, "dynamic band leaves quiet signal alone");

    // Match EQ fitting on a known curve.
    {
        Grid target {};
        BandSpec a; a.type = BandType::Bell; a.freq = 500; a.gainDb = 6; a.q = 1.2;
        BandSpec b; b.type = BandType::HighShelf; b.freq = 6000; b.gainDb = -5; b.q = 0.7;
        auto da = designBand (a, 48000), db = designBand (b, 48000);
        for (int i = 0; i < kGridPoints; ++i) target[(size_t) i] = (float) (magnitudeDb (da, gridFreq (i), 48000) + magnitudeDb (db, gridFreq (i), 48000));
        auto bands = fitMatch (target, 12);
        double worst = 0;
        for (int i = 0; i < kGridPoints; ++i)
        {
            double sum = 0;
            for (auto& m : bands) { BandSpec s; s.type = m.type; s.freq = m.freq; s.gainDb = m.gainDb; s.q = m.q; sum += magnitudeDb (designBand (s, 48000), gridFreq (i), 48000); }
            worst = std::max (worst, std::abs (sum - target[(size_t) i]));
        }
        std::printf ("    match fit: %d bands, worst error %.2f dB\n", (int) bands.size(), worst);
        CHECK (worst < 1.5, "Match EQ fits a known curve within 1.5 dB");
        p.applyMatch (bands);
        int on = 0; for (int i = 0; i < kMaxBands; ++i) on += p.isBandOn (i) ? 1 : 0;
        CHECK (on == (int) bands.size(), "Match EQ writes its bands into the plug-in");
    }

    snapshot (p, out.getChildFile ("contour.png"), [&p] (juce::Component& c)
    {
        auto& ed = dynamic_cast<ContourEditor&> (c);
        ed.selectBand (0);
        for (int i = 0; i < 20; ++i) { juce::AudioBuffer<float> b (2, 2048); juce::MidiBuffer m; juce::Random r;
            for (int s = 0; s < 2048; ++s) { float v = r.nextFloat() * 0.2f - 0.1f; b.setSample (0, s, v); b.setSample (1, s, v); }
            p.processBlock (b, m); p.preTap.tick(); p.postTap.tick(); }
    });
    std::printf (failures ? "FAILURES: %d\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
