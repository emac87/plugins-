#include "TestCommon.h"
#include "PluginProcessor.h"
#include "PluginEditor.h"

static juce::File dataDir() { return juce::File (__FILE__).getParentDirectory().getChildFile ("data"); }

struct Result { double rmsL, rmsR; bool finite; };
static Result run (CaptureRackProcessor& p, double sr, float amp, int seconds10 = 10, double freq = 220)
{
    const int block = 256, total = (int) (sr * seconds10 / 10);
    juce::AudioBuffer<float> buf (2, block);
    juce::MidiBuffer midi;
    double sl = 0, sr2 = 0; bool finite = true; int n = 0;
    for (int start = 0; start < total; start += block)
    {
        for (int i = 0; i < block; ++i) { const float s = amp * (float) std::sin (2 * juce::MathConstants<double>::pi * freq * (start + i) / sr); buf.setSample (0, i, s); buf.setSample (1, i, s); }
        p.processBlock (buf, midi);
        for (int i = 0; i < block; ++i) { const float a = buf.getSample (0, i), b = buf.getSample (1, i); finite = finite && std::isfinite (a) && std::isfinite (b);
            if (start > total / 2) { sl += a * a; sr2 += b * b; ++n; } }
    }
    return { std::sqrt (sl / std::max (1, n)), std::sqrt (sr2 / std::max (1, n)), finite };
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    juce::File out (argc > 1 ? argv[1] : "/tmp/test-output"); out.createDirectory();
    std::printf ("Capture Rack\n");
    CaptureRackProcessor p;
    auto tmpLib = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("rack-lib");
    tmpLib.deleteRecursively();
    p.library.setRoot (tmpLib);

    p.prepareToPlay (48000, 256);
    juce::String err;
    CHECK (p.loadSlot (0, dataDir().getChildFile ("A2.nam"), err), ("loads an A2 model " + err).toRawUTF8());
    std::printf ("    slot 1: %s\n", p.slotInfo (0).toRawUTF8());
    CHECK (p.slotInfo (0).startsWith ("A2"), "labels the A2 model as A2");
    auto r48 = run (p, 48000, 0.1f);
    std::printf ("    A2 @48k: out rms %.4f\n", r48.rmsL);
    CHECK (r48.finite && r48.rmsL > 0.001 && std::abs (r48.rmsL - 0.0707) > 0.005, "A2 model changes the sound at 48 kHz");

    p.prepareToPlay (44100, 256);
    auto r44 = run (p, 44100, 0.1f);
    std::printf ("    A2 @44.1k: out rms %.4f  [%s]\n", r44.rmsL, p.slotInfo (0).toRawUTF8());
    CHECK (p.slotInfo (0).contains ("resampled"), "44.1 kHz host runs the 48 kHz model through the resampler");
    CHECK (r44.finite && r44.rmsL > 0.001, "A2 model makes sound at 44.1 kHz through the resampler");
    CHECK (std::abs (20 * std::log10 (r44.rmsL / r48.rmsL)) < 2.0, "resampled level matches the native-rate level within 2 dB");

    p.prepareToPlay (48000, 256);
    CHECK (p.loadSlot (1, dataDir().getChildFile ("lstm.nam"), err), "loads an LSTM model in slot 2");
    auto series = run (p, 48000, 0.1f);
    setParam (p.apvts, "routing", 1);
    setParam (p.apvts, "s1_pan", -1); setParam (p.apvts, "s2_pan", 1);
    auto par = run (p, 48000, 0.1f);
    std::printf ("    series %.4f | parallel L %.4f R %.4f\n", series.rmsL, par.rmsL, par.rmsR);
    CHECK (series.finite && series.rmsL > 0.0005, "series chain makes sound");
    CHECK (par.finite && par.rmsL > 0.0005 && par.rmsR > 0.0005 && std::abs (par.rmsL - par.rmsR) > 1e-4, "parallel slots pan to separate sides");
    setParam (p.apvts, "routing", 0);
    setParam (p.apvts, "s1_pan", 0); setParam (p.apvts, "s2_pan", 0);

    // Gate: a very quiet input with the gate at -40 dB should come out silent.
    p.clearSlot (1);
    setParam (p.apvts, "s1_gate", -40);
    auto gated = run (p, 48000, 0.0005f);
    std::printf ("    gated quiet input rms %.6f\n", gated.rmsL);
    CHECK (gated.rmsL < 1e-4, "noise gate silences input below threshold");
    setParam (p.apvts, "s1_gate", -100);

    // IR: a short dark decay should lower the highs of a 5 kHz tone.
    {
        auto irFile = tmpLib.getChildFile ("IRs/test-ir.wav");
        juce::AudioBuffer<float> ir (1, 4800);
        juce::Random rnd (1); float lp = 0;
        for (int i = 0; i < ir.getNumSamples(); ++i) { lp += 0.05f * ((rnd.nextFloat() * 2 - 1) - lp); ir.setSample (0, i, lp * std::exp (-i / 600.0f)); }
        writeWav (irFile, ir, 48000);
        p.clearSlot (0); p.clearSlot (1);
        auto dry = run (p, 48000, 0.1f, 10, 5000);
        CHECK (p.loadIR (irFile, err), "loads a WAV IR");
        for (int i = 0; i < 40 && true; ++i) { run (p, 48000, 0.1f, 1, 5000); juce::Thread::sleep (20); }
        auto wet = run (p, 48000, 0.1f, 10, 5000);
        std::printf ("    5 kHz tone: dry %.4f, through dark IR %.4f\n", dry.rmsL, wet.rmsL);
        CHECK (wet.finite && wet.rmsL < dry.rmsL * 0.7, "the IR filters the signal");
    }

    // Library.
    {
        auto imported = p.library.import ({ dataDir().getChildFile ("A2.nam") }, "Amps");
        CHECK (imported.size() == 1 && imported[0].existsAsFile(), "imports a capture into a category folder");
        p.library.setFavourite (imported[0], true);
        p.library.setInfo (imported[0], "Tester", "TONE3000", "cc-by", "https://example.com");
        rack::Library again; again.setRoot (tmpLib);
        auto favs = again.items ("Favourites", {});
        CHECK (favs.size() == 1 && favs[0].author == "Tester", "favourites and author survive a reload");
        CHECK (again.items ("", "a2").size() == 1, "search finds it");
    }

    // State round trip.
    {
        p.loadSlot (2, dataDir().getChildFile ("lstm.nam"), err);
        p.moveSlot (2, -2);
        juce::MemoryBlock mb; p.getStateInformation (mb);
        CaptureRackProcessor q; q.prepareToPlay (48000, 256); q.setStateInformation (mb.getData(), (int) mb.getSize());
        CHECK (q.slotName (2) == "lstm" && q.getOrder()[0] == 2, "saved state restores models and chain order");
    }

    p.loadSlot (0, dataDir().getChildFile ("A2.nam"), err);
    snapshot (p, out.getChildFile ("capturerack.png"));
    std::printf (failures ? "FAILURES: %d\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
