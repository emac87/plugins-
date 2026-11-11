#pragma once
// Loading samples and instruments: audio files, SFZ, DecentSampler, kits; plus the smart mapper.
#include "Model.h"
#include <map>

namespace omni
{
    // Loads audio files once and shares them between zones.
    class SampleCache
    {
    public:
        SampleCache() { formats.registerBasicFormats(); }
        SamplePtr get (const juce::File& f, juce::String& error);
        juce::AudioFormatManager formats;
    private:
        std::map<juce::String, std::weak_ptr<const Sample>> cache;
    };

    // Root note from a file name ("Piano_C#3.wav", "bass-A1", "vln_60.wav"); -1 if none.
    int rootFromFileName (const juce::String& name);
    // Fundamental from the audio (YIN); -1 if unclear.
    int detectRoot (const Sample& s);

    // Smart mapping.
    enum class DropMode { VelocityLayers, RoundRobin };
    std::vector<Zone> mapPadFiles (const std::vector<SamplePtr>& samples, DropMode mode);
    std::vector<Zone> mapKeyFiles (const std::vector<SamplePtr>& samples);
    // Re-spread velocity ranges of zones sharing a key range, by loudness.
    void assignVelocityByLoudness (std::vector<Zone*>& zones);

    // Instrument formats. Each returns false and sets error on failure.
    bool loadSfz (const juce::File& f, SampleCache&, KeysMap& out, juce::String& error);
    bool loadDecentSampler (const juce::File& f, SampleCache&, KeysMap& out, juce::String& error);
    bool loadDsLibrary (const juce::File& zip, SampleCache&, KeysMap& out, juce::String& error);
    bool saveSfz (const KeysMap& keys, const juce::File& f, juce::String& error);

    // Kit (.omni) and plug-in state.
    std::unique_ptr<juce::XmlElement> instrumentToXml (const Instrument&);
    Instrument instrumentFromXml (const juce::XmlElement&, SampleCache&, juce::StringArray& missing);

    inline bool isAudioFile (const juce::File& f) { return f.hasFileExtension ("wav;aif;aiff;flac;mp3;ogg"); }
    inline bool isInstrumentFile (const juce::File& f) { return f.hasFileExtension ("sfz;dspreset;dslibrary"); }
}
