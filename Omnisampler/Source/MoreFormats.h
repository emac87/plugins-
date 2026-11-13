#pragma once
// More instrument formats in (SoundFont, EXS24, Ableton Sampler/Simpler, unlocked Kontakt) and
// DecentSampler out. Each loader fills a KeysMap with zones (samples come from the SampleCache,
// or from samples the loader writes into a cache folder) and returns false + error on failure.
// A non-empty error with true means "loaded, but with a warning" (e.g. some samples missing).
#include "Formats.h"

namespace omni
{
    // Where loaders may write extracted samples (SF2 sample data, Kontakt monoliths, zips).
    juce::File extractedSamplesFolder (const juce::String& instrumentName);

    bool loadSf2 (const juce::File& f, SampleCache&, KeysMap& out, juce::String& error);          // .sf2 (first preset)
    bool loadExs (const juce::File& f, SampleCache&, KeysMap& out, juce::String& error);          // .exs (EXS24 / Logic Sampler)
    bool loadAbleton (const juce::File& f, SampleCache&, KeysMap& out, juce::String& error);      // .adv (Sampler / Simpler preset), .adg drum rack not required
    bool loadKontakt (const juce::File& f, SampleCache&, KeysMap& out, juce::String& error);      // .nki, unlocked only
    bool saveDecentSampler (const KeysMap& keys, const juce::File& f, juce::String& error);       // .dspreset next to (or pointing at) the samples

    // Zips of instruments: unpack into extractedSamplesFolder and return the first instrument
    // file inside (sfz, dspreset, sf2, exs, adv, nki), or File() + error.
    juce::File unpackInstrumentZip (const juce::File& zip, juce::String& error);

    inline bool isMoreFormatsFile (const juce::File& f) { return f.hasFileExtension ("sf2;exs;adv;nki;zip"); }
}
