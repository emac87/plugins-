#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include "Model.h"
#include "Formats.h"
#include "Engine.h"
#include "Streamer.h"
#include "MoreFormats.h"

class OmniProcessor : public juce::AudioProcessor, private juce::Timer
{
public:
    OmniProcessor();
    ~OmniProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "Omnisampler"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 2.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // ---- message thread ----
    juce::AudioProcessorValueTreeState apvts;
    juce::MidiKeyboardState keyboardState;   // on-screen keyboard (Keys only); also lights keys played over MIDI
    juce::MidiKeyboardState padState;        // on-screen pads (Pads only)
    omni::SampleCache cache;
    omni::Streamer streamer;
    omni::Engine engine;

    omni::InstrumentPtr getInstrument() const { return current; }
    void edit (const std::function<void (omni::Instrument&)>& fn);   // copy, change, swap in
    void replaceInstrument (omni::Instrument);

    // Loading. Each returns a message for the user ("" = fine).
    juce::String loadFilesToPad (int pad, const juce::Array<juce::File>& files, omni::DropMode mode);
    juce::String loadFilesToKeys (const juce::Array<juce::File>& files, bool replace);
    juce::String loadInstrumentFile (const juce::File& f);   // .sfz / .dspreset / .dslibrary / .sf2 / .exs / .adv / .nki / .zip into Keys
    juce::String saveKeysAsDecentSampler (const juce::File& f);
    static bool canLoadInstrument (const juce::File& f) { return omni::isInstrumentFile (f) || omni::isMoreFormatsFile (f); }
    static juce::String instrumentPatterns() { return "*.sfz;*.dspreset;*.dslibrary;*.sf2;*.exs;*.adv;*.nki;*.zip"; }

    // Disk streaming of long samples (applies to samples loaded after the change).
    void setStreaming (bool on) { cache.streaming = on; }
    bool isStreaming() const { return cache.streaming.load(); }
    int streamUnderruns() const { return streamer.underruns.load(); }
    static juce::String outputName (int i) { return i == 0 ? juce::String ("Main") : "Out " + juce::String (i + 1); }
    juce::String loadKit (const juce::File& f);
    juce::String saveKit (const juce::File& f);

    int padsChannel() const { return (int) apvts.getRawParameterValue ("padsch")->load(); }
    int keysChannel() const { return (int) apvts.getRawParameterValue ("keysch")->load(); }
    std::atomic<int> voiceCount { 0 };
    // On-screen pads and keys: true = how hard you hit depends on where you tap; false = fixed velocity.
    std::atomic<bool> touchVelocity { false };
    bool hasOutputPicker() const { return wrapperType != wrapperType_Standalone; }

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    static BusesProperties makeBuses();
    void timerCallback() override;
    void swapIn (omni::InstrumentPtr p);

    omni::InstrumentPtr current;
    juce::SpinLock instLock;
    std::vector<std::pair<omni::InstrumentPtr, juce::uint32>> graveyard;
    juce::CriticalSection graveLock;
    juce::SmoothedValue<float> master { 1.0f };
    juce::MidiBuffer localPads, localKeys;   // taps on the on-screen pads / keyboard for this block

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OmniProcessor)
};
