#pragma once
#include "PluginProcessor.h"
#include "PluginLook.h"

class OmniEditor;

// Volume, pan, tune, envelope, filter, velocity and round-robin controls for a pad or the keys.
class VoicingControls : public juce::Component
{
public:
    VoicingControls();
    std::function<omni::Voicing()> get;
    std::function<void (const omni::Voicing&)> set;
    void refresh();
    void resized() override;

private:
    void push();
    look::Knob vol { "VOLUME" }, pan { "PAN" }, tune { "TUNE" }, att { "ATTACK" }, dec { "DECAY" }, sus { "SUSTAIN" }, rel { "RELEASE" },
               cut { "CUTOFF" }, res { "RESO" }, vs { "VEL SENS" };
    juce::ComboBox ftype, rr;
    juce::Label ftypeLabel { {}, "Filter" }, rrLabel { {}, "Round robin" };
    bool updating = false;
};

// Drum-synth layers of the selected pad: sample level, one layer shown at a time (tabs 1..4), its parameters.
class SynthLayersPanel : public juce::Component
{
public:
    explicit SynthLayersPanel (OmniEditor&);
    void refresh();
    void showLayer (int i) { layer = i; refresh(); }
    void resized() override;
    void paint (juce::Graphics&) override;

private:
    void push();
    void pushSampleLevel();
    OmniEditor& ed;
    int layer = 0, numLayers = 0;
    bool updating = false;
    juce::Label sampleLabel { {}, "Sample level" };
    juce::Slider sampleLevel;
    juce::TextButton tab[omni::kMaxSynthLayers] { juce::TextButton ("1"), juce::TextButton ("2"), juce::TextButton ("3"), juce::TextButton ("4") };
    juce::ToggleButton onBtn { "On" };
    juce::ComboBox model;
    juce::TextButton removeBtn { juce::String::fromUTF8 ("\xc3\x97") }, addBtn { "+ Synth layer" };
    juce::Viewport viewport;
    struct Holder : juce::Component { void resized() override {} } holder;
    look::Knob level { "LEVEL" }, pan { "PAN" }, tune { "TUNE" }, decay { "DECAY" }, tone { "TONE" }, pitchEnv { "PITCH ENV" },
               pitchDecay { "PITCH DECAY" }, click { "CLICK" }, noise { "NOISE" }, drive { "DRIVE" }, velSens { "VEL SENS" };
    look::Knob* knobs[11] { &level, &pan, &tune, &decay, &tone, &pitchEnv, &pitchDecay, &click, &noise, &drive, &velSens };
};

// Edits one zone (sample mapping) of a pad or of the keys.
class ZoneEditor : public juce::Component
{
public:
    explicit ZoneEditor (bool keysMode);
    std::function<const omni::Zone*()> get;         // the zone being edited, or nullptr
    std::function<void (const omni::Zone&)> set;
    void refresh();
    void resized() override;
    void paint (juce::Graphics&) override;

private:
    struct Field { juce::Label label; juce::Slider slider; };
    Field root, loKey, hiKey, loVel, hiVel, rrPos, tune, gain;
    juce::ToggleButton loop { "Loop" };
    juce::Label fileName;
    const bool keys;
    bool updating = false;
    omni::Zone shown;
    juce::Array<Field*> fields() { return { &root, &loKey, &hiKey, &loVel, &hiVel, &rrPos, &tune, &gain }; }
};

class PadButton : public juce::Component, public juce::FileDragAndDropTarget, private juce::Timer
{
public:
    PadButton (OmniEditor&, int slot);
    ~PadButton() override;
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    bool isInterestedInFileDrag (const juce::StringArray&) override { return true; }
    void filesDropped (const juce::StringArray&, int, int) override;
    void fileDragEnter (const juce::StringArray&, int, int) override { hover = true; repaint(); }
    void fileDragExit (const juce::StringArray&) override { hover = false; repaint(); }
    int padIndex() const;
    float flash = 0.0f;
    juce::uint32 lastHits = 0;

private:
    void timerCallback() override;   // the press became a hold: open the file picker (iOS)
    void release();
    OmniEditor& ed;
    const int slot;   // 0..15 within the bank
    bool hover = false;
    int playing = -1, playingCh = 0;   // note this press started, if any
};

// Key/velocity map for the Keys section.
class ZoneMap : public juce::Component, public juce::FileDragAndDropTarget
{
public:
    explicit ZoneMap (OmniEditor&);
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    bool isInterestedInFileDrag (const juce::StringArray&) override { return true; }
    void filesDropped (const juce::StringArray&, int, int) override;
    void fileDragEnter (const juce::StringArray&, int, int) override { hover = true; repaint(); }
    void fileDragExit (const juce::StringArray&) override { hover = false; repaint(); }

private:
    OmniEditor& ed;
    bool hover = false;
};

class OmniEditor : public juce::AudioProcessorEditor, private juce::Timer, private juce::ListBoxModel
{
public:
    explicit OmniEditor (OmniProcessor&);
    ~OmniEditor() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    void parentHierarchyChanged() override { resized(); }

    OmniProcessor& proc;
    look::PluginLook lnf { juce::Colour (0xff8f7bff) };
    int bank = 0, selectedPad = 0, selectedPadZone = 0, selectedKeyZone = 0;
    bool showingKeys = false;

    void selectPad (int pad);
    void selectKeyZone (int z);
    void refreshAll();
    void showMessage (const juce::String& title, const juce::String& text);
    void addSynthLayerMenu (juce::Component& target);   // popup of drum models, adds a synth layer to the selected pad
    void browsePadSamples (int pad);   // file picker, then load onto that pad
    bool isCompact() const;            // small screen (iPhone, small AUv3 window): one detail panel at a time
    omni::DropMode dropMode() const { return dropModeBox.getSelectedId() == 2 ? omni::DropMode::RoundRobin : omni::DropMode::VelocityLayers; }
    int playChannel (bool pads) const;

private:
    void timerCallback() override;
    void layoutPads (juce::Rectangle<int>);
    void layoutKeys (juce::Rectangle<int>);
    void layoutPadsCompact (juce::Rectangle<int>);
    void layoutKeysCompact (juce::Rectangle<int>);
    void applyVisibility();
    void browseKeySamples();
    void browseInstrument();
    void browseKit();
    void saveKitAs();
    void saveSfzAs();
    void saveDecentSamplerAs();
    void showOptionsMenu();
    void applyTouchVelocity();
    void showSaveMenu();
    void padFilesChosen (int pad, const juce::Array<juce::File>&);
    void instrumentChosen (const juce::File&);   // .omni, any instrument format, or a folder
   #if JUCE_IOS
    void showLoadMenu (juce::Component& target, bool withKits);
    void askNameThen (const juce::String& title, const juce::String& initial, std::function<void (juce::String)> then);
   #endif
    void setView (bool keys);
    const std::vector<omni::Zone>& currentZones() const;

    // ListBoxModel for the zone list (pads or keys, whichever is visible).
    int getNumRows() override;
    void paintListBoxItem (int row, juce::Graphics&, int w, int h, bool selected) override;
    void selectedRowsChanged (int row) override;
    void deleteKeyPressed (int row) override;

    omni::InstrumentPtr shownInst;

    // top bar
    juce::Label logo, voices;
    juce::TextButton padsTab { "Pads" }, keysTab { "Keys" }, loadKitBtn { "Load kit" }, saveKitBtn { "Save kit" }, optionsBtn { "..." };
    look::Knob masterKnob { "MASTER" };
    juce::ComboBox padsCh, keysCh;
    juce::Label padsChLabel { {}, "Pads MIDI" }, keysChLabel { {}, "Keys MIDI" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> aMaster;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> aPadsCh, aKeysCh;

    // pads view
    juce::TextButton bankBtn[4] { juce::TextButton ("A"), juce::TextButton ("B"), juce::TextButton ("C"), juce::TextButton ("D") };
    std::unique_ptr<PadButton> padButtons[omni::kPadsPerBank];
    juce::Label padTitle, padNoteLabel { {}, "MIDI note" }, chokeLabel { {}, "Choke" }, dropLabel { {}, "Drop several files as" };
    juce::ComboBox padNote, chokeBox, dropModeBox, padOutput, keysOutput;   // output combos are desktop only
    juce::Label padOutputLabel { {}, "Output" }, keysOutputLabel { {}, "Output" };
    juce::TextButton padBottomTab[2] { juce::TextButton ("Voicing"), juce::TextButton ("Layers") };
    int padBottom = 0;       // desktop: which panel sits under the zone list (0 voicing, 1 synth layers)
    SynthLayersPanel layersPanel { *this };
    juce::TextButton addSynthBtn { "+ Synth" };
    juce::ToggleButton oneShot { "One-shot" };
    juce::TextButton addToPadBtn { "Add files..." }, clearPadBtn { "Clear pad" }, remapBtn { "Re-map velocity by loudness" }, removeZoneBtn { "Remove" };

    // keys view
    ZoneMap zoneMap { *this };
    juce::MidiKeyboardComponent keyboard;
    juce::Label keysTitle;
    juce::TextButton loadInstBtn { "Load instrument..." }, addKeysBtn { "Add samples..." }, saveSfzBtn { "Save as SFZ..." },
                     saveDsBtn { "Save as DecentSampler..." }, clearKeysBtn { "Clear keys" };

    // shared
    juce::ListBox zoneList { "Zones", this };
    ZoneEditor padZoneEditor { false }, keyZoneEditor { true };
    VoicingControls padVoicing, keyVoicing;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::TextButton detailBtn[4] { juce::TextButton ("Samples"), juce::TextButton ("Edit"), juce::TextButton ("Sound"), juce::TextButton ("Layers") };
    int detail = 0;          // which panel a small screen shows
    bool wasCompact = false;
    juce::BorderSize<int> lastSafeInsets;   // iOS: notch / home bar margins the layout was made for

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OmniEditor)
};
