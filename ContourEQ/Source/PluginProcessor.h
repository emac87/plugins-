#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include "EqDsp.h"
#include "Spectrum.h"

class ContourProcessor : public juce::AudioProcessor
{
public:
    ContourProcessor();
    ~ContourProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "Contour EQ"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // ---- shared with the editor ----
    static juce::String pid (int band, const char* name) { return "b" + juce::String (band + 1) + "_" + name; }
    juce::AudioProcessorValueTreeState apvts;
    juce::UndoManager undo;

    contour::SpectrumTap preTap, postTap, scTap;
    std::atomic<int> soloBand { -1 };
    std::atomic<float> dynGainDb[contour::kMaxBands] {};   // live dynamic offset per band (dB)

    double getSampleRateSafe() const { return sampleRateHz > 0 ? sampleRateHz : 48000.0; }
    bool sidechainActive() const;

    // Band spec as the user set it (gain scale applied; no dynamics).
    contour::BandSpec getSpec (int band) const;
    bool isBandOn (int band) const { return raw[band].on->load() > 0.5f; }
    int findFreeBand() const;
    void setParam (const juce::String& id, float realValue);

    // A/B compare: two complete settings; switching keeps both.
    void switchAB();
    void copyCurrentToOther();
    int currentAB = 0;

    void applyMatch (const std::vector<contour::MatchBand>& bands);

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    struct RawBand
    {
        std::atomic<float>* on {}, * type {}, * freq {}, * gain {}, * q {}, * slope {}, * place {},
                          * dyn {}, * thr {}, * range {}, * att {}, * rel {}, * sc {};
    };
    RawBand raw[contour::kMaxBands];
    std::atomic<float>* gainScale {}, * outGain {}, * outPan {};

    struct BandState
    {
        contour::Biquad filt[2][contour::kMaxSections];
        contour::BandDesign design;
        contour::BandSpec lastSpec;
        bool wasOn = false;
        int lastType = -1, lastSlope = -1;
        juce::SmoothedValue<double, juce::ValueSmoothingTypes::Multiplicative> freq { 1000.0 }, q { 0.707 };
        juce::SmoothedValue<double, juce::ValueSmoothingTypes::Linear> gain { 0.0 };
        contour::Biquad det;
        double env = 0.0;
    };
    BandState st[contour::kMaxBands];

    contour::Biquad soloFilt[2];
    juce::AudioBuffer<float> dry;
    juce::SmoothedValue<float> outSmoothed { 1.0f };
    juce::ValueTree abSlot[2];
    double sampleRateHz = 48000.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ContourProcessor)
};
