#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include "NamSlot.h"
#include "Library.h"
#include "Tone3000.h"

class CaptureRackProcessor : public juce::AudioProcessor
{
public:
    static constexpr int kSlots = 3;

    CaptureRackProcessor();
    ~CaptureRackProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "Capture Rack"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.5; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // ---- used by the editor (message thread) ----
    juce::AudioProcessorValueTreeState apvts;
    rack::Library library;
    rack::Tone3000 tone3000;

    bool loadSlot (int slot, const juce::File& f, juce::String& error);
    void clearSlot (int slot);
    juce::String slotName (int slot) const;
    juce::String slotInfo (int slot) const;
    juce::File slotFile (int slot) const;

    bool loadIR (const juce::File& f, juce::String& error);
    void clearIR();
    juce::File irFile() const { return currentIR; }

    std::array<int, kSlots> getOrder() const;
    void moveSlot (int slot, int delta);   // reorder in the series chain

    std::atomic<float> inMeter { 0 }, outMeter { 0 };
    std::atomic<float> gateGain[kSlots] {};

    static juce::String sid (int slot, const char* n) { return "s" + juce::String (slot + 1) + "_" + n; }

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void prepareFilters();

    juce::SpinLock slotLock[kSlots];
    std::unique_ptr<rack::LoadedModel> models[kSlots];
    juce::File slotFiles[kSlots];
    std::atomic<int> order[kSlots] { 0, 1, 2 };
    rack::NoiseGate gates[kSlots];

    juce::dsp::Convolution conv { juce::dsp::Convolution::Latency { 0 } };
    juce::File currentIR;
    std::atomic<bool> irLoaded { false };

    using Filter = juce::dsp::IIR::Filter<float>;
    using Coefs = juce::dsp::IIR::Coefficients<float>;
    Filter lowCut[2], highCut[2], bass[2], mid[2], treble[2];
    float lastLow = -1, lastHigh = -1, lastBass = 99, lastMid = 99, lastTreble = 99;

    juce::AudioBuffer<float> mono, work, parallelSum, dryStereo, gateGains;
    double sampleRateHz = 48000.0;
    int maxBlock = 512;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CaptureRackProcessor)
};
