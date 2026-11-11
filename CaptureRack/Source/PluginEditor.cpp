#include "PluginEditor.h"

using namespace juce;

static void styleSmallKnob (look::Knob& k)
{
    k.slider.setTextBoxStyle (Slider::TextBoxBelow, false, 64, 14);
    k.label.setFont (FontOptions (10.5f));
}

static void message (Component* c, const String& title, const String& text)
{
    AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon, title, text, "OK", c);
}

// =============================================================================================
SlotCard::SlotCard (CaptureRackProcessor& p, CaptureRackEditor& e, int s) : slot (s), proc (p), ed (e)
{
    title.setText ("SLOT " + String (s + 1), dontSendNotification);
    title.setFont (FontOptions (13.0f, Font::bold));
    title.setColour (Label::textColourId, ed.lnf.accent);
    name.setFont (FontOptions (15.0f, Font::bold));
    name.setMinimumHorizontalScale (0.6f);
    info.setFont (FontOptions (11.5f));
    info.setColour (Label::textColourId, look::textDim);
    info.setMinimumHorizontalScale (0.6f);
    onBtn.setClickingTogglesState (true);
    loadBtn.onClick = [this]
    {
        chooser = std::make_unique<FileChooser> ("Load NAM model", proc.library.getRoot(), "*.nam");
        chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
                              [this] (const FileChooser& fc) { if (fc.getResult().existsAsFile()) load (fc.getResult()); });
    };
    clearBtn.onClick = [this] { proc.clearSlot (slot); refresh(); };
    leftBtn.onClick = [this] { proc.moveSlot (slot, -1); ed.refreshAll(); };
    rightBtn.onClick = [this] { proc.moveSlot (slot, 1); ed.refreshAll(); };
    leftBtn.setTooltip ("Move earlier in the chain");
    rightBtn.setTooltip ("Move later in the chain");

    auto& st = proc.apvts;
    aOn = std::make_unique<BA> (st, CaptureRackProcessor::sid (s, "on"), onBtn);
    aIn = std::make_unique<SA> (st, CaptureRackProcessor::sid (s, "in"), in.slider);
    aOut = std::make_unique<SA> (st, CaptureRackProcessor::sid (s, "out"), out.slider);
    aGate = std::make_unique<SA> (st, CaptureRackProcessor::sid (s, "gate"), gate.slider);
    aLevel = std::make_unique<SA> (st, CaptureRackProcessor::sid (s, "level"), level.slider);
    aPan = std::make_unique<SA> (st, CaptureRackProcessor::sid (s, "pan"), pan.slider);
    for (auto* k : { &in, &out, &gate, &level, &pan }) styleSmallKnob (*k);
    for (auto* c : std::initializer_list<Component*> { &title, &name, &info, &loadBtn, &clearBtn, &onBtn, &leftBtn, &rightBtn, &in, &out, &gate, &level, &pan })
        addAndMakeVisible (c);
    refresh();
}

void SlotCard::load (const File& f)
{
    String err;
    if (! proc.loadSlot (slot, f, err)) message (this, "Couldn't load model", f.getFileName() + "\n" + err);
    refresh();
}

void SlotCard::refresh()
{
    const auto n = proc.slotName (slot);
    name.setText (n.isNotEmpty() ? n : "Empty - drop a .nam here", dontSendNotification);
    name.setColour (Label::textColourId, n.isNotEmpty() ? look::text : look::textDim);
    info.setText (proc.slotInfo (slot), dontSendNotification);
    const bool parallel = proc.apvts.getRawParameterValue ("routing")->load() > 0.5f;
    level.setVisible (parallel);
    pan.setVisible (parallel);
    leftBtn.setVisible (! parallel);
    rightBtn.setVisible (! parallel);
    resized();
    repaint();
}

void SlotCard::paint (Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    look::drawPanel (g, r, 8.0f);
    if (hover)
    {
        g.setColour (ed.lnf.accent);
        g.drawRoundedRectangle (r.reduced (1.5f), 8.0f, 2.0f);
    }
    // Gate light.
    const float gg = proc.gateGain[slot].load();
    const bool gateOn = proc.apvts.getRawParameterValue (CaptureRackProcessor::sid (slot, "gate"))->load() > -99.0f;
    if (gateOn)
    {
        g.setColour (gg > 0.5f ? Colour (0xff5ee07a) : Colour (0xff704040));
        g.fillEllipse (gate.getBounds().toFloat().removeFromTop (14).withSizeKeepingCentre (6, 6).translated (24, 0));
    }
}

void SlotCard::resized()
{
    auto r = getLocalBounds().reduced (10);
    auto top = r.removeFromTop (22);
    rightBtn.setBounds (top.removeFromRight (24)); top.removeFromRight (2);
    leftBtn.setBounds (top.removeFromRight (24)); top.removeFromRight (6);
    onBtn.setBounds (top.removeFromRight (40));
    title.setBounds (top);
    r.removeFromTop (4);
    name.setBounds (r.removeFromTop (22));
    info.setBounds (r.removeFromTop (16));
    r.removeFromTop (4);
    auto b = r.removeFromTop (24);
    loadBtn.setBounds (b.removeFromLeft (b.getWidth() / 2 - 3));
    b.removeFromLeft (6);
    clearBtn.setBounds (b);
    r.removeFromTop (8);
    const bool parallel = level.isVisible();
    auto row1 = r.removeFromTop (juce::jmin (96, parallel ? r.getHeight() / 2 : r.getHeight()));
    const int w = row1.getWidth() / 3;
    in.setBounds (row1.removeFromLeft (w));
    gate.setBounds (row1.removeFromLeft (w));
    out.setBounds (row1);
    if (parallel)
    {
        r = r.removeFromTop (juce::jmin (96, r.getHeight()));
        const int w2 = r.getWidth() / 2;
        level.setBounds (r.removeFromLeft (w2));
        pan.setBounds (r);
    }
}

// =============================================================================================
IRCard::IRCard (CaptureRackProcessor& p, CaptureRackEditor& e) : proc (p), ed (e)
{
    title.setText ("CAB IR", dontSendNotification);
    title.setFont (FontOptions (13.0f, Font::bold));
    title.setColour (Label::textColourId, ed.lnf.accent);
    name.setFont (FontOptions (14.0f, Font::bold));
    name.setMinimumHorizontalScale (0.6f);
    onBtn.setClickingTogglesState (true);
    loadBtn.onClick = [this]
    {
        auto dir = proc.library.getRoot().getChildFile ("IRs");
        chooser = std::make_unique<FileChooser> ("Load cab IR", dir, "*.wav;*.aif;*.aiff");
        chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
                              [this] (const FileChooser& fc) { if (fc.getResult().existsAsFile()) load (fc.getResult()); });
    };
    clearBtn.onClick = [this] { proc.clearIR(); refresh(); };
    auto& st = proc.apvts;
    aOn = std::make_unique<BA> (st, "ir_on", onBtn);
    aMix = std::make_unique<SA> (st, "ir_mix", mix.slider);
    aLevel = std::make_unique<SA> (st, "ir_level", level.slider);
    aLow = std::make_unique<SA> (st, "low_cut", lowCut.slider);
    aHigh = std::make_unique<SA> (st, "high_cut", highCut.slider);
    for (auto* k : { &mix, &level, &lowCut, &highCut }) styleSmallKnob (*k);
    for (auto* c : std::initializer_list<Component*> { &title, &name, &loadBtn, &clearBtn, &onBtn, &mix, &level, &lowCut, &highCut })
        addAndMakeVisible (c);
    refresh();
}

void IRCard::load (const File& f)
{
    String err;
    if (! proc.loadIR (f, err)) message (this, "Couldn't load IR", f.getFileName() + "\n" + err);
    refresh();
}

void IRCard::refresh()
{
    const auto f = proc.irFile();
    name.setText (f.existsAsFile() ? f.getFileNameWithoutExtension() : "No IR - drop a .wav here", dontSendNotification);
    name.setColour (Label::textColourId, f.existsAsFile() ? look::text : look::textDim);
}

void IRCard::paint (Graphics& g) { look::drawPanel (g, getLocalBounds().toFloat(), 8.0f); }

void IRCard::resized()
{
    auto r = getLocalBounds().reduced (10);
    auto top = r.removeFromTop (22);
    onBtn.setBounds (top.removeFromRight (40));
    title.setBounds (top);
    r.removeFromTop (4);
    name.setBounds (r.removeFromTop (22));
    r.removeFromTop (20);
    auto b = r.removeFromTop (24);
    loadBtn.setBounds (b.removeFromLeft (b.getWidth() / 2 - 3));
    b.removeFromLeft (6);
    clearBtn.setBounds (b);
    r.removeFromTop (8);
    r = r.removeFromTop (juce::jmin (r.getHeight(), 200));
    auto row1 = r.removeFromTop (r.getHeight() / 2);
    mix.setBounds (row1.removeFromLeft (row1.getWidth() / 2));
    level.setBounds (row1);
    lowCut.setBounds (r.removeFromLeft (r.getWidth() / 2));
    highCut.setBounds (r);
}

// =============================================================================================
LibraryPanel::LibraryPanel (CaptureRackProcessor& p, CaptureRackEditor& e) : proc (p), ed (e)
{
    search.setTextToShowWhenEmpty ("Search captures or authors", look::textDim);
    search.onTextChange = [this] { reload(); };
    catList.setRowHeight (24);
    itemList.setRowHeight (24);
    itemList.setMultipleSelectionEnabled (true);
    newFolderBtn.onClick = [this]
    {
        auto* w = new AlertWindow ("New folder", "Name for the new category folder:", MessageBoxIconType::NoIcon, this);
        w->addTextEditor ("name", "", "Name");
        w->addButton ("Create", 1, KeyPress (KeyPress::returnKey));
        w->addButton ("Cancel", 0, KeyPress (KeyPress::escapeKey));
        w->enterModalState (true, ModalCallbackFunction::create ([this, w] (int r)
        {
            if (r == 1 && w->getTextEditorContents ("name").trim().isNotEmpty())
            {
                proc.library.addCategory (w->getTextEditorContents ("name"));
                reload();
            }
        }), true);
    };
    importBtn.onClick = [this]
    {
        chooser = std::make_unique<FileChooser> ("Import captures and IRs", File::getSpecialLocation (File::userHomeDirectory).getChildFile ("Downloads"),
                                                 "*.nam;*.wav;*.aif;*.aiff");
        chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles | FileBrowserComponent::canSelectMultipleItems,
            [this] (const FileChooser& fc)
            {
                auto files = fc.getResults();
                if (files.isEmpty()) return;
                const int row = catList.getSelectedRow();
                String cat = row >= 2 ? cats[row] : String();
                Array<File> nams, irs;
                for (auto& f : files) (f.hasFileExtension ("nam") ? nams : irs).add (f);
                proc.library.import (nams, cat.isNotEmpty() && cat != "IRs" ? cat : "Amps");
                proc.library.import (irs, "IRs");
                reload();
            });
    };
    showBtn.onClick = [this]
    {
        const int r = itemList.getSelectedRow();
        if (r >= 0 && r < items.size()) items[r].file.revealToUser();
        else proc.library.getRoot().revealToUser();
    };
    favBtn.onClick = [this]
    {
        for (int i = 0; i < itemList.getNumSelectedRows(); ++i)
        {
            const int r = itemList.getSelectedRow (i);
            if (r >= 0 && r < items.size()) proc.library.setFavourite (items[r].file, ! items[r].favourite);
        }
        reload();
    };
    for (int s = 0; s < 3; ++s) toSlot[s].onClick = [this, s] { loadSelectedInto (s); };
    toIR.onClick = [this] { loadSelectedInto (3); };

    for (auto* c : std::initializer_list<Component*> { &catList, &itemList, &search, &newFolderBtn, &importBtn, &showBtn, &favBtn,
                                                       &toSlot[0], &toSlot[1], &toSlot[2], &toIR })
        addAndMakeVisible (c);
    reload();
    catList.selectRow (0);
}

void LibraryPanel::reload()
{
    const int catRow = catList.getSelectedRow();
    const auto prevCat = catRow >= 0 ? cats[catRow] : String ("All");
    cats.clear();
    cats.add ("All"); cats.add ("Favourites");
    cats.addArray (proc.library.categories());
    catList.updateContent();
    const int idx = cats.indexOf (prevCat);
    if (idx >= 0 && idx != catRow) catList.selectRow (idx, true, true);
    const auto cat = idx >= 0 ? prevCat : String ("All");
    items = proc.library.items (cat == "All" ? String() : cat, search.getText().trim());
    itemList.updateContent();
    itemList.repaint();
    updateButtons();
}

void LibraryPanel::CategoryModel::paintListBoxItem (int row, Graphics& g, int w, int h, bool sel)
{
    if (sel) g.fillAll (owner.ed.lnf.accent.withAlpha (0.3f));
    g.setColour (row < 2 ? look::textDim : look::text);
    g.setFont (FontOptions (13.5f));
    g.drawText ((row == 1 ? "* " : "") + owner.cats[row], 8, 0, w - 8, h, Justification::centredLeft);
}

void LibraryPanel::paintListBoxItem (int row, Graphics& g, int w, int h, bool sel)
{
    if (row < 0 || row >= items.size()) return;
    const auto& it = items.getReference (row);
    if (sel) g.fillAll (ed.lnf.accent.withAlpha (0.3f));
    else if (row % 2) g.fillAll (Colours::white.withAlpha (0.02f));
    g.setFont (FontOptions (13.5f));
    g.setColour (it.favourite ? Colour (0xffffd34d) : look::textDim.withAlpha (0.4f));
    g.drawText ("*", 6, 0, 14, h, Justification::centred);
    g.setColour (look::text);
    g.drawText (it.file.getFileNameWithoutExtension(), 24, 0, w / 2, h, Justification::centredLeft, true);
    g.setColour (look::textDim);
    String right = it.category;
    if (it.author.isNotEmpty()) right = "by " + it.author + "   " + right;
    if (it.isIR()) right << "   IR";
    g.drawText (right, w / 2 + 24, 0, w / 2 - 30, h, Justification::centredRight, true);
}

void LibraryPanel::listBoxItemDoubleClicked (int row, const MouseEvent&)
{
    if (row < 0 || row >= items.size()) return;
    if (items[row].isIR()) { loadSelectedInto (3); return; }
    for (int s = 0; s < 3; ++s)
        if (proc.slotName (s).isEmpty()) { loadSelectedInto (s); return; }
    loadSelectedInto (0);
}

var LibraryPanel::getDragSourceDescription (const SparseSet<int>& rows)
{
    if (rows.isEmpty() || rows[0] >= items.size()) return {};
    return items[rows[0]].file.getFullPathName();
}

void LibraryPanel::updateButtons()
{
    const int r = itemList.getSelectedRow();
    const bool has = r >= 0 && r < items.size();
    const bool ir = has && items[r].isIR();
    for (auto& b : toSlot) b.setEnabled (has && ! ir);
    toIR.setEnabled (ir);
    favBtn.setEnabled (has);
}

void LibraryPanel::loadSelectedInto (int target)
{
    const int r = itemList.getSelectedRow();
    if (r < 0 || r >= items.size()) return;
    const auto f = items[r].file;
    String err;
    const bool ok = target == 3 ? proc.loadIR (f, err) : proc.loadSlot (target, f, err);
    if (! ok) message (this, "Couldn't load", f.getFileName() + "\n" + err);
    ed.refreshAll();
}

void LibraryPanel::paint (Graphics& g) { g.fillAll (look::panel); }

void LibraryPanel::resized()
{
    auto r = getLocalBounds().reduced (8);
    auto left = r.removeFromLeft (170);
    newFolderBtn.setBounds (left.removeFromBottom (26));
    left.removeFromBottom (6);
    catList.setBounds (left);
    r.removeFromLeft (8);
    auto top = r.removeFromTop (26);
    toIR.setBounds (top.removeFromRight (56)); top.removeFromRight (4);
    for (int s = 2; s >= 0; --s) { toSlot[s].setBounds (top.removeFromRight (70)); top.removeFromRight (4); }
    top.removeFromRight (8);
    favBtn.setBounds (top.removeFromRight (84)); top.removeFromRight (4);
    showBtn.setBounds (top.removeFromRight (110)); top.removeFromRight (4);
    importBtn.setBounds (top.removeFromRight (80)); top.removeFromRight (8);
    search.setBounds (top);
    r.removeFromTop (6);
    itemList.setBounds (r);
}

// =============================================================================================
ToneBrowser::ToneBrowser (CaptureRackProcessor& p, CaptureRackEditor& e) : proc (p), ed (e)
{
    brand.setText ("TONE3000", dontSendNotification);
    brand.setFont (FontOptions (18.0f, Font::bold));
    keyLabel.setText ("App key", dontSendNotification);
    keyLabel.setColour (Label::textColourId, look::textDim);
    clientId.setTextToShowWhenEmpty ("Publishable key (client_id) from tone3000.com/api", look::textDim);
    clientId.setText (proc.tone3000.clientId(), false);
    clientId.onTextChange = [this] { proc.tone3000.setClientId (clientId.getText()); };
    codeLabel.setFont (FontOptions (15.0f, Font::bold));
    codeLabel.setColour (Label::textColourId, ed.lnf.accent);
    status.setColour (Label::textColourId, look::textDim);
    account.setColour (Label::textColourId, look::text);
    openPageBtn.setVisible (false);
    openPageBtn.onClick = [this] { if (verifyUrl.isNotEmpty()) URL (verifyUrl).launchInDefaultBrowser(); };

    signInBtn.onClick = [this]
    {
        if (proc.tone3000.signedIn()) { proc.tone3000.signOut(); refreshAccount(); return; }
        status.setText ("Asking TONE3000 for a sign-in code...", dontSendNotification);
        SafePointer<ToneBrowser> safe (this);
        proc.tone3000.signIn (
            [safe] (String code, String url)
            {
                if (safe == nullptr) return;
                safe->verifyUrl = url;
                safe->codeLabel.setText ("Code: " + code, dontSendNotification);
                safe->openPageBtn.setVisible (true);
                safe->status.setText ("Open the activation page, sign in, and enter the code.", dontSendNotification);
                URL (url).launchInDefaultBrowser();
            },
            [safe] (bool ok, String msg)
            {
                if (safe == nullptr) return;
                safe->codeLabel.setText ({}, dontSendNotification);
                safe->openPageBtn.setVisible (false);
                safe->status.setText (msg, dontSendNotification);
                safe->refreshAccount();
                if (ok) safe->fetch();
            });
    };

    listBox.addItem ("My favourites", 1); listBox.addItem ("My tones", 2); listBox.addItem ("Downloaded", 3);
    listBox.addItem ("Trending", 4); listBox.addItem ("Latest", 5); listBox.addItem ("Search", 6);
    listBox.setSelectedId (1, dontSendNotification);
    listBox.onChange = [this] { query.setVisible (listBox.getSelectedId() == 6); page = 1; resized(); fetch(); };
    query.setTextToShowWhenEmpty ("Search TONE3000", look::textDim);
    query.onReturnKey = [this] { page = 1; fetch(); };
    query.setVisible (false);
    goBtn.onClick = [this] { page = 1; fetch(); };
    prevBtn.onClick = [this] { if (page > 1) { --page; fetch(); } };
    nextBtn.onClick = [this] { if (page < totalPages) { ++page; fetch(); } };

    destBox.addItem ("Folder: from gear type", 1);
    int id = 2;
    for (auto* c : rack::Library::defaultCategories) destBox.addItem (String ("Folder: ") + c, id++);
    destBox.setSelectedId (1, dontSendNotification);
    dlBtn.onClick = [this] { downloadSelected (false); };
    dlAllBtn.onClick = [this] { downloadSelected (true); };
    openToneBtn.onClick = [this]
    {
        const int r = toneList.getSelectedRow();
        if (r >= 0 && r < tones.size() && tones[r].url.isNotEmpty()) URL (tones[r].url).launchInDefaultBrowser();
    };

    toneList.setRowHeight (34);
    modelList.setRowHeight (24);
    modelList.setMultipleSelectionEnabled (true);
    for (auto* c : std::initializer_list<Component*> { &brand, &account, &status, &codeLabel, &keyLabel, &clientId, &query, &signInBtn, &openPageBtn,
                                                       &goBtn, &prevBtn, &nextBtn, &dlBtn, &dlAllBtn, &openToneBtn, &listBox, &destBox, &toneList, &modelList })
        addAndMakeVisible (c);
    query.setVisible (false);
    openPageBtn.setVisible (false);
    refreshAccount();
}

void ToneBrowser::refreshAccount()
{
    const bool in = proc.tone3000.signedIn();
    signInBtn.setButtonText (in ? "Sign out" : "Sign in");
    account.setText (in ? "Signed in as " + proc.tone3000.userName() : "Not signed in", dontSendNotification);
}

void ToneBrowser::fetch()
{
    static const char* lists[] = { "favorited", "created", "downloaded", "trending", "latest", "search" };
    const int idx = jlimit (0, 5, listBox.getSelectedId() - 1);
    if (! proc.tone3000.signedIn() && idx < 3) { status.setText ("Sign in to see your own lists.", dontSendNotification); return; }
    status.setText ("Loading...", dontSendNotification);
    SafePointer<ToneBrowser> safe (this);
    proc.tone3000.tones (lists[idx], query.getText(), page, [safe] (Array<rack::T3kTone> t, int pages, String err)
    {
        if (safe == nullptr) return;
        safe->tones = t;
        safe->totalPages = pages;
        safe->models.clear();
        safe->toneList.updateContent(); safe->toneList.deselectAllRows();
        safe->modelList.updateContent();
        safe->status.setText (err.isNotEmpty() ? err : String (t.size()) + " tones, page " + String (safe->page) + " of " + String (pages), dontSendNotification);
    });
}

String ToneBrowser::categoryFor (const rack::T3kTone& t) const
{
    if (destBox.getSelectedId() > 1) return rack::Library::defaultCategories[destBox.getSelectedId() - 2];
    if (t.format == "ir" || t.gear == "ir" || t.gear == "cab") return "IRs";
    if (t.gear == "amp") return "Amps";
    if (t.gear == "amp-cab" || t.gear == "full-rig") return "Amp + Cab";
    if (t.gear == "pedal") return "Pedals";
    if (t.gear == "outboard") return "Outboard";
    if (t.gear == "space") return "Space";
    return "Experimental";
}

void ToneBrowser::downloadSelected (bool all)
{
    const int tr = toneList.getSelectedRow();
    if (tr < 0 || tr >= tones.size()) { status.setText ("Pick a tone first.", dontSendNotification); return; }
    const auto tone = tones[tr];
    Array<rack::T3kModel> pick;
    if (all) pick = models;
    else for (int i = 0; i < modelList.getNumSelectedRows(); ++i) { const int r = modelList.getSelectedRow (i); if (r >= 0 && r < models.size()) pick.add (models[r]); }
    if (pick.isEmpty()) { status.setText ("Pick one or more models.", dontSendNotification); return; }

    const auto cat = categoryFor (tone);
    auto folder = proc.library.addCategory (cat).getChildFile (File::createLegalFileName (tone.title + (tone.author.isNotEmpty() ? " - " + tone.author : String())));
    auto remaining = std::make_shared<int> (pick.size());
    auto okCount = std::make_shared<int> (0);
    status.setText ("Downloading " + String (pick.size()) + " file(s) to " + cat + "...", dontSendNotification);
    SafePointer<ToneBrowser> safe (this);
    for (auto& m : pick)
    {
        const bool ir = m.arch == "IR" || tone.format == "ir";
        auto dest = folder.getChildFile (File::createLegalFileName (m.name.isNotEmpty() ? m.name : m.id) + (ir ? ".wav" : ".nam"));
        proc.tone3000.download (m, dest, [safe, dest, tone, remaining, okCount, cat] (bool ok, String msg)
        {
            if (safe == nullptr) return;
            if (ok)
            {
                ++*okCount;
                safe->proc.library.setInfo (dest, tone.author, "TONE3000", tone.license, tone.url);
            }
            if (--*remaining == 0)
            {
                safe->status.setText (String (*okCount) + " downloaded into " + cat + (ok ? String() : "  (last error: " + msg + ")"), dontSendNotification);
                safe->ed.libraryChanged();
            }
        });
    }
}

void ToneBrowser::Tones::paintListBoxItem (int row, Graphics& g, int w, int h, bool sel)
{
    if (row < 0 || row >= o.tones.size()) return;
    const auto& t = o.tones.getReference (row);
    if (sel) g.fillAll (o.ed.lnf.accent.withAlpha (0.3f));
    g.setColour (look::text);
    g.setFont (FontOptions (13.5f, Font::bold));
    g.drawText (t.title, 8, 2, w - 16, 16, Justification::centredLeft, true);
    g.setColour (look::textDim);
    g.setFont (FontOptions (11.5f));
    String sub = "by " + t.author + "   " + t.gear;
    if (t.a1) sub << "   A1 x" << t.a1;
    if (t.a2) sub << "   A2 x" << t.a2;
    if (t.irs) sub << "   IR x" << t.irs;
    g.drawText (sub, 8, 18, w - 16, 14, Justification::centredLeft, true);
}

void ToneBrowser::Tones::selectedRowsChanged (int row)
{
    if (row < 0 || row >= o.tones.size()) return;
    o.models.clear();
    o.modelList.updateContent();
    o.status.setText ("Loading models...", dontSendNotification);
    SafePointer<ToneBrowser> safe (&o);
    o.proc.tone3000.models (o.tones[row].id, [safe] (Array<rack::T3kModel> m, String err)
    {
        if (safe == nullptr) return;
        safe->models = m;
        safe->modelList.updateContent();
        safe->modelList.selectRow (0);
        safe->status.setText (m.isEmpty() ? (err.isNotEmpty() ? err : String ("No downloadable models")) : String (m.size()) + " models", dontSendNotification);
    });
}

void ToneBrowser::Models::paintListBoxItem (int row, Graphics& g, int w, int h, bool sel)
{
    if (row < 0 || row >= o.models.size()) return;
    const auto& m = o.models.getReference (row);
    if (sel) g.fillAll (o.ed.lnf.accent.withAlpha (0.3f));
    g.setColour (look::text);
    g.setFont (FontOptions (13.0f));
    g.drawText (m.name, 8, 0, w - 110, h, Justification::centredLeft, true);
    g.setColour (m.arch == "A2" ? o.ed.lnf.accent : look::textDim);
    g.drawText (m.arch + "  " + m.size, w - 104, 0, 96, h, Justification::centredRight);
}

void ToneBrowser::paint (Graphics& g)
{
    g.fillAll (look::panel);
    g.setColour (look::textDim);
    g.setFont (FontOptions (11.0f));
    g.drawText ("Captures and IRs from the TONE3000 community. Creator names are kept with each download.",
                getLocalBounds().reduced (8).removeFromBottom (14), Justification::centredLeft);
}

void ToneBrowser::resized()
{
    auto r = getLocalBounds().reduced (8);
    r.removeFromBottom (16);
    auto row1 = r.removeFromTop (28);
    brand.setBounds (row1.removeFromLeft (110));
    keyLabel.setBounds (row1.removeFromLeft (56));
    clientId.setBounds (row1.removeFromLeft (300).reduced (0, 2)); row1.removeFromLeft (8);
    signInBtn.setBounds (row1.removeFromLeft (80)); row1.removeFromLeft (8);
    openPageBtn.setBounds (row1.removeFromLeft (150)); row1.removeFromLeft (8);
    codeLabel.setBounds (row1.removeFromLeft (150));
    account.setBounds (row1);
    r.removeFromTop (6);
    auto row2 = r.removeFromTop (26);
    listBox.setBounds (row2.removeFromLeft (150)); row2.removeFromLeft (6);
    if (query.isVisible()) { query.setBounds (row2.removeFromLeft (220)); row2.removeFromLeft (6); }
    goBtn.setBounds (row2.removeFromLeft (60)); row2.removeFromLeft (6);
    prevBtn.setBounds (row2.removeFromLeft (28)); row2.removeFromLeft (2);
    nextBtn.setBounds (row2.removeFromLeft (28)); row2.removeFromLeft (10);
    status.setBounds (row2);
    r.removeFromTop (6);
    auto right = r.removeFromRight (r.getWidth() * 2 / 5);
    toneList.setBounds (r.withTrimmedRight (6));
    auto btns = right.removeFromBottom (26);
    right.removeFromBottom (6);
    auto dest = right.removeFromTop (26);
    destBox.setBounds (dest.removeFromLeft (dest.getWidth() / 2 + 20)); dest.removeFromLeft (6);
    openToneBtn.setBounds (dest);
    right.removeFromTop (6);
    modelList.setBounds (right);
    dlBtn.setBounds (btns.removeFromLeft (btns.getWidth() / 2 - 3));
    btns.removeFromLeft (6);
    dlAllBtn.setBounds (btns);
}

// =============================================================================================
CaptureRackEditor::CaptureRackEditor (CaptureRackProcessor& p)
    : AudioProcessorEditor (p), proc (p), irCard (p, *this), libraryPanel (p, *this), toneBrowser (p, *this)
{
    setLookAndFeel (&lnf);
    logo.setText ("CAPTURE RACK", dontSendNotification);
    logo.setFont (FontOptions (19.0f, Font::bold));
    logo.setColour (Label::textColourId, lnf.accent);
    chainLabel.setColour (Label::textColourId, look::textDim);
    chainLabel.setFont (FontOptions (12.0f));

    routing.addItem ("Series", 1);
    routing.addItem ("Parallel", 2);
    aRouting = std::make_unique<AudioProcessorValueTreeState::ComboBoxAttachment> (proc.apvts, "routing", routing);
    routing.onChange = [this] { refreshAll(); };
    normBtn.setClickingTogglesState (true);
    normBtn.setTooltip ("Level-match models using their recorded loudness");
    aNorm = std::make_unique<BA> (proc.apvts, "normalise", normBtn);
    aIn = std::make_unique<SA> (proc.apvts, "input", input.slider);
    aOut = std::make_unique<SA> (proc.apvts, "output", output.slider);
    aBass = std::make_unique<SA> (proc.apvts, "bass", bass.slider);
    aMid = std::make_unique<SA> (proc.apvts, "mid", mid.slider);
    aTreble = std::make_unique<SA> (proc.apvts, "treble", treble.slider);
    for (auto* k : { &input, &output, &bass, &mid, &treble }) styleSmallKnob (*k);
    for (auto* k : { &input, &output })
    {
        k->slider.setTextBoxStyle (Slider::NoTextBox, false, 0, 0);
        k->slider.setPopupDisplayEnabled (true, true, this);
    }
    saveBtn.onClick = [this] { savePreset(); };
    loadBtn.onClick = [this] { loadPreset(); };

    for (int s = 0; s < CaptureRackProcessor::kSlots; ++s)
    {
        slots[s] = std::make_unique<SlotCard> (p, *this, s);
        addAndMakeVisible (*slots[s]);
    }
    tabs.addTab ("Library", look::panel, &libraryPanel, false);
    tabs.addTab ("TONE3000", look::panel, &toneBrowser, false);
    tabs.setTabBarDepth (30);
    tabs.setOutline (0);

    for (auto* c : std::initializer_list<Component*> { &logo, &chainLabel, &input, &output, &bass, &mid, &treble, &routing, &normBtn, &saveBtn, &loadBtn, &irCard, &tabs })
        addAndMakeVisible (c);

    setResizable (true, true);
    setResizeLimits (980, 680, 2000, 1400);
    setSize (1180, 800);
    refreshAll();
    startTimerHz (20);
}

CaptureRackEditor::~CaptureRackEditor() { setLookAndFeel (nullptr); }

void CaptureRackEditor::refreshAll()
{
    for (auto& s : slots) s->refresh();
    irCard.refresh();
    const auto o = proc.getOrder();
    const bool parallel = proc.apvts.getRawParameterValue ("routing")->load() > 0.5f;
    chainLabel.setText (parallel ? "Guitar -> [ 1 | 2 | 3 ] -> IR -> Tone -> Out"
                                 : "Guitar -> " + String (o[0] + 1) + " -> " + String (o[1] + 1) + " -> " + String (o[2] + 1) + " -> IR -> Tone -> Out",
                        dontSendNotification);
    resized();
}

void CaptureRackEditor::timerCallback()
{
    inLevel = jmax (proc.inMeter.load(), inLevel * 0.85f);
    outLevel = jmax (proc.outMeter.load(), outLevel * 0.85f);
    proc.inMeter.store (0); proc.outMeter.store (0);
    repaint (0, 0, getWidth(), 60);
    for (auto& s : slots) s->repaint();
}

void CaptureRackEditor::paint (Graphics& g)
{
    g.fillAll (look::bg);
    // Meters next to the input and output knobs.
    auto meter = [&g] (Rectangle<int> r, float lvl)
    {
        g.setColour (look::panelHi);
        g.fillRoundedRectangle (r.toFloat(), 2.0f);
        const float db = Decibels::gainToDecibels (lvl, -60.0f);
        const float frac = jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);
        auto f = r.toFloat().withTrimmedTop (r.getHeight() * (1.0f - frac));
        g.setColour (db > -1.0f ? Colour (0xffff5050) : db > -12.0f ? Colour (0xffffc24d) : Colour (0xff5ee07a));
        g.fillRoundedRectangle (f, 2.0f);
    };
    meter (input.getBounds().withX (input.getRight() + 2).withWidth (6).reduced (0, 6), inLevel);
    meter (output.getBounds().withX (output.getRight() + 2).withWidth (6).reduced (0, 6), outLevel);
}

void CaptureRackEditor::resized()
{
    auto r = getLocalBounds().reduced (10);
    auto top = r.removeFromTop (56);
    logo.setBounds (top.removeFromLeft (170));
    input.setBounds (top.removeFromLeft (64)); top.removeFromLeft (14);
    routing.setBounds (top.removeFromLeft (110).withSizeKeepingCentre (110, 28)); top.removeFromLeft (8);
    normBtn.setBounds (top.removeFromLeft (90).withSizeKeepingCentre (90, 28)); top.removeFromLeft (12);
    loadBtn.setBounds (top.removeFromRight (60).withSizeKeepingCentre (60, 28)); top.removeFromRight (6);
    saveBtn.setBounds (top.removeFromRight (60).withSizeKeepingCentre (60, 28)); top.removeFromRight (16);
    output.setBounds (top.removeFromRight (64)); top.removeFromRight (14);
    chainLabel.setBounds (top);

    r.removeFromTop (6);
    auto rack = r.removeFromTop (jmax (290, r.getHeight() / 2 - 10));
    auto tone = rack.removeFromRight (130);
    rack.removeFromRight (8);
    auto ir = rack.removeFromRight (220);
    rack.removeFromRight (8);
    const auto o = proc.getOrder();
    const bool parallel = proc.apvts.getRawParameterValue ("routing")->load() > 0.5f;
    const int w = (rack.getWidth() - 16) / 3;
    for (int k = 0; k < CaptureRackProcessor::kSlots; ++k)
    {
        const int s = parallel ? k : o[(size_t) k];   // cards follow the chain order in series
        slots[s]->setBounds (rack.removeFromLeft (w));
        rack.removeFromLeft (8);
    }
    irCard.setBounds (ir);
    auto t = tone.reduced (6).removeFromTop (juce::jmin (tone.getHeight() - 12, 300));
    const int kh = t.getHeight() / 3;
    bass.setBounds (t.removeFromTop (kh));
    mid.setBounds (t.removeFromTop (kh));
    treble.setBounds (t);

    r.removeFromTop (10);
    tabs.setBounds (r);
}

void CaptureRackEditor::savePreset()
{
    auto dir = File::getSpecialLocation (File::userDocumentsDirectory).getChildFile ("Capture Rack/Presets");
    dir.createDirectory();
    chooser = std::make_unique<FileChooser> ("Save rig", dir.getChildFile ("My rig.rig"), "*.rig");
    chooser->launchAsync (FileBrowserComponent::saveMode | FileBrowserComponent::warnAboutOverwriting, [this] (const FileChooser& fc)
    {
        auto f = fc.getResult();
        if (f == File()) return;
        MemoryBlock mb;
        proc.getStateInformation (mb);
        if (auto xml = AudioProcessor::getXmlFromBinary (mb.getData(), (int) mb.getSize())) xml->writeTo (f.withFileExtension ("rig"));
    });
}

void CaptureRackEditor::loadPreset()
{
    auto dir = File::getSpecialLocation (File::userDocumentsDirectory).getChildFile ("Capture Rack/Presets");
    chooser = std::make_unique<FileChooser> ("Load rig", dir, "*.rig");
    chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles, [this] (const FileChooser& fc)
    {
        auto f = fc.getResult();
        if (! f.existsAsFile()) return;
        if (auto xml = XmlDocument::parse (f))
        {
            MemoryBlock mb;
            AudioProcessor::copyXmlToBinary (*xml, mb);
            proc.setStateInformation (mb.getData(), (int) mb.getSize());
            refreshAll();
        }
    });
}
