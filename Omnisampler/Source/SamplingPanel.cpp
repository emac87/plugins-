#include "SamplingPanel.h"
#include "PluginEditor.h"

using namespace juce;
using namespace omni;

static constexpr int kLabelW = 118, kRowH = 24, kRowGap = 5;

#if OMNI_HOSTING
// A window for the loaded plug-in's own editor, so its sound can be chosen before it is sampled.
class SamplingPanel::PluginWindow : public DocumentWindow
{
public:
    PluginWindow (const String& name, AudioProcessorEditor* editor, std::function<void()> closed)
        : DocumentWindow (name, look::bg, DocumentWindow::closeButton), onClosed (std::move (closed))
    {
        setUsingNativeTitleBar (true);
        setContentOwned (editor, true);
        setResizable (editor->isResizable(), false);
        setAlwaysOnTop (true);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }
    void closeButtonPressed() override { if (onClosed) onClosed(); }

private:
    std::function<void()> onClosed;
};
#endif

void SamplingPanel::Group::resized()
{
    auto r = getLocalBounds();
    const int total = r.getWidth();
    for (size_t i = 0; i < parts.size(); ++i)
    {
        auto b = i + 1 == parts.size() ? r : r.removeFromLeft (roundToInt ((float) total * parts[i].second));
        parts[i].first->setBounds (i + 1 == parts.size() ? b : b.withTrimmedRight (4));
    }
}

void SamplingPanel::addRow (std::vector<Row>& rows, const String& caption, Component& control)
{
    Row r;
    r.label = std::make_unique<Label> (String(), caption);
    r.label->setColour (Label::textColourId, look::textDim);
    r.label->setFont (FontOptions (12.5f));
    r.control = &control;
    addAndMakeVisible (*r.label);
    addAndMakeVisible (control);
    rows.push_back (std::move (r));
}

void SamplingPanel::layoutRows (std::vector<Row>& rows, Rectangle<int>& area)
{
    for (auto& row : rows)
    {
        auto r = area.removeFromTop (kRowH);
        row.label->setBounds (r.removeFromLeft (kLabelW));
        row.control->setBounds (r);
        area.removeFromTop (kRowGap);
    }
}

SamplingPanel::SamplingPanel (OmniEditor& e) : ed (e), proc (e.proc)
{
    auto title = [this] (Label& l, const String& t)
    {
        l.setText (t, dontSendNotification);
        l.setFont (FontOptions (13.0f, Font::bold));
        l.setColour (Label::textColourId, look::textDim);
        addAndMakeVisible (l);
    };
    title (recTitle, "RECORD FROM THE INPUT");
    title (autoTitle, "AUTO SAMPLER");
    auto bar = [] (Slider& s, double lo, double hi, double step, double value, const String& suffix)
    {
        s.setSliderStyle (Slider::LinearBar);
        s.setRange (lo, hi, step);
        s.setValue (value, dontSendNotification);
        s.setTextValueSuffix (suffix);
    };

    // ---- record ----
    noInput.setText ("The instrument has no audio input inside a DAW. To record sound, put Omnisampler FX in an effects slot, "
                     "or open the standalone app. The Auto Sampler on the right can still sample a plug-in.", dontSendNotification);
    noInput.setFont (FontOptions (12.5f));
    noInput.setColour (Label::textColourId, look::text);
    noInput.setJustificationType (Justification::topLeft);
    addChildComponent (noInput);
    meterLabel.setColour (Label::textColourId, look::textDim);
    meterLabel.setFont (FontOptions (12.5f));
    addAndMakeVisible (meterLabel);

    recMode.addItem ("Record, then Stop", 1);
    recMode.addItem ("Start on sound, stop on silence", 2);
    if (proc.wrapperType != AudioProcessor::wrapperType_Standalone) recMode.addItem ("A number of bars, in time with the host", 3);
    recMode.setSelectedId (1, dontSendNotification);
    recMode.onChange = [this] { updateEnablement(); };
    bar (threshold, -60.0, 0.0, 1.0, -30.0, " dB");
    threshold.onValueChange = [this] { repaint (meterArea); };
    for (int b : { 1, 2, 4, 8, 16 }) barsBox.addItem (String (b) + (b == 1 ? " bar" : " bars"), b);
    barsBox.setSelectedId (1, dontSendNotification);
    recTarget.addItem ("The pad below (replaces its samples)", 1);
    recTarget.addItem ("The pad below, as another velocity layer", 2);
    recTarget.addItem ("The Keys, at the note it hears", 3);
    recTarget.setSelectedId (1, dontSendNotification);
    recTarget.onChange = [this] { updateEnablement(); };
    padName.setJustificationType (Justification::centred);
    padPrev.onClick = [this] { ed.selectPad (jmax (0, ed.selectedPad - 1)); };
    padNext.onClick = [this] { ed.selectPad (jmin (kPads - 1, ed.selectedPad + 1)); };
    padPicker.add (padPrev, 0.14f); padPicker.add (padName, 0.72f); padPicker.add (padNext, 0.14f);
    addRow (recRows, "Start and stop", recMode);
    addRow (recRows, "Start level", threshold);
    addRow (recRows, "Length", barsBox);
    addRow (recRows, "Put it on", recTarget);
    addRow (recRows, "Pad", padPicker);
    trimBtn.setToggleState (true, dontSendNotification);
    normBtn.setToggleState (true, dontSendNotification);
    monitorBtn.setToggleState (proc.monitorInput.load(), dontSendNotification);
    monitorBtn.onClick = [this] { proc.monitorInput = monitorBtn.getToggleState(); };
    normBtn.setTooltip ("Velocity layers keep their recorded level, so soft and hard hits sort themselves");
    recBtn.onClick = [this] { toggleRecording(); };
    recBtn.setColour (TextButton::buttonOnColourId, Colour (0xffd0453a));
    for (auto* c : std::initializer_list<Component*> { &trimBtn, &normBtn, &keepBtn, &monitorBtn, &recBtn }) addAndMakeVisible (c);

    // ---- auto sampler ----
    sourceBox.addItem ("The audio input (notes go out as MIDI)", 1);
   #if OMNI_HOSTING
    sourceBox.addItem ("A plug-in loaded here (fastest)", 2);
   #endif
    sourceBox.setSelectedId (proc.hasAudioInput() || sourceBox.getNumItems() < 2 ? 1 : 2, dontSendNotification);
    sourceBox.onChange = [this] { updateEnablement(); updateSummary(); };
    pluginRow.add (choosePluginBtn, 0.5f); pluginRow.add (showPluginBtn, 0.2f); pluginRow.add (auditionBtn, 0.3f);
   #if OMNI_HOSTING
    choosePluginBtn.onClick = [this] { choosePlugin(); };
    showPluginBtn.onClick = [this] { showPlugin(); };
    auditionBtn.onClick = [this] { proc.audition = auditionBtn.getToggleState(); };
    auditionBtn.setTooltip ("On: the keyboard plays the plug-in. Off: it plays Omnisampler's Keys, so you can compare the copy");
   #endif
    midiOutBox.onChange = [this] { proc.setMidiOutput (midiOutBox.getSelectedItemIndex()); };
    for (int i = 1; i <= 16; ++i) channelBox.addItem ("Ch " + String (i), i);
    channelBox.setSelectedId (1, dontSendNotification);
    midiRow.add (midiOutBox, 0.72f); midiRow.add (channelBox, 0.28f);
    for (int n = 0; n < 128; ++n) { fromNote.addItem (noteName (n) + " (" + String (n) + ")", n + 1); toNote.addItem (noteName (n) + " (" + String (n) + ")", n + 1); }
    fromNote.setSelectedId (36 + 1, dontSendNotification);
    toNote.setSelectedId (84 + 1, dontSendNotification);
    noteRow.add (fromNote, 0.5f); noteRow.add (toNote, 0.5f);
    stepBox.addItem ("Every note", 1);
    stepBox.addItem ("Every 2 semitones", 2);
    stepBox.addItem ("Every 3 semitones (minor thirds)", 3);
    stepBox.addItem ("Every 4 semitones (major thirds)", 4);
    stepBox.addItem ("Every 6 semitones", 6);
    stepBox.addItem ("Every octave", 12);
    stepBox.setSelectedId (3, dontSendNotification);
    for (int i = 1; i <= 8; ++i) layersBox.addItem (String (i) + (i == 1 ? " velocity layer" : " velocity layers"), i);
    layersBox.setSelectedId (3, dontSendNotification);
    for (int i = 1; i <= 4; ++i) rrBox.addItem (i == 1 ? String ("No round robins") : String (i) + " round robins", i);
    rrBox.setSelectedId (1, dontSendNotification);
    layerRow.add (layersBox, 0.5f); layerRow.add (rrBox, 0.5f);
    bar (holdSlider, 0.1, 20.0, 0.1, 2.0, " s");
    bar (tailSlider, 0.1, 20.0, 0.1, 1.5, " s");
    destBox.addItem ("An instrument on the Keys", 1);
    destBox.addItem ("A kit: one pad per note, from the selected pad", 2);
    destBox.setSelectedId (1, dontSendNotification);
    nameEdit.setText ("Auto Sampled", dontSendNotification);
    for (auto* c : { &fromNote, &toNote, &stepBox, &layersBox, &rrBox, &destBox }) c->onChange = [this] { updateSummary(); };
    holdSlider.onValueChange = [this] { updateSummary(); };
    tailSlider.onValueChange = [this] { updateSummary(); };
    addRow (autoRows, "Sample", sourceBox);
    addRow (autoRows, "Plug-in", pluginRow);
    addRow (autoRows, "Send notes to", midiRow);
    addRow (autoRows, "Lowest to highest", noteRow);
    addRow (autoRows, "Take a sample", stepBox);
    addRow (autoRows, "At each note", layerRow);
    addRow (autoRows, "Hold each note for", holdSlider);
    addRow (autoRows, "Then record on for", tailSlider);
    addRow (autoRows, "Make", destBox);
    addRow (autoRows, "Name", nameEdit);
    autoNormBtn.setToggleState (true, dontSendNotification);
    autoNormBtn.setTooltip ("One gain for the whole set, so soft notes stay soft");
    startBtn.onClick = [this] { toggleAutoSampler(); };
    progress.setPercentageDisplay (true);
    for (auto* l : { &summary, &autoStatus })
    {
        l->setColour (Label::textColourId, look::textDim);
        l->setFont (FontOptions (12.5f));
        addAndMakeVisible (l);
    }
    for (auto* c : std::initializer_list<Component*> { &loopBtn, &autoNormBtn, &startBtn }) addAndMakeVisible (c);
    addChildComponent (progress);

    status.setFont (FontOptions (13.0f));
    status.setJustificationType (Justification::centredLeft);
    status.setMinimumHorizontalScale (0.8f);
    addAndMakeVisible (status);
    libraryBtn.setTooltip ("Open something sampled earlier, here or in the other Omnisampler");
    libraryBtn.onClick = [this] { showLibrary(); };
    addAndMakeVisible (libraryBtn);

    updateEnablement();
    updateSummary();
}

SamplingPanel::~SamplingPanel()
{
    stopTimer();
   #if OMNI_HOSTING
    pluginWindow.reset();
   #endif
}

void SamplingPanel::visibilityChanged()
{
    if (! isVisible()) { stopTimer(); return; }
    // The list of MIDI outputs is read when the page opens, so a synth plugged in since then shows up.
    const auto choices = proc.midiOutputChoices();
    midiOutBox.clear (dontSendNotification);
    for (int i = 0; i < choices.size(); ++i) midiOutBox.addItem (choices[i], i + 1);
    // Logic and GarageBand don't pass a plug-in's MIDI output on to other tracks. There the notes go out
    // through a virtual MIDI port instead, which the selected instrument track hears like a keyboard.
    const PluginHostType host;
    if (proc.fx && ! triedDefaultMidi && proc.midiOutputIndex() == 0 && proc.virtualMidiChoice() > 0 && (host.isLogic() || host.isGarageBand() || host.isMainStage()))
    {
        proc.setMidiOutput (proc.virtualMidiChoice());
        status.setText ("Auto Sampler in Logic: put Omnisampler FX after the instrument, keep that track selected, and its notes arrive through the \"Omnisampler\" MIDI port.", dontSendNotification);
    }
    triedDefaultMidi = true;
    midiOutBox.setSelectedItemIndex (jlimit (0, jmax (0, choices.size() - 1), proc.midiOutputIndex()), dontSendNotification);
    refresh();
    startTimerHz (30);
}

void SamplingPanel::refresh()
{
    const int pad = ed.selectedPad;
    const auto inst = proc.getInstrument();
    String name = "Pad " + String::charToString ((juce_wchar) "ABCD"[pad / kPadsPerBank]) + String (pad % kPadsPerBank + 1);
    if (! inst->pads[pad].isEmpty()) name << "  (" << (inst->pads[pad].name.isNotEmpty() ? inst->pads[pad].name : String ("in use")) << ")";
    padName.setText (name, dontSendNotification);
   #if OMNI_HOSTING
    choosePluginBtn.setButtonText (proc.hosted() != nullptr ? proc.hostedName : String ("Choose a plug-in..."));
    auditionBtn.setToggleState (proc.audition.load(), dontSendNotification);
   #endif
    updateEnablement();
}

void SamplingPanel::news()
{
    status.setText (proc.samplingMessage, dontSendNotification);
    // "Keep going" has moved to the next pad: follow it.
    const auto& setup = proc.recordSetup();
    if (proc.recorder.state() != Recorder::State::Idle && setup.target != OmniProcessor::TakeTarget::Keys && setup.pad != ed.selectedPad)
        ed.selectPad (setup.pad);
    refresh();
}

void SamplingPanel::updateEnablement()
{
    const bool in = proc.hasAudioInput();
    const int mode = recMode.getSelectedId(), target = recTarget.getSelectedId();
    noInput.setVisible (! in);
    for (auto* c : std::initializer_list<Component*> { &recMode, &recTarget, &trimBtn, &normBtn, &monitorBtn, &recBtn }) c->setEnabled (in);
    threshold.setEnabled (in && mode == 2);
    barsBox.setEnabled (in && mode == 3);
    trimBtn.setEnabled (in && mode != 3);
    padPicker.setEnabled (in && target != 3);
    keepBtn.setEnabled (in && mode == 2 && target != 3);
    normBtn.setEnabled (in && target != 2);

    const bool plug = sourceIsPlugin();
    pluginRow.setEnabled (plug);
    midiRow.setEnabled (true);
    midiOutBox.setEnabled (! plug);
   #if OMNI_HOSTING
    showPluginBtn.setEnabled (plug && proc.hosted() != nullptr);
    auditionBtn.setEnabled (plug && proc.hosted() != nullptr);
   #else
    pluginRow.setEnabled (false);
   #endif
    repaint();
}

AutoSettings SamplingPanel::autoSettings() const
{
    AutoSettings a;
    a.loNote = jmin (fromNote.getSelectedId(), toNote.getSelectedId()) - 1;
    a.hiNote = jmax (fromNote.getSelectedId(), toNote.getSelectedId()) - 1;
    a.step = jmax (1, stepBox.getSelectedId());
    a.velLayers = jmax (1, layersBox.getSelectedId());
    a.roundRobins = jmax (1, rrBox.getSelectedId());
    a.holdSecs = (float) holdSlider.getValue();
    a.tailSecs = (float) tailSlider.getValue();
    a.channel = jlimit (1, 16, channelBox.getSelectedId());
    a.toPads = destBox.getSelectedId() == 2;
    a.firstPad = ed.selectedPad;
    a.loop = loopBtn.getToggleState();
    a.normalize = autoNormBtn.getToggleState();
    a.name = nameEdit.getText().trim().isNotEmpty() ? nameEdit.getText().trim() : String ("Auto Sampled");
    return a;
}

void SamplingPanel::updateSummary()
{
    const auto a = autoSettings();
    const int samples = (int) autoNotes (a).size();
    const int notes = samples / jmax (1, a.velLayers * a.roundRobins);
    String s = String (notes) + (notes == 1 ? " note" : " notes") + ", " + String (samples) + (samples == 1 ? " sample" : " samples");
    if (sourceIsPlugin()) s << ", rendered faster than real time";
    else
    {
        const int secs = roundToInt (samples * (0.3 + a.holdSecs + a.tailSecs));
        s << ", about " << (secs >= 90 ? String (roundToInt (secs / 60.0)) + " min" : String (secs) + " s");
    }
    if (a.toPads && notes > kPads - a.firstPad) s << ". Only " << (kPads - a.firstPad) << " fit on the pads from here";
    summary.setText (s, dontSendNotification);
}

void SamplingPanel::toggleRecording()
{
    const auto st = proc.recorder.state();
    if (st == Recorder::State::Armed || st == Recorder::State::Recording) { proc.stopRecording(); return; }
    if (st != Recorder::State::Idle) return;
    OmniProcessor::RecordSetup s;
    const int mode = recMode.getSelectedId(), target = recTarget.getSelectedId();
    s.mode = mode == 2 ? Recorder::Mode::Threshold : mode == 3 ? Recorder::Mode::Bars : Recorder::Mode::Manual;
    s.thresholdDb = (float) threshold.getValue();
    s.bars = jmax (1, barsBox.getSelectedId());
    s.trim = trimBtn.getToggleState();
    s.normalize = normBtn.getToggleState();
    s.keepGoing = keepBtn.getToggleState() && keepBtn.isEnabled();
    s.target = target == 2 ? OmniProcessor::TakeTarget::PadLayer : target == 3 ? OmniProcessor::TakeTarget::Keys : OmniProcessor::TakeTarget::PadReplace;
    s.pad = ed.selectedPad;
    if (! proc.startRecording (s)) ed.showMessage ("Can't record", "There's no audio input to record from.");
    else status.setText ({}, dontSendNotification);
}

void SamplingPanel::toggleAutoSampler()
{
    if (proc.autoSampler.state() == AutoSampler::State::Running) { proc.cancelAutoSampler(); return; }
    const auto err = proc.startAutoSampler (autoSettings(), sourceIsPlugin());
    if (err.isNotEmpty()) ed.showMessage ("Auto Sampler", err);
    else status.setText ({}, dontSendNotification);
}

void SamplingPanel::timerCallback()
{
    meter = jmax (proc.recorder.takeLevel(), meter * 0.85f);
    repaint (meterArea);

    const auto st = proc.recorder.state();
    String t = "Record";
    if (st == Recorder::State::Armed)
        t = proc.recorder.mode() == Recorder::Mode::Threshold ? "Waiting for a sound...  (click to cancel)" : "Waiting for the next bar...  (click to cancel)";
    else if (st == Recorder::State::Recording)
        t = "Stop    " + String ((double) proc.recorder.frames() / jmax (1.0, proc.recorder.rate()), 1) + " s";
    if (recBtn.getButtonText() != t) recBtn.setButtonText (t);
    recBtn.setToggleState (st == Recorder::State::Recording, dontSendNotification);

    const bool running = proc.autoSampler.state() == AutoSampler::State::Running;
    if (running)
    {
        const int done = proc.autoSampler.done(), total = jmax (1, proc.autoSampler.total());
        progressValue = (double) done / total;
        const auto n = proc.autoSampler.current();
        autoStatus.setText ("Sampling " + noteName (n.note) + " at velocity " + String (n.vel) + "   (" + String (jmin (total, done + 1)) + " of " + String (total) + ")", dontSendNotification);
    }
    else if (progress.isVisible()) autoStatus.setText ({}, dontSendNotification);
    progress.setVisible (running);
    const String b = running ? "Cancel" : "Start sampling";
    if (startBtn.getButtonText() != b) startBtn.setButtonText (b);
}

void SamplingPanel::paint (Graphics& g)
{
    look::drawPanel (g, recArea.toFloat());
    look::drawPanel (g, autoArea.toFloat());
    // Input meter, -60 to 0 dB, with the start level marked in "start on sound" mode.
    auto m = meterArea.toFloat().reduced (0.0f, 5.0f);
    g.setColour (look::panelHi);
    g.fillRoundedRectangle (m, 3.0f);
    auto toX = [&m] (float db) { return m.getX() + m.getWidth() * jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f); };
    const float db = Decibels::gainToDecibels (meter, -100.0f);
    if (db > -60.0f)
    {
        g.setColour (db > -1.0f ? Colour (0xffd0453a) : ed.lnf.accent);
        g.fillRoundedRectangle (m.withRight (toX (db)), 3.0f);
    }
    if (recMode.getSelectedId() == 2)
    {
        g.setColour (look::text);
        const float x = toX ((float) threshold.getValue());
        g.fillRect (x - 0.5f, m.getY() - 3.0f, 1.5f, m.getHeight() + 6.0f);
    }
    // The last recording, as it was trimmed and saved.
    if (const auto take = proc.lastTake; take != nullptr && ! waveArea.isEmpty())
    {
        auto w = waveArea.toFloat();
        g.setColour (look::textDim);
        g.setFont (FontOptions (12.5f));
        g.drawText ("Last take: " + take->file.getFileNameWithoutExtension() + ",  " + String ((double) take->length() / take->sampleRate, 2) + " s",
                    w.removeFromTop (20.0f), Justification::centredLeft);
        g.setColour (look::bg);
        g.fillRoundedRectangle (w, 4.0f);
        const auto& d = take->drawData();
        const int frames = d.getNumSamples(), cols = jmax (1, (int) w.getWidth());
        const float mid = w.getCentreY(), half = w.getHeight() * 0.46f;
        g.setColour (ed.lnf.accent);
        for (int x = 0; x < cols && frames > 0; ++x)
        {
            const int from = (int) ((int64) x * frames / cols), to = jmax (from + 1, (int) ((int64) (x + 1) * frames / cols));
            const float peak = jmin (1.0f, d.getMagnitude (0, from, jmin (to, frames) - from));
            g.fillRect (w.getX() + (float) x, mid - peak * half, 1.0f, jmax (1.0f, peak * half * 2.0f));
        }
    }
}

void SamplingPanel::resized()
{
    auto r = getLocalBounds();
    auto foot = r.removeFromBottom (36);
    libraryBtn.setBounds (foot.removeFromRight (110).reduced (0, 4));
    foot.removeFromRight (10);
    status.setBounds (foot);
    r.removeFromBottom (4);
    const bool stacked = r.getWidth() < 760;
    recArea = stacked ? r.removeFromTop (r.getHeight() * 9 / 20) : r.removeFromLeft (r.getWidth() / 2 - 6);
    if (stacked) r.removeFromTop (8); else r.removeFromLeft (12);
    autoArea = r;

    auto a = recArea.reduced (14, 10);
    recTitle.setBounds (a.removeFromTop (22));
    a.removeFromTop (4);
    if (noInput.isVisible()) { noInput.setBounds (a.removeFromTop (54)); a.removeFromTop (6); }
    auto m = a.removeFromTop (kRowH);
    meterLabel.setBounds (m.removeFromLeft (kLabelW));
    meterArea = m;
    a.removeFromTop (kRowGap + 3);
    layoutRows (recRows, a);
    a.removeFromTop (3);
    for (auto* t : { &trimBtn, &normBtn, &keepBtn, &monitorBtn }) { t->setBounds (a.removeFromTop (24)); a.removeFromTop (2); }
    a.removeFromTop (8);
    recBtn.setBounds (a.removeFromTop (jmin (40, jmax (24, a.getHeight()))));
    a.removeFromTop (12);
    waveArea = a.getHeight() >= 60 ? a.withHeight (jmin (150, a.getHeight())) : Rectangle<int>();

    auto b = autoArea.reduced (14, 10);
    autoTitle.setBounds (b.removeFromTop (22));
    b.removeFromTop (4);
    layoutRows (autoRows, b);
    b.removeFromTop (3);
    auto toggles = b.removeFromTop (24);
    loopBtn.setBounds (toggles.removeFromLeft (toggles.getWidth() / 2));
    autoNormBtn.setBounds (toggles);
    b.removeFromTop (4);
    summary.setBounds (b.removeFromTop (20));
    b.removeFromTop (6);
    auto go = b.removeFromTop (jmin (40, jmax (24, b.getHeight())));
    startBtn.setBounds (go.removeFromLeft (jmin (190, go.getWidth() / 2)));
    go.removeFromLeft (10);
    progress.setBounds (go.reduced (0, 8));
    b.removeFromTop (4);
    autoStatus.setBounds (b.removeFromTop (20));
}

// Everything sampled so far: auto-sampled instruments and kits to open, and the folders themselves.
void SamplingPanel::showLibrary()
{
    Array<File> found;
    for (auto& f : autoSampledFolder().findChildFiles (File::findFiles, true, "*.sfz;*.omni")) found.add (f);
    found.sort();
    PopupMenu menu;
    for (int i = 0; i < found.size(); ++i)
        menu.addItem (i + 1, found[i].getFileNameWithoutExtension() + (found[i].hasFileExtension ("omni") ? "  (kit)" : "  (instrument)"));
    if (found.isEmpty()) menu.addItem (30000, "Nothing auto-sampled yet", false);
    menu.addSeparator();
    menu.addItem (30001, "Show the library folder");
    menu.showMenuAsync (PopupMenu::Options().withTargetComponent (&libraryBtn), [safe = Component::SafePointer<SamplingPanel> (this), found] (int r)
    {
        if (safe == nullptr || r == 0) return;
        if (r == 30001) { libraryFolder().createDirectory(); libraryFolder().revealToUser(); return; }
        if (r >= 1 && r <= found.size()) safe->ed.openInstrument (found[r - 1]);
    });
}

#if OMNI_HOSTING
void SamplingPanel::choosePlugin()
{
    const auto list = proc.instrumentPlugins();
    PopupMenu menu, au, vst;
    for (int i = 0; i < list.size(); ++i)
        (list[i].format == "AudioUnit" ? au : vst).addItem (i + 1, list[i].name, true, proc.hosted() != nullptr && list[i].name == proc.hostedName);
    if (au.getNumItems() > 0) menu.addSubMenu ("Audio Units", au);
    if (vst.getNumItems() > 0) menu.addSubMenu ("VST3", vst);
    if (list.isEmpty()) menu.addItem (20000, "No instrument plug-ins found", false);
    menu.addSeparator();
    menu.addItem (20001, "Look for plug-ins again");
    if (proc.hosted() != nullptr) menu.addItem (20002, "Unload " + proc.hostedName);
    menu.showMenuAsync (PopupMenu::Options().withTargetComponent (&choosePluginBtn), [safe = Component::SafePointer<SamplingPanel> (this), list] (int r)
    {
        if (safe == nullptr || r == 0) return;
        if (r == 20001) { safe->proc.instrumentPlugins (true); return; }
        if (r == 20002) { safe->pluginWindow.reset(); safe->proc.unloadHosted(); safe->refresh(); return; }
        if (r < 1 || r > list.size()) return;
        const auto choice = list[r - 1];
        safe->pluginWindow.reset();
        safe->status.setText ("Loading " + choice.name + "...", dontSendNotification);
        safe->proc.loadHosted (choice, [safe, choice] (const String& err)
        {
            if (safe == nullptr) return;
            if (err.isNotEmpty())
            {
                safe->status.setText ({}, dontSendNotification);
                safe->ed.showMessage ("Couldn't load the plug-in", err);
            }
            else
            {
                safe->status.setText (choice.name + " is loaded. Pick its sound, play it from the keyboard, then press Start sampling.", dontSendNotification);
                safe->sourceBox.setSelectedId (2, dontSendNotification);
                if (safe->nameEdit.getText().trim() == "Auto Sampled") safe->nameEdit.setText (choice.name, dontSendNotification);
                safe->showPlugin();
            }
            safe->refresh();
            safe->updateSummary();
        });
    });
}

void SamplingPanel::showPlugin()
{
    auto* inst = proc.hosted();
    if (inst == nullptr) return;
    if (pluginWindow != nullptr) { pluginWindow->toFront (true); return; }
    AudioProcessorEditor* editor = inst->hasEditor() ? inst->createEditorIfNeeded() : nullptr;
    if (editor == nullptr) editor = new GenericAudioProcessorEditor (*inst);
    pluginWindow = std::make_unique<PluginWindow> (proc.hostedName, editor, [safe = Component::SafePointer<SamplingPanel> (this)]
    {
        MessageManager::callAsync ([safe] { if (safe != nullptr) safe->pluginWindow.reset(); });
    });
}
#endif
