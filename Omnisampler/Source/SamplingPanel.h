#pragma once
#include "PluginProcessor.h"
#include "PluginLook.h"

class OmniEditor;

// The Sample page: record from the audio input on the left, the Auto Sampler on the right.
class SamplingPanel : public juce::Component, private juce::Timer
{
public:
    explicit SamplingPanel (OmniEditor&);
    ~SamplingPanel() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    void visibilityChanged() override;
    void refresh();     // the selected pad, the loaded plug-in or the instrument changed
    void news();        // a take or an Auto Sampler run has landed: show what happened

private:
    // A caption and the control beside it.
    struct Row { std::unique_ptr<juce::Label> label; juce::Component* control = nullptr; };
    // Several controls sharing one row, each with a share of the width.
    struct Group : juce::Component
    {
        void add (juce::Component& c, float share) { parts.push_back ({ &c, share }); addAndMakeVisible (c); }
        void resized() override;
        std::vector<std::pair<juce::Component*, float>> parts;
    };
    void addRow (std::vector<Row>& rows, const juce::String& caption, juce::Component& control);
    void layoutRows (std::vector<Row>& rows, juce::Rectangle<int>& area);
    void timerCallback() override;
    void updateEnablement();
    void updateSummary();
    void toggleRecording();
    void toggleAutoSampler();
    void showLibrary();
    omni::AutoSettings autoSettings() const;
    bool sourceIsPlugin() const { return sourceBox.getSelectedId() == 2; }
   #if OMNI_HOSTING
    class PluginWindow;
    void choosePlugin();
    void showPlugin();
    std::unique_ptr<juce::DocumentWindow> pluginWindow;
   #endif

    OmniEditor& ed;
    OmniProcessor& proc;
    juce::Rectangle<int> recArea, autoArea, meterArea, waveArea;
    bool triedDefaultMidi = false;
    float meter = 0.0f;
    double progressValue = 0.0;

    // record
    juce::Label recTitle, noInput, padName, meterLabel { {}, "Input level" };
    juce::ComboBox recMode, barsBox, recTarget;
    juce::Slider threshold;
    juce::TextButton padPrev { "<" }, padNext { ">" }, recBtn { "Record" };
    juce::ToggleButton trimBtn { "Trim the silence off both ends" }, normBtn { "Bring the level up (normalize)" },
                       keepBtn { "Keep going: move to the next pad after each sound" }, monitorBtn { "Let the input through to the output" };
    Group padPicker;
    std::vector<Row> recRows;

    // auto sampler
    juce::Label autoTitle, summary, autoStatus;
    juce::ComboBox sourceBox, midiOutBox, channelBox, fromNote, toNote, stepBox, layersBox, rrBox, destBox;
    juce::TextButton choosePluginBtn { "Choose a plug-in..." }, showPluginBtn { "Show it" }, startBtn { "Start sampling" };
    juce::ToggleButton auditionBtn { "Keyboard plays it" }, loopBtn { "Loop the held part of each note" }, autoNormBtn { "Bring the level up (normalize)" };
    Group pluginRow, midiRow, noteRow, layerRow;
    juce::Slider holdSlider, tailSlider;
    juce::TextEditor nameEdit;
    juce::ProgressBar progress { progressValue };
    std::vector<Row> autoRows;

    juce::Label status;   // the latest result, across the bottom
    juce::TextButton libraryBtn { "Library..." };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SamplingPanel)
};
