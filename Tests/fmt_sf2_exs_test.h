#pragma once
// SoundFont 2 and EXS24 import tests. dataDir = folder with the sf2exs fixtures
// (test.sf2, test_be.exs, test_le.exs, sineB.wav, Samples/sineA.wav); regenerate with a generator script
// if the layout ever changes (the .exs files are hand-built to the ConvertWithMoss block layout).
#include "MoreFormats.h"
#include <cmath>
#include <cstdio>

inline bool testSf2Exs (const juce::File& dataDir)
{
    int failed = 0;
    auto check = [&failed] (const char* what, bool ok)
    {
        std::printf ("  %s  %s\n", ok ? "PASS" : "FAIL", what);
        if (! ok) ++failed;
    };
    auto near = [] (float a, float b, float tol) { return std::abs (a - b) <= tol; };

    std::printf ("SF2 / EXS import\n");

    // ---- SoundFont ----
    {
        omni::SampleCache cache;
        omni::KeysMap km;
        juce::String err;
        const bool ok = omni::loadSf2 (dataDir.getChildFile ("test.sf2"), cache, km, err);
        check ("sf2 loads", ok);
        if (ok)
        {
            check ("sf2 no warning", err.isEmpty());
            check ("sf2 name from INAM", km.name == "TestBank");
            check ("sf2 has 2 zones", km.zones.size() == 2);
            if (km.zones.size() == 2)
            {
                const auto& a = km.zones[0];
                const auto& b = km.zones[1];
                check ("sf2 zone 0 keys 0-71", a.loKey == 0 && a.hiKey == 71);
                check ("sf2 zone 1 keys 72-127", b.loKey == 72 && b.hiKey == 127);
                check ("sf2 zone 0 root 60 (original pitch)", a.root == 60);
                check ("sf2 zone 1 root 74 (overridingRootKey)", b.root == 74);
                check ("sf2 zone 0 loops 200..1800", a.loop && a.loopStart == 200 && a.loopEnd == 1800);
                check ("sf2 zone 1 no loop", ! b.loop);
                check ("sf2 zone 1 fine tune 10 cents", near (b.tuneCents, 10.0f, 0.01f));
                check ("sf2 samples are real audio", a.sample != nullptr && a.sample->length() == 2000 && b.sample != nullptr && b.sample->length() == 2000);
                check ("sf2 sample rate 22050", a.sample != nullptr && near ((float) a.sample->sampleRate, 22050.0f, 0.5f));
            }
            check ("sf2 release from global zone (~0.5 s)", near (km.voicing.env.release, 0.5f, 0.03f));
        }

        omni::KeysMap bad;
        juce::String err2;
        check ("sf2 rejects a non-SoundFont", ! omni::loadSf2 (dataDir.getChildFile ("sineB.wav"), cache, bad, err2) && err2.isNotEmpty());
    }

    // ---- EXS24 (big- and little-endian) ----
    for (const char* fileName : { "test_be.exs", "test_le.exs" })
    {
        const juce::String tag (fileName);
        omni::SampleCache cache;
        omni::KeysMap km;
        juce::String err;
        const bool ok = omni::loadExs (dataDir.getChildFile (fileName), cache, km, err);
        check ((tag + " loads").toRawUTF8(), ok);
        if (! ok) { std::printf ("    error: %s\n", err.toRawUTF8()); continue; }
        check ((tag + " no warning").toRawUTF8(), err.isEmpty());
        check ((tag + " has 2 zones").toRawUTF8(), km.zones.size() == 2);
        if (km.zones.size() != 2) continue;
        const auto& a = km.zones[0];
        const auto& b = km.zones[1];
        check ((tag + " zone 0 keys 0-71 root 60").toRawUTF8(), a.loKey == 0 && a.hiKey == 71 && a.root == 60);
        check ((tag + " zone 1 keys 72-127 root 74").toRawUTF8(), b.loKey == 72 && b.hiKey == 127 && b.root == 74);
        check ((tag + " zone 0 loops 200..1800").toRawUTF8(), a.loop && a.loopStart == 200 && a.loopEnd == 1800);
        check ((tag + " zone 1 no loop").toRawUTF8(), ! b.loop);
        check ((tag + " zone 1 fine tune 10 cents").toRawUTF8(), near (b.tuneCents, 10.0f, 0.01f));
        check ((tag + " velocity range 1-127").toRawUTF8(), a.loVel == 1 && a.hiVel == 127);
        check ((tag + " samples found (Samples/ folder and next to file)").toRawUTF8(), a.sample != nullptr && b.sample != nullptr);
        check ((tag + " envelope from params").toRawUTF8(), near (km.voicing.env.decay, 0.498f, 0.01f) && near (km.voicing.env.release, 0.098f, 0.01f)
                                                              && near (km.voicing.env.sustain, 1.0f, 0.01f));
    }

    // ---- EXS with missing samples: a clear failure, no crash ----
    {
        const auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("omni_exs_missing_" + juce::String (juce::Random::getSystemRandom().nextInt (1000000)));
        const auto deep = tmp.getChildFile ("a").getChildFile ("b");   // the .exs's parent folder holds nothing else
        deep.createDirectory();
        const auto copy = deep.getChildFile ("lonely.exs");
        dataDir.getChildFile ("test_be.exs").copyFileTo (copy);
        omni::SampleCache cache;
        omni::KeysMap km;
        juce::String err;
        check ("exs with no samples fails with a message", ! omni::loadExs (copy, cache, km, err) && err.isNotEmpty());
        // Truncated file must not crash.
        juce::MemoryBlock mb;
        dataDir.getChildFile ("test_be.exs").loadFileAsData (mb);
        copy.replaceWithData (mb.getData(), mb.getSize() / 2);
        omni::KeysMap km2;
        juce::String err2;
        (void) omni::loadExs (copy, cache, km2, err2);
        check ("truncated exs does not crash", true);
        // Garbage.
        copy.replaceWithText ("this is not an exs file at all, it is much too short to hold even a single block header okay?");
        omni::KeysMap km3;
        juce::String err3;
        check ("garbage is rejected", ! omni::loadExs (copy, cache, km3, err3) && err3.isNotEmpty());
        tmp.deleteRecursively();
    }

    std::printf ("SF2/EXS: %s\n", failed == 0 ? "ALL PASS" : "FAILURES");
    return failed == 0;
}
