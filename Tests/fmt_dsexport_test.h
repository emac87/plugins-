#pragma once
// Round trip for the DecentSampler exporter and zip unpacking: write two sine WAVs, save a two-zone
// preset, load it back and compare, then zip the folder and check unpackInstrumentZip finds the preset.
// Header-only; prints PASS/FAIL lines and returns the overall result.
#include "MoreFormats.h"
#include <cmath>
#include <cstdio>
#include <memory>

namespace omni_test_detail
{
    inline bool writeSine (const juce::File& f, double freq, double seconds = 0.1)
    {
        f.deleteFile();
        auto stream = f.createOutputStream();
        if (stream == nullptr) return false;

        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (stream.get(), 44100.0, 1, 16,
                                                                               juce::StringPairArray(), 0));
        if (writer == nullptr) return false;
        stream.release();   // the writer owns the stream from here on

        const int numSamples = (int) (44100.0 * seconds);
        juce::AudioBuffer<float> buf (1, numSamples);
        for (int i = 0; i < numSamples; ++i)
            buf.setSample (0, i, 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * freq * i / 44100.0));
        return writer->writeFromAudioSampleBuffer (buf, 0, numSamples);
    }
}

inline bool testDsExport (const juce::File& tempDir)
{
    bool allOk = true;
    auto check = [&allOk] (bool ok, const char* what)
    {
        std::printf ("%s: %s\n", ok ? "PASS" : "FAIL", what);
        allOk = allOk && ok;
    };

    // Two tiny WAVs, loaded through the cache like a real instrument would be.
    tempDir.createDirectory();
    const auto wavA = tempDir.getChildFile ("sine_a.wav");
    const auto wavB = tempDir.getChildFile ("sine_b.wav");
    check (omni_test_detail::writeSine (wavA, 220.0) && omni_test_detail::writeSine (wavB, 440.0),
           "wrote two test WAVs");

    omni::SampleCache cache;
    juce::String err;
    auto sampleA = cache.get (wavA, err);
    auto sampleB = cache.get (wavB, err);
    check (sampleA != nullptr && sampleB != nullptr, "loaded both WAVs through SampleCache");
    if (sampleA == nullptr || sampleB == nullptr) return false;

    // Zone A: round robin 0, looped. Zone B: round robin position 1, not looped.
    omni::KeysMap keys;
    keys.name = "DsExportTest";
    omni::Zone za;
    za.sample = sampleA;
    za.loKey = 36; za.hiKey = 47; za.root = 40; za.rrPos = 0;
    za.tuneCents = 25.0f; za.gainDb = -6.0f; za.pan = 0.5f;
    za.loop = true; za.loopStart = 100; za.loopEnd = 3000; za.start = 0;
    omni::Zone zb;
    zb.sample = sampleB;
    zb.loKey = 48; zb.hiKey = 60; zb.root = 54; zb.rrPos = 1;
    keys.zones = { za, zb };

    const auto preset = tempDir.getChildFile ("export.dspreset");
    const bool saved = omni::saveDecentSampler (keys, preset, err);
    check (saved && preset.existsAsFile(), "saved export.dspreset");
    if (! saved) return false;

    omni::KeysMap back;
    const bool loaded = omni::loadDecentSampler (preset, cache, back, err);
    check (loaded && back.zones.size() == 2, "reloaded the preset with 2 zones");
    if (! loaded || back.zones.size() != 2) return false;

    const auto& ra = back.zones[0];
    const auto& rb = back.zones[1];
    check (ra.loKey == 36 && ra.hiKey == 47 && ra.root == 40, "zone A key range and root survive");
    check (rb.loKey == 48 && rb.hiKey == 60 && rb.root == 54, "zone B key range and root survive");
    check (ra.sample != nullptr && ra.sample->file.getFileName() == "sine_a.wav", "zone A still points at its sample");
    check (rb.sample != nullptr && rb.sample->file.getFileName() == "sine_b.wav", "zone B still points at its sample");
    check (ra.loop && ra.loopStart == 100 && ra.loopEnd == 3000, "zone A loop points survive");
    check (! rb.loop, "zone B stays unlooped");
    check (ra.rrPos == 0 && rb.rrPos == 1, "round-robin positions survive");
    check (std::abs (ra.tuneCents - 25.0f) < 0.5f, "tuning survives (cents)");
    check (std::abs (ra.gainDb + 6.0f) < 0.1f, "volume survives (dB)");
    check (std::abs (ra.pan - 0.5f) < 0.01f, "pan survives");

    // Zip the test folder (the preset and both WAVs) and unpack it again.
    const auto zipName = "DsExportTest-" + juce::String (juce::Time::currentTimeMillis());
    const auto zipFile = tempDir.getChildFile (zipName + ".zip");
    juce::ZipFile::Builder builder;
    for (auto& f : tempDir.findChildFiles (juce::File::findFiles, true))
        if (! f.hasFileExtension ("zip")) builder.addFile (f, 3, f.getRelativePathFrom (tempDir));

    bool zipped = false;
    {
        juce::FileOutputStream out (zipFile);
        zipped = out.openedOk() && builder.writeToStream (out, nullptr);
    }
    check (zipped, "zipped the test folder");

    juce::String unpackErr;
    const auto found = omni::unpackInstrumentZip (zipFile, unpackErr);
    check (found.existsAsFile() && found.hasFileExtension ("dspreset"), "unpackInstrumentZip returns a .dspreset");

    // Remove the unpacked copy again; it lives under a name unique to this run.
    omni::extractedSamplesFolder (zipName).deleteRecursively();

    std::printf ("%s: DsExport round trip\n", allOk ? "PASS" : "FAIL");
    return allOk;
}
