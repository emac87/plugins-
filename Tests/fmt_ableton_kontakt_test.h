#pragma once
// Ableton (.adv) and Kontakt (.nki) import tests against the small synthetic fixtures in Tests/data/ablkon.
// Header-only; prints PASS/FAIL lines and returns the overall result.
#include "MoreFormats.h"
#include <cmath>
#include <cstdio>

inline bool testAbletonKontakt (const juce::File& dataDir)
{
    bool allOk = true;
    auto check = [&allOk] (bool ok, const char* what)
    {
        std::printf ("%s: %s\n", ok ? "PASS" : "FAIL", what);
        allOk = allOk && ok;
    };
    auto near = [] (double a, double b, double tol) { return std::abs (a - b) <= tol; };

    const auto root = dataDir.getChildFile ("ablkon");

    // ---------------------------------------------------------------- Ableton
    {
        omni::SampleCache cache;
        omni::KeysMap km;
        juce::String err;
        const bool ok = omni::loadAbleton (root.getChildFile ("Presets/Instruments/Sampler/AblTest.adv"), cache, km, err);
        check (ok && km.zones.size() == 2, "ableton: two zones load");
        if (ok && km.zones.size() == 2)
        {
            const auto& a = km.zones[0];
            const auto& b = km.zones[1];
            check (a.loKey == 0 && a.hiKey == 59 && a.root == 48, "ableton: zone 1 key range and root");
            check (near (a.tuneCents, 10.0, 0.01) && near (a.gainDb, -6.02, 0.05) && near (a.pan, -0.5, 0.01), "ableton: detune, volume, pan");
            check (b.loKey == 60 && b.hiKey == 127 && b.root == 67, "ableton: zone 2 key range and root");
            check (b.loop && b.loopStart == 100 && b.loopEnd == 800, "ableton: sustain loop");
            check (a.sample != nullptr && a.sample->length() == 1500, "ableton: sample resolved by relative path");
            check (near (km.voicing.env.attack, 0.005, 1.0e-4) && near (km.voicing.env.decay, 0.2, 1.0e-3)
                   && near (km.voicing.env.sustain, 0.8, 1.0e-3) && near (km.voicing.env.release, 0.3, 1.0e-3), "ableton: amp envelope");
        }

        omni::SampleCache cache2;
        omni::KeysMap rr;
        juce::String err2;
        const bool okOld = omni::loadAbleton (root.getChildFile ("AblOld.adv"), cache2, rr, err2);
        check (okOld && rr.zones.size() == 2 && rr.zones[0].rrPos == 1 && rr.zones[1].rrPos == 2,
               "ableton: sample found by name, selector ranges become round robin");

        omni::KeysMap miss;
        juce::String err3;
        check (! omni::loadAbleton (root.getChildFile ("AblMissing.adv"), cache2, miss, err3) && err3.isNotEmpty(), "ableton: missing samples fail with a message");
        omni::KeysMap junk;
        juce::String err4;
        check (! omni::loadAbleton (root.getChildFile ("AblGarbage.adv"), cache2, junk, err4) && err4.isNotEmpty(), "ableton: garbage fails with a message");
    }

    // ---------------------------------------------------------------- Kontakt
    const auto kdir = root.getChildFile ("Kontakt");
    {
        omni::SampleCache cache;
        omni::KeysMap km;
        juce::String err;
        const bool ok = omni::loadKontakt (kdir.getChildFile ("Test K1.nki"), cache, km, err);
        check (ok && km.zones.size() == 2, "kontakt 1: release group skipped, two zones");
        if (ok && km.zones.size() == 2)
        {
            check (km.zones[0].loKey == 0 && km.zones[0].hiKey == 59 && km.zones[0].root == 48 && near (km.zones[0].pan, -0.5, 0.01), "kontakt 1: range, root, pan");
            check (km.zones[1].loop && km.zones[1].loopStart == 100, "kontakt 1: loop");
            check (near (km.voicing.env.attack, 0.01, 1.0e-3) && near (km.voicing.env.sustain, 0.7, 1.0e-2), "kontakt 1: amp envelope");
        }
    }
    {
        omni::SampleCache cache;
        omni::KeysMap km;
        juce::String err;
        const bool ok = omni::loadKontakt (kdir.getChildFile ("Test K2.nki"), cache, km, err);
        check (ok && km.zones.size() == 4 && err.isNotEmpty(), "kontakt 2: four zones plus a missing-sample warning");
        if (ok && km.zones.size() == 4)
        {
            check (near (km.zones[0].tuneCents, 150.0, 0.5) && km.zones[0].pan < -0.2, "kontakt 2: group and zone tuning, pan");
            check (km.zones[1].sample != nullptr && km.zones[1].sample->length() == 1500 && km.zones[1].loop, "kontakt 2: NCW sample decoded, loop");
            check (km.zones[2].rrPos == 1 && km.zones[3].rrPos == 2, "kontakt 2: round robin groups");
        }
    }
    for (const char* file : { "Test K42.nki", "Test K5.nki" })
    {
        omni::SampleCache cache;
        omni::KeysMap km;
        juce::String err;
        const bool ok = omni::loadKontakt (kdir.getChildFile (file), cache, km, err);
        const auto tag = juce::String (file) + ": ";
        check (ok && km.zones.size() == 4, (tag + "four zones (release and missing skipped)").toRawUTF8());
        if (ok && km.zones.size() == 4)
        {
            check (km.zones[0].loKey == 0 && km.zones[0].hiKey == 59 && km.zones[0].root == 48 && near (km.zones[0].tuneCents, 50.0, 0.5), (tag + "range, root, tune").toRawUTF8());
            check (km.zones[1].loop && km.zones[1].loopStart == 100 && km.zones[1].root == 67, (tag + "loop").toRawUTF8());
            check (km.zones[2].rrPos == 1 && km.zones[3].rrPos == 2, (tag + "round robin").toRawUTF8());
        }
    }

    const juce::String expected = juce::String::fromUTF8 ("This Kontakt instrument is encrypted (Kontakt Player library) \xE2\x80\x94 it can only be opened in Kontakt.");
    for (const char* file : { "Test Encrypted K5.nki", "Test Library K2.nki" })
    {
        omni::SampleCache cache;
        omni::KeysMap km;
        juce::String err;
        const bool ok = omni::loadKontakt (kdir.getChildFile (file), cache, km, err);
        check (! ok && err == expected, (juce::String (file) + ": encrypted fails with the exact message").toRawUTF8());
    }
    {
        omni::SampleCache cache;
        omni::KeysMap km;
        juce::String err;
        check (! omni::loadKontakt (kdir.getChildFile ("Test Garbage.nki"), cache, km, err) && err.isNotEmpty(), "kontakt: garbage fails with a message");
    }
    return allOk;
}
