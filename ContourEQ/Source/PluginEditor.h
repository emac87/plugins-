#pragma once
#include "PluginProcessor.h"
#include "PluginLook.h"

class ContourEditor;

// The big interactive display: analyser, curves and band nodes.
class EqDisplay : public juce::Component
{
public:
    EqDisplay (ContourProcessor&, ContourEditor&);
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override { hoverPos = {}; hoverBand = -1; repaint(); }
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    int analyserMode = 3;     // 0 off, 1 pre, 2 post, 3 both
    float rangeDb = 24.0f;
    const contour::Grid* matchRef = nullptr;
    const contour::Grid* matchSrc = nullptr;
    const contour::Grid* matchTarget = nullptr;

    static juce::Colour bandColour (int b) { return juce::Colour::fromHSV (std::fmod (0.47f + b * 0.137f, 1.0f), 0.62f, 0.95f, 1.0f); }

private:
    ContourProcessor& proc;
    ContourEditor& ed;
    juce::Rectangle<float> plot;
    int dragBand = -1, hoverBand = -1;
    bool dragFine = false;
    juce::Point<float> dragStart, hoverPos;
    float dragStartFreq = 0, dragStartGain = 0;

    float freqToX (double f) const;
    double xToFreq (float x) const;
    float dbToY (double db) const;
    double yToDb (float y) const;
    float specDbToY (double db) const;   // analyser scale
    juce::Point<float> nodePos (int band) const;
    int hitNode (juce::Point<float>) const;
    void showBandMenu (int band);
    void beginGestures (int band);
    void endGestures (int band);
};

// Controls for the selected band.
class BandPanel : public juce::Component
{
public:
    BandPanel (ContourProcessor&, ContourEditor&);
    void setBand (int band);
    int getBand() const { return band; }
    void paint (juce::Graphics&) override;
    void resized() override;
    void refreshEnablement();

private:
    ContourProcessor& proc;
    ContourEditor& ed;
    int band = -1;
    juce::Label title, hint;
    juce::TextButton onBtn { "On" }, dynBtn { "Dynamic" }, scBtn { "Ext SC" }, soloBtn { "Solo" }, delBtn { "Remove" };
    juce::ComboBox typeBox, slopeBox, placeBox;
    look::Knob freq { "FREQ" }, gain { "GAIN" }, q { "Q" }, thr { "THRESHOLD" }, range { "RANGE" }, att { "ATTACK" }, rel { "RELEASE" };
    using SA = juce::AudioProcessorValueTreeState::SliderAttachment;
    using BA = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using CA = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    std::unique_ptr<SA> aFreq, aGain, aQ, aThr, aRange, aAtt, aRel;
    std::unique_ptr<BA> aOn, aDyn, aSc;
    std::unique_ptr<CA> aType, aSlope, aPlace;
};

// Match EQ: capture a reference and a source, then fit bands.
class MatchPanel : public juce::Component
{
public:
    MatchPanel (ContourProcessor&, ContourEditor&);
    void paint (juce::Graphics&) override;
    void resized() override;
    void tick();
    contour::Grid ref {}, src {}, target {};
    bool haveRef = false, haveSrc = false, haveTarget = false;

private:
    void updateTarget();
    ContourProcessor& proc;
    ContourEditor& ed;
    juce::Label title, status, bandsLabel, amountLabel, smoothLabel;
    juce::ComboBox refSource, smoothBox;
    juce::TextButton loadBtn { "Load reference file..." }, capRefBtn { "Capture reference" }, capSrcBtn { "Capture this track" },
                     matchBtn { "Match" }, clearBtn { "Clear" };
    juce::Slider bands, amount;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::String fileName;
};

class ContourEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit ContourEditor (ContourProcessor&);
    ~ContourEditor() override;
    void paint (juce::Graphics&) override;
    void resized() override;

    void selectBand (int b);
    int getSelectedBand() const { return selected; }
    bool matchVisible() const { return matchPanel.isVisible(); }

private:
    void timerCallback() override;
    void savePreset();
    void loadPreset();

    ContourProcessor& proc;
    look::PluginLook lnf { juce::Colour (0xff35c9b4) };
    int selected = -1;

    EqDisplay display;
    BandPanel bandPanel;
    MatchPanel matchPanel;

    juce::Label logo;
    juce::ComboBox analyserBox, rangeBox;
    look::Knob scaleKnob { "GAIN SCALE" }, outKnob { "OUTPUT" }, panKnob { "PAN" };
    juce::TextButton undoBtn { "Undo" }, redoBtn { "Redo" }, abBtn { "A" }, abCopyBtn { "Copy A>B" },
                     saveBtn { "Save" }, loadBtn { "Load" }, matchBtn { "Match EQ" };
    juce::Label phaseLabel;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> aScale, aOut, aPan;
    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ContourEditor)
};
