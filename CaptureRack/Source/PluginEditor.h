#pragma once
#include "PluginProcessor.h"
#include "PluginLook.h"

class CaptureRackEditor;
using SA = juce::AudioProcessorValueTreeState::SliderAttachment;
using BA = juce::AudioProcessorValueTreeState::ButtonAttachment;

// One NAM slot card. Accepts .nam files from Finder or from the library list.
class SlotCard : public juce::Component, public juce::FileDragAndDropTarget, public juce::DragAndDropTarget
{
public:
    SlotCard (CaptureRackProcessor&, CaptureRackEditor&, int slot);
    void paint (juce::Graphics&) override;
    void resized() override;
    void refresh();
    void load (const juce::File&);

    bool isInterestedInFileDrag (const juce::StringArray& f) override { return f.size() == 1 && f[0].endsWithIgnoreCase (".nam"); }
    void filesDropped (const juce::StringArray& f, int, int) override { load (juce::File (f[0])); hover = false; repaint(); }
    void fileDragEnter (const juce::StringArray&, int, int) override { hover = true; repaint(); }
    void fileDragExit (const juce::StringArray&) override { hover = false; repaint(); }
    bool isInterestedInDragSource (const SourceDetails& d) override { return d.description.toString().endsWithIgnoreCase (".nam"); }
    void itemDropped (const SourceDetails& d) override { load (juce::File (d.description.toString())); hover = false; repaint(); }
    void itemDragEnter (const SourceDetails&) override { hover = true; repaint(); }
    void itemDragExit (const SourceDetails&) override { hover = false; repaint(); }

    const int slot;

private:
    CaptureRackProcessor& proc;
    CaptureRackEditor& ed;
    bool hover = false;
    juce::Label title, name, info;
    juce::TextButton loadBtn { "Load" }, clearBtn { "Clear" }, onBtn { "On" }, leftBtn { "<" }, rightBtn { ">" };
    look::Knob in { "IN" }, out { "OUT" }, gate { "GATE" }, level { "LEVEL" }, pan { "PAN" };
    std::unique_ptr<SA> aIn, aOut, aGate, aLevel, aPan;
    std::unique_ptr<BA> aOn;
    std::unique_ptr<juce::FileChooser> chooser;
};

class IRCard : public juce::Component, public juce::FileDragAndDropTarget, public juce::DragAndDropTarget
{
public:
    IRCard (CaptureRackProcessor&, CaptureRackEditor&);
    void paint (juce::Graphics&) override;
    void resized() override;
    void refresh();
    void load (const juce::File&);

    bool isInterestedInFileDrag (const juce::StringArray& f) override { return f.size() == 1 && juce::File (f[0]).hasFileExtension ("wav;aif;aiff"); }
    void filesDropped (const juce::StringArray& f, int, int) override { load (juce::File (f[0])); }
    bool isInterestedInDragSource (const SourceDetails& d) override { return juce::File (d.description.toString()).hasFileExtension ("wav;aif;aiff"); }
    void itemDropped (const SourceDetails& d) override { load (juce::File (d.description.toString())); }

private:
    CaptureRackProcessor& proc;
    CaptureRackEditor& ed;
    juce::Label title, name;
    juce::TextButton loadBtn { "Load IR" }, clearBtn { "Clear" }, onBtn { "On" };
    look::Knob mix { "MIX" }, level { "LEVEL" }, lowCut { "LOW CUT" }, highCut { "HIGH CUT" };
    std::unique_ptr<SA> aMix, aLevel, aLow, aHigh;
    std::unique_ptr<BA> aOn;
    std::unique_ptr<juce::FileChooser> chooser;
};

// Local library: categories, search, favourites, drag to slots.
class LibraryPanel : public juce::Component, private juce::ListBoxModel
{
public:
    LibraryPanel (CaptureRackProcessor&, CaptureRackEditor&);
    void resized() override;
    void paint (juce::Graphics&) override;
    void reload();

private:
    int getNumRows() override { return items.size(); }
    void paintListBoxItem (int row, juce::Graphics&, int w, int h, bool selected) override;
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override;
    juce::var getDragSourceDescription (const juce::SparseSet<int>& rows) override;
    void selectedRowsChanged (int) override { updateButtons(); }
    void updateButtons();
    void loadSelectedInto (int target);   // 0..2 slots, 3 = IR

    struct CategoryModel : public juce::ListBoxModel
    {
        LibraryPanel& owner;
        explicit CategoryModel (LibraryPanel& o) : owner (o) {}
        int getNumRows() override { return owner.cats.size(); }
        void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool sel) override;
        void selectedRowsChanged (int) override { owner.reload(); }
    };

    CaptureRackProcessor& proc;
    CaptureRackEditor& ed;
    CategoryModel catModel { *this };
    juce::StringArray cats;
    juce::Array<rack::LibraryItem> items;
    juce::ListBox catList { "Categories", &catModel }, itemList { "Captures", this };
    juce::TextEditor search;
    juce::TextButton newFolderBtn { "New folder" }, importBtn { "Import..." }, showBtn { "Show in folder" }, favBtn { "Favourite" },
                     toSlot[3] { juce::TextButton ("> Slot 1"), juce::TextButton ("> Slot 2"), juce::TextButton ("> Slot 3") }, toIR { "> IR" };
    std::unique_ptr<juce::FileChooser> chooser;
};

// TONE3000 browser: sign in, list tones, download models into library folders.
class ToneBrowser : public juce::Component
{
public:
    ToneBrowser (CaptureRackProcessor&, CaptureRackEditor&);
    void resized() override;
    void paint (juce::Graphics&) override;

private:
    void refreshAccount();
    void fetch();
    void downloadSelected (bool all);
    juce::String categoryFor (const rack::T3kTone&) const;

    struct Tones : public juce::ListBoxModel
    {
        ToneBrowser& o; explicit Tones (ToneBrowser& b) : o (b) {}
        int getNumRows() override { return o.tones.size(); }
        void paintListBoxItem (int row, juce::Graphics&, int w, int h, bool sel) override;
        void selectedRowsChanged (int row) override;
    };
    struct Models : public juce::ListBoxModel
    {
        ToneBrowser& o; explicit Models (ToneBrowser& b) : o (b) {}
        int getNumRows() override { return o.models.size(); }
        void paintListBoxItem (int row, juce::Graphics&, int w, int h, bool sel) override;
    };

    CaptureRackProcessor& proc;
    CaptureRackEditor& ed;
    Tones toneModel { *this };
    Models modelModel { *this };
    juce::Array<rack::T3kTone> tones;
    juce::Array<rack::T3kModel> models;
    int page = 1, totalPages = 1;

    juce::Label brand, account, status, codeLabel, keyLabel;
    juce::TextEditor clientId, query;
    juce::TextButton signInBtn { "Sign in" }, openPageBtn { "Open activation page" }, goBtn { "Show" }, prevBtn { "<" }, nextBtn { ">" },
                     dlBtn { "Download selected" }, dlAllBtn { "Download all" }, openToneBtn { "View on TONE3000" };
    juce::ComboBox listBox, destBox;
    juce::ListBox toneList { "Tones", &toneModel }, modelList { "Models", &modelModel };
    juce::String verifyUrl;
};

class CaptureRackEditor : public juce::AudioProcessorEditor, public juce::DragAndDropContainer, private juce::Timer
{
public:
    explicit CaptureRackEditor (CaptureRackProcessor&);
    ~CaptureRackEditor() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    void parentHierarchyChanged() override { resized(); }

    void refreshAll();
    void libraryChanged() { libraryPanel.reload(); }
    look::PluginLook lnf { juce::Colour (0xffff8a3d) };

private:
    void timerCallback() override;
    void savePreset();
    void loadPreset();

    CaptureRackProcessor& proc;
    juce::Rectangle<int> lastSafeCheck;
    juce::Component content;   // holds every control; scaled to fit small screens (iPhone) and kept inside the safe area
    juce::Label logo, chainLabel;
    look::Knob input { "INPUT" }, output { "OUTPUT" }, bass { "BASS" }, mid { "MID" }, treble { "TREBLE" };
    juce::ComboBox routing;
    juce::TextButton normBtn { "Normalise" }, saveBtn { "Save" }, loadBtn { "Load" }, browseBtn { "Library" };
    bool showBrowser = false;   // compact (iPhone) layout: rack or Library/TONE3000, one at a time
    std::unique_ptr<SA> aIn, aOut, aBass, aMid, aTreble;
    std::unique_ptr<BA> aNorm;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> aRouting;
    std::unique_ptr<SlotCard> slots[CaptureRackProcessor::kSlots];
    IRCard irCard;
    LibraryPanel libraryPanel;
    ToneBrowser toneBrowser;
    juce::TabbedComponent tabs { juce::TabbedButtonBar::TabsAtTop };
    std::unique_ptr<juce::FileChooser> chooser;
    float inLevel = 0, outLevel = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CaptureRackEditor)
};
