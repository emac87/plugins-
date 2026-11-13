#include "PluginEditor.h"
#if JucePlugin_Build_Standalone && ! JUCE_IOS && ! JUCE_ANDROID
 // The standalone app mutes its audio input by default (JUCE's guard against feedback). Omnisampler
 // doesn't pass the input through unless asked, so the Sample page switches that mute off.
 #include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
 #define OMNI_STANDALONE_INPUT 1
#else
 #define OMNI_STANDALONE_INPUT 0
#endif
#include "IOSFiles.h"

using namespace juce;
using namespace omni;

static void setupRotary (look::Knob& k, double lo, double hi, double mid, const String& suffix = {})
{
    k.slider.setRange (lo, hi);
    if (mid > lo && mid < hi) k.slider.setSkewFactorFromMidPoint (mid);
    k.slider.setTextValueSuffix (suffix);
    k.slider.setTextBoxStyle (Slider::TextBoxBelow, false, 62, 14);
    k.slider.setNumDecimalPlacesToDisplay (2);
    k.label.setFont (FontOptions (10.5f));
}

// =============================================================================================
VoicingControls::VoicingControls()
{
    setupRotary (vol, -48, 12, -12, " dB");
    setupRotary (pan, -1, 1, 0);
    setupRotary (tune, -24, 24, 0, " st");
    setupRotary (att, 0.0005, 5, 0.1, " s");
    setupRotary (dec, 0.01, 10, 0.5, " s");
    setupRotary (sus, 0, 1, 0.5);
    setupRotary (rel, 0.01, 10, 0.5, " s");
    setupRotary (cut, 20, 20000, 1000, " Hz");
    setupRotary (res, 0, 1, 0.5);
    setupRotary (vs, 0, 1, 0.5);
    cut.slider.setNumDecimalPlacesToDisplay (0);
    ftype.addItemList ({ "Off", "Low pass", "High pass", "Band pass" }, 1);
    rr.addItemList ({ "In order", "Random", "Random, no repeat" }, 1);
    for (auto* l : { &ftypeLabel, &rrLabel }) { l->setColour (Label::textColourId, look::textDim); l->setFont (FontOptions (11.0f)); }
    for (auto* k : { &vol, &pan, &tune, &att, &dec, &sus, &rel, &cut, &res, &vs })
    {
        k->slider.onValueChange = [this] { push(); };
        addAndMakeVisible (k);
    }
    ftype.onChange = [this] { push(); };
    rr.onChange = [this] { push(); };
    for (auto* c : std::initializer_list<Component*> { &ftype, &rr, &ftypeLabel, &rrLabel }) addAndMakeVisible (c);
}

void VoicingControls::refresh()
{
    if (! get) return;
    const auto v = get();
    updating = true;
    vol.slider.setValue (v.volumeDb, dontSendNotification); pan.slider.setValue (v.pan, dontSendNotification);
    tune.slider.setValue (v.tuneSemis, dontSendNotification);
    att.slider.setValue (v.env.attack, dontSendNotification); dec.slider.setValue (v.env.decay, dontSendNotification);
    sus.slider.setValue (v.env.sustain, dontSendNotification); rel.slider.setValue (v.env.release, dontSendNotification);
    cut.slider.setValue (v.filter.cutoff, dontSendNotification); res.slider.setValue (v.filter.resonance, dontSendNotification);
    vs.slider.setValue (v.velSens, dontSendNotification);
    ftype.setSelectedId ((int) v.filter.type + 1, dontSendNotification);
    rr.setSelectedId ((int) v.rr + 1, dontSendNotification);
    cut.setEnabled (v.filter.type != FilterType::Off);
    res.setEnabled (v.filter.type != FilterType::Off);
    updating = false;
}

void VoicingControls::push()
{
    if (updating || ! get || ! set) return;
    auto v = get();
    v.volumeDb = (float) vol.slider.getValue(); v.pan = (float) pan.slider.getValue(); v.tuneSemis = (float) tune.slider.getValue();
    v.env = { (float) att.slider.getValue(), (float) dec.slider.getValue(), (float) sus.slider.getValue(), (float) rel.slider.getValue() };
    v.filter.type = (FilterType) jmax (0, ftype.getSelectedId() - 1);
    v.filter.cutoff = (float) cut.slider.getValue(); v.filter.resonance = (float) res.slider.getValue();
    v.velSens = (float) vs.slider.getValue();
    v.rr = (RRMode) jmax (0, rr.getSelectedId() - 1);
    cut.setEnabled (v.filter.type != FilterType::Off);
    res.setEnabled (v.filter.type != FilterType::Off);
    set (v);
}

void VoicingControls::resized()
{
    auto r = getLocalBounds();
    auto row1 = r.removeFromTop (r.getHeight() / 2);
    const int w = row1.getWidth() / 6;
    for (auto* k : { &vol, &pan, &tune, &vs }) k->setBounds (row1.removeFromLeft (w));
    auto combos = row1.reduced (4, 6);
    ftypeLabel.setBounds (combos.removeFromTop (14)); ftype.setBounds (combos.removeFromTop (24)); combos.removeFromTop (4);
    rrLabel.setBounds (combos.removeFromTop (14)); rr.setBounds (combos.removeFromTop (24));
    const int w2 = r.getWidth() / 6;
    for (auto* k : { &att, &dec, &sus, &rel, &cut, &res }) k->setBounds (r.removeFromLeft (w2));
}

// =============================================================================================
SynthLayersPanel::SynthLayersPanel (OmniEditor& e) : ed (e)
{
    sampleLabel.setColour (Label::textColourId, look::textDim);
    sampleLabel.setFont (FontOptions (11.0f));
    sampleLevel.setSliderStyle (Slider::LinearHorizontal);
    sampleLevel.setRange (-48.0, 12.0);
    sampleLevel.setSkewFactorFromMidPoint (-12.0);
    sampleLevel.setTextValueSuffix (" dB");
    sampleLevel.setNumDecimalPlacesToDisplay (1);
    sampleLevel.setTextBoxStyle (Slider::TextBoxRight, false, 64, 20);
    sampleLevel.setDoubleClickReturnValue (true, 0.0);
    sampleLevel.setTooltip ("Level of the pad's samples against its synth layers");
    sampleLevel.onValueChange = [this] { pushSampleLevel(); };
    addAndMakeVisible (sampleLabel);
    addAndMakeVisible (sampleLevel);

    for (int i = 0; i < kMaxSynthLayers; ++i)
    {
        tab[i].setRadioGroupId (5);
        tab[i].setClickingTogglesState (true);
        tab[i].onClick = [this, i] { if (tab[i].getToggleState()) { layer = i; refresh(); } };
        addChildComponent (tab[i]);
    }
    for (int m = 0; m < (int) DrumModel::NumModels; ++m) model.addItem (drumModelName ((DrumModel) m), m + 1);
    model.onChange = [this] { push(); };
    onBtn.onClick = [this] { push(); };
    removeBtn.setTooltip ("Remove this synth layer");
    removeBtn.onClick = [this]
    {
        const int pad = ed.selectedPad, li = layer;
        ed.proc.edit ([&] (Instrument& in) { auto& sy = in.pads[pad].synths; if (li < (int) sy.size()) sy.erase (sy.begin() + li); });
        ed.refreshAll();
    };
    addBtn.onClick = [this] { ed.addSynthLayerMenu (addBtn); };
    for (auto* c : std::initializer_list<Component*> { &onBtn, &model, &removeBtn, &addBtn }) addChildComponent (c);
    addAndMakeVisible (addBtn);

    setupRotary (level, -48, 12, -12, " dB");
    setupRotary (pan, -1, 1, 0);
    setupRotary (tune, -24, 24, 0, " st");
    setupRotary (decay, 0.02, 4, 0.4, " s");
    setupRotary (tone, 0, 1, 0.5);
    setupRotary (pitchEnv, 0, 1, 0.5);
    setupRotary (pitchDecay, 0.005, 0.5, 0.05, " s");
    setupRotary (click, 0, 1, 0.5);
    setupRotary (noise, 0, 1, 0.5);
    setupRotary (drive, 0, 1, 0.5);
    setupRotary (velSens, 0, 1, 0.5);
    for (auto* k : knobs)
    {
        k->slider.onValueChange = [this] { push(); };
        holder.addAndMakeVisible (k);
    }
    viewport.setViewedComponent (&holder, false);
    viewport.setScrollBarsShown (true, false);
    viewport.setScrollOnDragMode (Viewport::ScrollOnDragMode::nonHover);
    addAndMakeVisible (viewport);
}

void SynthLayersPanel::refresh()
{
    auto inst = ed.proc.getInstrument();
    const auto& p = inst->pads[ed.selectedPad];
    numLayers = (int) p.synths.size();
    layer = jlimit (0, jmax (0, numLayers - 1), layer);
    updating = true;
    sampleLevel.setValue (p.sampleLevelDb, dontSendNotification);
    for (int i = 0; i < kMaxSynthLayers; ++i)
    {
        tab[i].setVisible (i < numLayers);
        tab[i].setToggleState (i == layer, dontSendNotification);
    }
    const bool has = numLayers > 0;
    for (auto* c : std::initializer_list<Component*> { &onBtn, &model, &removeBtn }) c->setVisible (has);
    viewport.setVisible (has);
    addBtn.setEnabled (numLayers < kMaxSynthLayers);
    if (has)
    {
        const auto& s = p.synths[(size_t) layer];
        onBtn.setToggleState (s.on, dontSendNotification);
        model.setSelectedId ((int) s.model + 1, dontSendNotification);
        level.slider.setValue (s.levelDb, dontSendNotification);   pan.slider.setValue (s.pan, dontSendNotification);
        tune.slider.setValue (s.tuneSemis, dontSendNotification);  decay.slider.setValue (s.decay, dontSendNotification);
        tone.slider.setValue (s.tone, dontSendNotification);       pitchEnv.slider.setValue (s.pitchEnv, dontSendNotification);
        pitchDecay.slider.setValue (s.pitchDecay, dontSendNotification);
        click.slider.setValue (s.click, dontSendNotification);     noise.slider.setValue (s.noise, dontSendNotification);
        drive.slider.setValue (s.drive, dontSendNotification);     velSens.slider.setValue (s.velSens, dontSendNotification);
        holder.setEnabled (s.on);
    }
    updating = false;
    resized();
    repaint();
}

void SynthLayersPanel::pushSampleLevel()
{
    if (updating) return;
    const float v = (float) sampleLevel.getValue();
    const int pad = ed.selectedPad;
    ed.proc.edit ([&] (Instrument& in) { in.pads[pad].sampleLevelDb = v; });
}

void SynthLayersPanel::push()
{
    if (updating || numLayers == 0) return;
    SynthLayer s;
    s.model = (DrumModel) jlimit (0, (int) DrumModel::NumModels - 1, model.getSelectedId() - 1);
    s.on = onBtn.getToggleState();
    s.levelDb = (float) level.slider.getValue();      s.pan = (float) pan.slider.getValue();
    s.tuneSemis = (float) tune.slider.getValue();     s.decay = (float) decay.slider.getValue();
    s.tone = (float) tone.slider.getValue();          s.pitchEnv = (float) pitchEnv.slider.getValue();
    s.pitchDecay = (float) pitchDecay.slider.getValue();
    s.click = (float) click.slider.getValue();        s.noise = (float) noise.slider.getValue();
    s.drive = (float) drive.slider.getValue();        s.velSens = (float) velSens.slider.getValue();
    holder.setEnabled (s.on);
    const int pad = ed.selectedPad, li = layer;
    ed.proc.edit ([&] (Instrument& in) { auto& sy = in.pads[pad].synths; if (li < (int) sy.size()) sy[(size_t) li] = s; });
}

void SynthLayersPanel::paint (Graphics& g)
{
    if (numLayers > 0) return;
    g.setColour (look::textDim);
    g.setFont (FontOptions (13.0f));
    g.drawFittedText ("No synth layers. Add a kick, snare, hat... to play with this pad's samples,\nor on its own.",
                      getLocalBounds().withTrimmedTop (62).reduced (10), Justification::centred, 3);
}

void SynthLayersPanel::resized()
{
    auto r = getLocalBounds();
    auto row1 = r.removeFromTop (24);
    sampleLabel.setBounds (row1.removeFromLeft (88));
    sampleLevel.setBounds (row1.removeFromLeft (jmin (row1.getWidth(), 320)));
    r.removeFromTop (4);
    auto row2 = r.removeFromTop (26);
    r.removeFromTop (4);
    addBtn.setBounds (row2.removeFromRight (jmin (112, row2.getWidth() / 2)));
    row2.removeFromRight (4);
    for (int i = 0; i < kMaxSynthLayers; ++i)
        if (i < numLayers) { tab[i].setBounds (row2.removeFromLeft (26).reduced (1, 0)); }
    row2.removeFromLeft (4);
    if (numLayers > 0)
    {
        onBtn.setBounds (row2.removeFromLeft (50));
        removeBtn.setBounds (row2.removeFromRight (26)); row2.removeFromRight (4);
        model.setBounds (row2.removeFromLeft (jmin (150, row2.getWidth())));
    }
    viewport.setBounds (r);
    const int cols = 6, rowH = 74, w = jmax (60, r.getWidth() - viewport.getScrollBarThickness());
    holder.setSize (w, jmax (r.getHeight(), 2 * rowH));
    const int rh = holder.getHeight() / 2, cw = w / cols;
    for (int i = 0; i < 11; ++i) knobs[i]->setBounds ((i % cols) * cw, (i / cols) * rh, cw, rh);
}

// =============================================================================================
ZoneEditor::ZoneEditor (bool keysMode) : keys (keysMode)
{
    auto setup = [this] (Field& f, const String& name, double lo, double hi, double step)
    {
        f.label.setText (name, dontSendNotification);
        f.label.setFont (FontOptions (11.0f));
        f.label.setColour (Label::textColourId, look::textDim);
        f.slider.setSliderStyle (Slider::IncDecButtons);
        f.slider.setTextBoxStyle (Slider::TextBoxLeft, false, 56, 22);
        f.slider.setRange (lo, hi, step);
        f.slider.onValueChange = [this]
        {
            if (updating || ! set || ! get || get() == nullptr) return;
            auto z = shown;
            z.root = (int) root.slider.getValue(); z.loKey = (int) loKey.slider.getValue(); z.hiKey = (int) hiKey.slider.getValue();
            z.loVel = (int) loVel.slider.getValue(); z.hiVel = (int) hiVel.slider.getValue(); z.rrPos = (int) rrPos.slider.getValue();
            z.tuneCents = (float) tune.slider.getValue(); z.gainDb = (float) gain.slider.getValue();
            z.hiKey = jmax (z.hiKey, z.loKey); z.hiVel = jmax (z.hiVel, z.loVel);
            set (z);
        };
        addAndMakeVisible (f.label);
        addAndMakeVisible (f.slider);
    };
    setup (root, "Root", 0, 127, 1); setup (loKey, "Low key", 0, 127, 1); setup (hiKey, "High key", 0, 127, 1);
    setup (loVel, "Low vel", 1, 127, 1); setup (hiVel, "High vel", 1, 127, 1); setup (rrPos, "Round robin #", 0, 32, 1);
    setup (tune, "Tune (cents)", -1200, 1200, 1); setup (gain, "Gain (dB)", -48, 24, 0.1);
    for (auto* f : { &root, &loKey, &hiKey })
    {
        f->slider.textFromValueFunction = [] (double v) { return noteName ((int) v); };
        f->slider.valueFromTextFunction = [] (const String& t) { for (int n = 0; n < 128; ++n) if (noteName (n).equalsIgnoreCase (t.trim())) return (double) n; return (double) t.getIntValue(); };
        f->slider.setTextBoxStyle (Slider::TextBoxLeft, false, 56, 22);
        f->label.setVisible (keys); f->slider.setVisible (keys);
    }
    loop.onClick = [this]
    {
        if (updating || ! set || ! get || get() == nullptr) return;
        auto z = shown;
        z.loop = loop.getToggleState();
        if (z.loop && z.loopEnd <= z.loopStart && z.sample)
        {
            z.loopStart = z.sample->length() / 4;
            z.loopEnd = z.sample->length() - 1;
        }
        set (z);
    };
    loop.setVisible (keys);
    addAndMakeVisible (loop);
    fileName.setFont (FontOptions (12.0f));
    fileName.setMinimumHorizontalScale (0.5f);
    addAndMakeVisible (fileName);
}

void ZoneEditor::refresh()
{
    const Zone* z = get ? get() : nullptr;
    updating = true;
    setEnabled (z != nullptr);
    if (z != nullptr)
    {
        shown = *z;
        root.slider.setValue (z->root, dontSendNotification); loKey.slider.setValue (z->loKey, dontSendNotification);
        hiKey.slider.setValue (z->hiKey, dontSendNotification); loVel.slider.setValue (z->loVel, dontSendNotification);
        hiVel.slider.setValue (z->hiVel, dontSendNotification); rrPos.slider.setValue (z->rrPos, dontSendNotification);
        tune.slider.setValue (z->tuneCents, dontSendNotification); gain.slider.setValue (z->gainDb, dontSendNotification);
        loop.setToggleState (z->loop, dontSendNotification);
        fileName.setText (z->sample ? z->sample->file.getFileName() + "   (" + String (z->sample->loudDb, 1) + " dB loudest)" + (z->sample->streamed ? "   streaming" : "") : String(), dontSendNotification);
    }
    else
    {
        shown = {};
        fileName.setText ("No zone selected", dontSendNotification);
    }
    updating = false;
    repaint();
}

void ZoneEditor::paint (Graphics& g)
{
    auto wave = getLocalBounds().removeFromTop (70).reduced (0, 4).toFloat().withTrimmedTop (18);
    g.setColour (look::panelHi);
    g.fillRoundedRectangle (wave, 4.0f);
    if (shown.sample == nullptr) return;
    const auto& d = shown.sample->drawData();
    const int n = d.getNumSamples();
    const int len = jmax (1, shown.sample->length());   // whole-sample length in frames (a streamed sample only keeps its start in memory)
    const bool streamed = shown.sample->streamed;       // drawData is then one channel of peaks, drawn symmetric
    if (n == 0) return;
    g.setColour (findParentComponentOfClass<OmniEditor>() ? findParentComponentOfClass<OmniEditor>()->lnf.accent : look::text);
    const int W = (int) wave.getWidth();
    const float mid = wave.getCentreY();
    float pk = 0.0f;
    for (int c = 0; c < d.getNumChannels(); ++c) pk = jmax (pk, d.getMagnitude (c, 0, n));
    const float h = wave.getHeight() * 0.45f / jmax (pk, 1.0e-4f);   // drawn normalised
    for (int x = 0; x < W; ++x)
    {
        const int s0 = jmin (n - 1, (int) ((int64) x * n / W)), s1 = jlimit (s0 + 1, n, (int) ((int64) (x + 1) * n / W));
        auto mm = d.findMinMax (0, s0, s1 - s0);
        if (streamed)
        {
            const float a = jmax (std::abs (mm.getStart()), std::abs (mm.getEnd())) * h;
            g.drawVerticalLine ((int) wave.getX() + x, mid - a, mid + a + 1.0f);
        }
        else
            g.drawVerticalLine ((int) wave.getX() + x, mid - mm.getEnd() * h, mid - mm.getStart() * h + 1.0f);
    }
    if (shown.loop && shown.loopEnd > shown.loopStart)
    {
        g.setColour (Colours::yellow.withAlpha (0.6f));
        g.drawVerticalLine ((int) (wave.getX() + wave.getWidth() * (float) shown.loopStart / (float) len), wave.getY(), wave.getBottom());
        g.drawVerticalLine ((int) (wave.getX() + wave.getWidth() * (float) shown.loopEnd / (float) len), wave.getY(), wave.getBottom());
    }
}

void ZoneEditor::resized()
{
    auto r = getLocalBounds();
    fileName.setBounds (r.removeFromTop (18));
    r.removeFromTop (54);
    auto cols = r;
    const int colW = cols.getWidth() / 2;
    auto left = cols.removeFromLeft (colW).reduced (2, 0), right = cols.reduced (2, 0);
    const int rowH = jlimit (18, 26, r.getHeight() / (keys ? 5 : 3) - 3);   // rows shrink a little on small screens
    auto place = [rowH] (Rectangle<int>& col, Field& f) { auto row = col.removeFromTop (rowH); f.label.setBounds (row.removeFromLeft (84)); f.slider.setBounds (row); col.removeFromTop (3); };
    if (keys) { place (left, root); place (left, loKey); place (left, hiKey); }
    place (left, loVel); place (left, hiVel);
    place (right, rrPos); place (right, tune); place (right, gain);
    if (keys) loop.setBounds (right.removeFromTop (24));
}

#if JUCE_IOS
static const char* const emptyPadHint = "hold to load";
#else
static const char* const emptyPadHint = "drop samples";
#endif

// =============================================================================================
PadButton::PadButton (OmniEditor& e, int s) : ed (e), slot (s) {}

int PadButton::padIndex() const { return ed.bank * kPadsPerBank + slot; }

void PadButton::paint (Graphics& g)
{
    const int pi = padIndex();
    auto inst = ed.proc.getInstrument();
    const auto& p = inst->pads[pi];
    auto r = getLocalBounds().toFloat().reduced (3);
    const bool sel = pi == ed.selectedPad;
    const bool has = ! p.isEmpty();
    g.setColour (has ? look::panelHi : look::panel);
    g.fillRoundedRectangle (r, 7.0f);
    if (flash > 0.01f)
    {
        g.setColour (ed.lnf.accent.withAlpha (0.55f * flash));
        g.fillRoundedRectangle (r, 7.0f);
    }
    g.setColour (hover ? Colours::white : sel ? ed.lnf.accent : look::outline);
    g.drawRoundedRectangle (r, 7.0f, sel || hover ? 2.0f : 1.0f);
    g.setColour (look::textDim);
    g.setFont (FontOptions (11.0f));
    g.drawText (String::charToString ((juce_wchar) "ABCD"[ed.bank]) + String (slot + 1) + "  " + noteName (p.note), r.reduced (8, 6), Justification::topLeft);
    g.setColour (has ? look::text : look::textDim.withAlpha (0.5f));
    g.setFont (FontOptions (13.0f, Font::bold));
    g.drawFittedText (has ? (p.name.isNotEmpty() ? p.name : p.synths.empty() ? String ("Pad") : String (drumModelName (p.synths[0].model))) : String (emptyPadHint), r.reduced (8, 20).toNearestInt(), Justification::centred, 2);
    if (has)
    {
        int rr = 0;
        for (auto& z : p.zones) rr = jmax (rr, z.rrPos);
        g.setColour (look::textDim);
        g.setFont (FontOptions (10.5f));
        String info;
        if (! p.zones.empty()) info = String ((int) p.zones.size()) + (p.zones.size() == 1 ? " sample" : " samples");
        if (! p.synths.empty()) info << (info.isEmpty() ? "" : "  ") << (int) p.synths.size() << (p.synths.size() == 1 ? " synth" : " synths");
        if (rr > 0) info << "  RR" << rr;
        if (p.chokeGroup > 0) info << "  choke " << p.chokeGroup;
        g.drawText (info, r.reduced (8, 6), Justification::bottomLeft);
    }
}

// Velocity for a tap on the on-screen pads: fixed, or (touch velocity on) louder towards the top of the pad.
static float padVelocity (bool touch, const MouseEvent& e, int height)
{
    if (! touch || height <= 0) return 0.8f;
    return jlimit (0.08f, 1.0f, 1.0f - e.position.y / (float) height * 0.92f);
}

PadButton::~PadButton() { stopTimer(); release(); }

// A tap plays the pad at once (a synth layer on an otherwise empty pad too). On iPad/iPhone,
// where there's no drag and drop, holding a pad for half a second opens the file picker instead.
void PadButton::mouseDown (const MouseEvent& e)
{
    ed.selectPad (padIndex());
    const auto& p = ed.proc.getInstrument()->pads[padIndex()];
    release();
    if (! p.isEmpty())
    {
        playing = p.note; playingCh = ed.playChannel (true);
        ed.proc.padState.noteOn (playingCh, playing, padVelocity (ed.proc.touchVelocity.load(), e, getHeight()));
    }
   #if JUCE_IOS
    startTimer (500);
   #endif
}

void PadButton::mouseDrag (const MouseEvent& e)
{
    if (e.getDistanceFromDragStart() > 12) stopTimer();   // a swipe, not a hold
}

void PadButton::mouseUp (const MouseEvent&)
{
    stopTimer();
    release();
}

void PadButton::release()
{
    if (playing >= 0) ed.proc.padState.noteOff (playingCh, playing, 0.0f);
    playing = -1;
}

void PadButton::timerCallback()
{
    stopTimer();
    if (! isMouseButtonDown()) return;
    release();
    ed.browsePadSamples (padIndex());
}

void PadButton::filesDropped (const StringArray& files, int, int)
{
    hover = false;
    Array<File> fs;
    for (auto& f : files)
    {
        File file (f);
        if (file.isDirectory()) fs.addArray (file.findChildFiles (File::findFiles, false, "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg"));
        else if (isAudioFile (file)) fs.add (file);
    }
    if (fs.isEmpty()) { ed.showMessage ("Nothing to load", "Drop audio files (WAV, AIFF, FLAC, MP3, OGG) on a pad."); return; }
    const auto msg = ed.proc.loadFilesToPad (padIndex(), fs, ed.dropMode());
    ed.selectPad (padIndex());
    if (msg.isNotEmpty()) ed.showMessage ("Some files didn't load", msg);
}

// =============================================================================================
ZoneMap::ZoneMap (OmniEditor& e) : ed (e) {}

void ZoneMap::paint (Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (look::panel);
    g.fillRoundedRectangle (r, 6.0f);
    const float kw = r.getWidth() / 128.0f;
    for (int k = 0; k < 128; ++k)
        if (k % 12 == 0)
        {
            g.setColour (Colours::white.withAlpha (0.06f));
            g.drawVerticalLine ((int) (r.getX() + k * kw), r.getY(), r.getBottom());
            g.setColour (look::textDim);
            g.setFont (FontOptions (10.0f));
            g.drawText (noteName (k), (int) (r.getX() + k * kw) + 2, (int) r.getBottom() - 14, 30, 12, Justification::left);
        }
    auto inst = ed.proc.getInstrument();
    const auto& zones = inst->keys.zones;
    const float h = r.getHeight() - 16.0f;
    for (int i = 0; i < (int) zones.size(); ++i)
    {
        const auto& z = zones[(size_t) i];
        const float x0 = r.getX() + z.loKey * kw, x1 = r.getX() + (z.hiKey + 1) * kw;
        const float y0 = r.getY() + (127 - z.hiVel) / 127.0f * h, y1 = r.getY() + (128 - z.loVel) / 127.0f * h;
        const auto col = Colour::fromHSV (std::fmod (0.7f + (z.root % 12) * 0.083f, 1.0f), 0.5f, 0.9f, 1.0f);
        const bool sel = i == ed.selectedKeyZone;
        g.setColour (col.withAlpha (sel ? 0.55f : 0.22f));
        g.fillRect (Rectangle<float> (x0, y0, x1 - x0, y1 - y0).reduced (0.5f));
        g.setColour (col.withAlpha (sel ? 1.0f : 0.6f));
        g.drawRect (Rectangle<float> (x0, y0, x1 - x0, y1 - y0).reduced (0.5f), sel ? 2.0f : 1.0f);
        g.setColour (Colours::white);
        g.fillRect (r.getX() + (z.root + 0.5f) * kw - 1.0f, y1 - 6.0f, 2.0f, 5.0f);
    }
    if (zones.empty())
    {
        g.setColour (look::textDim);
        g.setFont (FontOptions (15.0f));
        g.drawFittedText (
                         #if JUCE_IOS
                          "Use Load instrument or Add samples above.\n"
                         #else
                          "Drop samples, a folder, or an instrument (.sfz, .dspreset, .sf2, .exs, .nki, .zip...) here.\n"
                         #endif
                          "Notes are read from file names (C4 = middle C), file metadata, or detected from the audio.\n"
                          "Several samples on the same note become velocity layers by loudness.",
                          getLocalBounds().reduced (12), Justification::centred, 4);
    }
    if (hover) { g.setColour (ed.lnf.accent); g.drawRoundedRectangle (r.reduced (1), 6.0f, 2.0f); }
}

void ZoneMap::mouseDown (const MouseEvent& e)
{
    auto inst = ed.proc.getInstrument();
    const auto& zones = inst->keys.zones;
    const auto r = getLocalBounds().toFloat();
    const int key = (int) ((e.position.x - r.getX()) / (r.getWidth() / 128.0f));
    const int vel = 127 - (int) ((e.position.y - r.getY()) / (r.getHeight() - 16.0f) * 127.0f);
    for (int i = (int) zones.size() - 1; i >= 0; --i)
    {
        const auto& z = zones[(size_t) i];
        if (key >= z.loKey && key <= z.hiKey && vel >= z.loVel && vel <= z.hiVel) { ed.selectKeyZone (i); return; }
    }
}

void ZoneMap::filesDropped (const StringArray& files, int, int)
{
    hover = false;
    Array<File> fs;
    for (auto& f : files) fs.add (File (f));
    const bool replace = ed.proc.getInstrument()->keys.zones.empty() || ! ModifierKeys::currentModifiers.isShiftDown();
    const auto msg = ed.proc.loadFilesToKeys (fs, replace);
    ed.selectKeyZone (0);
    if (msg.isNotEmpty()) ed.showMessage ("Note", msg);
}

// =============================================================================================
OmniEditor::OmniEditor (OmniProcessor& p)
    : AudioProcessorEditor (p), proc (p), keyboard (p.keyboardState, MidiKeyboardComponent::horizontalKeyboard)
{
    setLookAndFeel (&lnf);
    shownInst = proc.getInstrument();
    logo.setText (proc.fx ? "OMNISAMPLER FX" : "OMNISAMPLER", dontSendNotification);
    logo.setFont (FontOptions (19.0f, Font::bold));
    logo.setColour (Label::textColourId, lnf.accent);
    voices.setColour (Label::textColourId, look::textDim);
    voices.setFont (FontOptions (12.0f));

    padsTab.setRadioGroupId (1); keysTab.setRadioGroupId (1); sampleTab.setRadioGroupId (1);
    padsTab.setClickingTogglesState (true); keysTab.setClickingTogglesState (true); sampleTab.setClickingTogglesState (true);
    padsTab.onClick = [this] { setView (false); };
    keysTab.onClick = [this] { setView (true); };
    sampleTab.onClick = [this] { setSampleView(); };
    sampleTab.setTooltip ("Record from the audio input, or auto-sample an instrument");
    addChildComponent (sampleTab);
    sampleTab.setVisible (OmniProcessor::canSample());
    addChildComponent (samplingPanel);
    aMaster = std::make_unique<AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, "master", masterKnob.slider);
    masterKnob.slider.setTextBoxStyle (Slider::NoTextBox, false, 0, 0);
    masterKnob.slider.setPopupDisplayEnabled (true, true, this);
    masterKnob.label.setFont (FontOptions (10.0f));
    for (auto* c : { &padsCh, &keysCh })
    {
        c->addItem ("Any", 1);
        for (int i = 1; i <= 16; ++i) c->addItem ("Ch " + String (i), i + 1);
    }
    aPadsCh = std::make_unique<AudioProcessorValueTreeState::ComboBoxAttachment> (proc.apvts, "padsch", padsCh);
    aKeysCh = std::make_unique<AudioProcessorValueTreeState::ComboBoxAttachment> (proc.apvts, "keysch", keysCh);
    keysCh.onChange = [this] { keyboard.setMidiChannel (playChannel (false)); };
    padsCh.setTooltip ("MIDI channel the pads answer to");
    keysCh.setTooltip ("MIDI channel the keys answer to");
    for (auto* l : { &padsChLabel, &keysChLabel }) { l->setColour (Label::textColourId, look::textDim); l->setFont (FontOptions (11.0f)); l->setJustificationType (Justification::centredRight); }

    loadKitBtn.onClick = [this] { browseKit(); };
    saveKitBtn.onClick = [this] { saveKitAs(); };
    optionsBtn.setTooltip ("Options");
    optionsBtn.onClick = [this] { showOptionsMenu(); };
    voices.setJustificationType (Justification::centredRight);

    // ---- pads ----
    for (int b = 0; b < 4; ++b)
    {
        bankBtn[b].setRadioGroupId (2);
        bankBtn[b].setClickingTogglesState (true);
        bankBtn[b].onClick = [this, b] { bank = b; selectPad (b * kPadsPerBank + selectedPad % kPadsPerBank); };
        addAndMakeVisible (bankBtn[b]);
    }
    bankBtn[0].setToggleState (true, dontSendNotification);
    for (int i = 0; i < kPadsPerBank; ++i) { padButtons[i] = std::make_unique<PadButton> (*this, i); addAndMakeVisible (*padButtons[i]); }

    padTitle.setFont (FontOptions (16.0f, Font::bold));
    padTitle.setEditable (false, true);
    padTitle.onTextChange = [this] { const auto t = padTitle.getText(); proc.edit ([&] (Instrument& in) { in.pads[selectedPad].name = t; }); refreshAll(); };
    for (int n = 0; n < 128; ++n) padNote.addItem (noteName (n) + " (" + String (n) + ")", n + 1);
    padNote.onChange = [this] { const int n = padNote.getSelectedId() - 1; if (n >= 0) proc.edit ([&] (Instrument& in) { in.pads[selectedPad].note = n; }); refreshAll(); };
    chokeBox.addItem ("None", 1);
    for (int i = 1; i <= 8; ++i) chokeBox.addItem ("Group " + String (i), i + 1);
    chokeBox.onChange = [this] { const int c = chokeBox.getSelectedId() - 1; proc.edit ([&] (Instrument& in) { in.pads[selectedPad].chokeGroup = jmax (0, c); }); refreshAll(); };
    dropModeBox.addItem ("Velocity layers (by loudness)", 1);
    dropModeBox.addItem ("Round robins", 2);
    dropModeBox.setSelectedId (1, dontSendNotification);
    for (int i = 0; i < kOutputs; ++i) { padOutput.addItem (OmniProcessor::outputName (i), i + 1); keysOutput.addItem (OmniProcessor::outputName (i), i + 1); }
    padOutput.onChange = [this] { const int o = padOutput.getSelectedId() - 1; if (o >= 0) proc.edit ([&] (Instrument& in) { in.pads[selectedPad].output = o; }); };
    keysOutput.onChange = [this] { const int o = keysOutput.getSelectedId() - 1; if (o >= 0) proc.edit ([&] (Instrument& in) { in.keys.output = o; }); };
    padOutput.setTooltip ("Which of the plug-in's stereo outputs this pad plays on");
    keysOutput.setTooltip ("Which of the plug-in's stereo outputs the keys play on");
    for (auto* l : { &padOutputLabel, &keysOutputLabel }) { l->setColour (Label::textColourId, look::textDim); l->setFont (FontOptions (11.0f)); }
    for (int i = 0; i < 2; ++i)
    {
        padBottomTab[i].setRadioGroupId (4);
        padBottomTab[i].setClickingTogglesState (true);
        padBottomTab[i].onClick = [this, i] { if (padBottomTab[i].getToggleState()) { padBottom = i; applyVisibility(); resized(); } };
    }
    padBottomTab[0].setToggleState (true, dontSendNotification);
    addSynthBtn.onClick = [this] { addSynthLayerMenu (addSynthBtn); };
    oneShot.onClick = [this] { const bool v = oneShot.getToggleState(); proc.edit ([&] (Instrument& in) { in.pads[selectedPad].oneShot = v; }); };
    for (auto* l : { &padNoteLabel, &chokeLabel, &dropLabel }) { l->setColour (Label::textColourId, look::textDim); l->setFont (FontOptions (11.0f)); }
    addToPadBtn.onClick = [this] { browsePadSamples (selectedPad); };
    clearPadBtn.onClick = [this] { proc.edit ([this] (Instrument& in) { in.pads[selectedPad].zones.clear(); in.pads[selectedPad].synths.clear(); in.pads[selectedPad].sampleLevelDb = 0.0f; in.pads[selectedPad].name.clear(); }); refreshAll(); };
    remapBtn.onClick = [this]
    {
        proc.edit ([this] (Instrument& in)
        {
            std::vector<Zone*> ptrs;
            for (auto& z : in.pads[selectedPad].zones) { z.rrPos = 0; ptrs.push_back (&z); }
            assignVelocityByLoudness (ptrs);
        });
        refreshAll();
    };
    removeZoneBtn.onClick = [this] { deleteKeyPressed (zoneList.getSelectedRow()); };

    padZoneEditor.get = [this]() -> const Zone*
    {
        const auto& zs = shownInst->pads[selectedPad].zones;
        return selectedPadZone >= 0 && selectedPadZone < (int) zs.size() ? &zs[(size_t) selectedPadZone] : nullptr;
    };
    padZoneEditor.set = [this] (const Zone& z)
    {
        proc.edit ([&] (Instrument& in) { auto& zs = in.pads[selectedPad].zones; if (selectedPadZone < (int) zs.size()) zs[(size_t) selectedPadZone] = z; });
        shownInst = proc.getInstrument();
        zoneList.repaint();
    };
    padVoicing.get = [this] { return proc.getInstrument()->pads[selectedPad].voicing; };
    padVoicing.set = [this] (const Voicing& v) { proc.edit ([&] (Instrument& in) { in.pads[selectedPad].voicing = v; }); };

    // ---- keys ----
    keysTitle.setFont (FontOptions (16.0f, Font::bold));
    keyboard.setAvailableRange (21, 108);
    applyTouchVelocity();
    keyboard.setMidiChannel (1);
    loadInstBtn.onClick = [this] { browseInstrument(); };
    addKeysBtn.onClick = [this] { browseKeySamples(); };
   #if JUCE_IOS
    saveSfzBtn.onClick = [this] { showSaveMenu(); };
   #else
    saveSfzBtn.onClick = [this] { saveSfzAs(); };
   #endif
    saveDsBtn.onClick = [this] { saveDecentSamplerAs(); };
    clearKeysBtn.onClick = [this] { proc.edit ([] (Instrument& in) { in.keys.zones.clear(); in.keys.name.clear(); }); selectKeyZone (0); };

    keyZoneEditor.get = [this]() -> const Zone*
    {
        const auto& zs = shownInst->keys.zones;
        return selectedKeyZone >= 0 && selectedKeyZone < (int) zs.size() ? &zs[(size_t) selectedKeyZone] : nullptr;
    };
    keyZoneEditor.set = [this] (const Zone& z)
    {
        proc.edit ([&] (Instrument& in) { auto& zs = in.keys.zones; if (selectedKeyZone < (int) zs.size()) zs[(size_t) selectedKeyZone] = z; });
        shownInst = proc.getInstrument();
        zoneList.repaint();
        zoneMap.repaint();
    };
    keyVoicing.get = [this] { return proc.getInstrument()->keys.voicing; };
    keyVoicing.set = [this] (const Voicing& v) { proc.edit ([&] (Instrument& in) { in.keys.voicing = v; }); };

    zoneList.setRowHeight (22);

    for (auto* c : std::initializer_list<Component*> { &logo, &voices, &padsTab, &keysTab, &masterKnob, &padsCh, &keysCh, &padsChLabel, &keysChLabel,
                                                       &loadKitBtn, &saveKitBtn, &padTitle, &padNote, &chokeBox, &dropModeBox, &oneShot, &padNoteLabel,
                                                       &chokeLabel, &dropLabel, &addToPadBtn, &clearPadBtn, &remapBtn, &removeZoneBtn, &zoneMap, &keyboard,
                                                       &keysTitle, &loadInstBtn, &addKeysBtn, &saveSfzBtn, &clearKeysBtn, &zoneList, &padZoneEditor,
                                                       &keyZoneEditor, &padVoicing, &keyVoicing, &optionsBtn, &layersPanel, &saveDsBtn, &addSynthBtn,
                                                       &padBottomTab[0], &padBottomTab[1] })
        addAndMakeVisible (c);
    for (auto* c : std::initializer_list<Component*> { &padOutput, &keysOutput, &padOutputLabel, &keysOutputLabel }) addChildComponent (c);

    // Small screens show one of these three panels at a time (see isCompact).
    for (int i = 0; i < 4; ++i)
    {
        detailBtn[i].setRadioGroupId (3);
        detailBtn[i].setClickingTogglesState (true);
        detailBtn[i].onClick = [this, i] { if (detailBtn[i].getToggleState()) { detail = i; applyVisibility(); resized(); } };
        addChildComponent (detailBtn[i]);
    }
    detailBtn[0].setToggleState (true, dontSendNotification);

    setResizable (true, true);
   #if JUCE_IOS
    setResizeLimits (300, 240, 4000, 4000);   // phones and AUv3 hosts give whatever space they have
    addToPadBtn.setButtonText ("Load samples...");
    saveSfzBtn.setButtonText ("Save as...");
   #else
    setResizeLimits (1000, 700, 2200, 1400);
   #endif
    setSize (1220, 800);
    padsTab.setToggleState (true, dontSendNotification);
    setView (false);
    selectPad (0);
    lastSamplingSerial = proc.samplingSerial.load();
    if (proc.fx) setSampleView();   // the effect is there to sample: open on that page
    startTimerHz (30);
}

OmniEditor::~OmniEditor() { setLookAndFeel (nullptr); }

int OmniEditor::playChannel (bool pads) const
{
    const int ch = pads ? proc.padsChannel() : proc.keysChannel();
    return ch == 0 ? (pads ? 10 : 1) : ch;
}

void OmniEditor::showMessage (const String& title, const String& text)
{
    AlertWindow::showMessageBoxAsync (MessageBoxIconType::InfoIcon, title, text, "OK", this);
}

void OmniEditor::setSampleView()
{
    if (! OmniProcessor::canSample()) return;
    showingSample = true;
    padsTab.setToggleState (false, dontSendNotification);
    keysTab.setToggleState (false, dontSendNotification);
    sampleTab.setToggleState (true, dontSendNotification);
   #if OMNI_STANDALONE_INPUT
    if (auto* holder = StandalonePluginHolder::getInstance()) holder->getMuteInputValue().setValue (false);
   #endif
    applyVisibility();
    keyboard.setMidiChannel (playChannel (false));
    resized();
}

void OmniEditor::setView (bool keys)
{
    showingKeys = keys;
    showingSample = false;
    sampleTab.setToggleState (false, dontSendNotification);
    if (keys && detail == 3) { detail = 2; detailBtn[2].setToggleState (true, dontSendNotification); }
    padsTab.setToggleState (! keys, dontSendNotification);
    keysTab.setToggleState (keys, dontSendNotification);
    applyVisibility();
    keyboard.setMidiChannel (playChannel (false));
    refreshAll();
    resized();
}

bool OmniEditor::isCompact() const { return getWidth() < 900 || getHeight() < 600; }

// Which controls show: the Pads or Keys page, and on small screens only one detail panel.
void OmniEditor::applyVisibility()
{
    const bool keys = showingKeys, c = isCompact();
    const bool list = ! c || detail == 0, edit = ! c || detail == 1, sound = ! c || detail == 2;
    const bool padVoicingShown = c ? detail == 2 : padBottom == 0, padLayersShown = c ? detail == 3 : padBottom == 1;
    const bool sp = showingSample;   // the Sample page replaces the Pads / Keys pages
    auto show = [sp] (std::initializer_list<Component*> cs, bool v) { for (auto* comp : cs) comp->setVisible (v && ! sp); };
    show ({ &bankBtn[0], &bankBtn[1], &bankBtn[2], &bankBtn[3], &padTitle, &addToPadBtn }, ! keys);
    for (auto& pb : padButtons) pb->setVisible (! keys && ! sp);
    show ({ &padNote, &chokeBox, &dropModeBox, &oneShot, &padNoteLabel, &chokeLabel, &dropLabel, &clearPadBtn, &remapBtn }, ! keys && list);
    show ({ &padZoneEditor }, ! keys && edit);
    show ({ &padVoicing }, ! keys && padVoicingShown);
    show ({ &layersPanel }, ! keys && padLayersShown);
    show ({ &addSynthBtn }, ! keys && c && list);
    show ({ &padBottomTab[0], &padBottomTab[1] }, ! keys && ! c);
    show ({ &zoneMap, &keyboard, &keysTitle, &loadInstBtn, &addKeysBtn, &saveSfzBtn, &clearKeysBtn }, keys);
    show ({ &keyZoneEditor }, keys && edit);
    show ({ &keyVoicing }, keys && sound);
    show ({ &zoneList, &removeZoneBtn }, list);
    show ({ &detailBtn[0], &detailBtn[1], &detailBtn[2] }, c);
    show ({ &detailBtn[3] }, c && ! keys);
   #if ! JUCE_IOS
    show ({ &saveDsBtn }, keys);
   #else
    saveDsBtn.setVisible (false);
   #endif
    // Output pickers: plug-in only (the standalone app plays Main). Small screens show them on the Sound panel.
    const bool outs = proc.hasOutputPicker();
    show ({ &padOutput, &padOutputLabel }, outs && ! keys && (c ? detail == 2 : list));
    show ({ &keysOutput, &keysOutputLabel }, outs && keys && sound);
    voices.setVisible (true);
    // The Sample page keeps the keyboard, to try a plug-in before sampling it and the result afterwards.
    if (sp) keyboard.setVisible (true);
    samplingPanel.setVisible (sp);
    zoneList.setRowHeight (c ? 30 : 22);
}

// ---- loading and saving -----------------------------------------------------------------------
// Desktop uses the normal file dialogs. iPhone/iPad goes through omni::ios::pickAndImport, which
// copies the picked files into the app first (see IOSFiles.h).

void OmniEditor::padFilesChosen (int pad, const Array<File>& files)
{
    Array<File> audio;
    for (auto& f : files)
    {
        if (f.isDirectory()) audio.addArray (f.findChildFiles (File::findFiles, false, "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg"));
        else if (isAudioFile (f)) audio.add (f);
    }
    if (audio.isEmpty()) { showMessage ("Nothing to load", "Pick audio files (WAV, AIFF, FLAC, MP3, OGG) for a pad."); return; }
    const auto msg = proc.loadFilesToPad (pad, audio, dropMode());
    selectPad (pad);
    if (msg.isNotEmpty()) showMessage ("Some files didn't load", msg);
}

void OmniEditor::instrumentChosen (const File& f)
{
    if (f == File()) return;
    if (f.hasFileExtension ("omni"))
    {
        const auto msg = proc.loadKit (f);
        refreshAll();
        if (msg.isNotEmpty()) showMessage ("Note", msg);
        return;
    }
    if (f.isDirectory())
    {
        // A folder: use the instrument file inside it, or map its audio files across the keys.
        auto found = f.findChildFiles (File::findFiles, true, OmniProcessor::instrumentPatterns().replace ("*.zip", "").replace (";;", ";").trimCharactersAtEnd (";"));
        found.sort();
        if (found.size() > 1)
        {
            PopupMenu m;
            for (int i = 0; i < found.size(); ++i) m.addItem (i + 1, found[i].getRelativePathFrom (f));
            m.showMenuAsync (PopupMenu::Options().withTargetComponent (showingKeys ? (Component*) &loadInstBtn : (Component*) &loadKitBtn),
                             [safe = Component::SafePointer<OmniEditor> (this), found] (int r) { if (safe != nullptr && r > 0) safe->instrumentChosen (found[r - 1]); });
            return;
        }
        if (found.size() == 1) { instrumentChosen (found[0]); return; }
        const auto msg = proc.loadFilesToKeys ({ f }, true);
        setView (true);
        selectKeyZone (0);
        if (msg.isNotEmpty()) showMessage ("Note", msg);
        return;
    }
    if (! OmniProcessor::canLoadInstrument (f))
    {
        showMessage ("Can't open that", f.getFileName() + " isn't a kit or instrument Omnisampler can open yet.\n\n"
                     "It opens .omni kits, SFZ, DecentSampler (.dspreset, .dslibrary), SoundFont (.sf2), Logic EXS (.exs), Ableton (.adv), Kontakt (.nki) and .zip packs.");
        return;
    }
    auto msg = proc.loadInstrumentFile (f);
   #if JUCE_IOS
    if (msg.isNotEmpty() && ! f.hasFileExtension ("dslibrary;sf2;zip"))
        msg << "\n\nAn .sfz or .dspreset needs its sample files next to it. Use Load > Folder and pick the whole instrument folder.";
   #endif
    setView (true);
    selectKeyZone (0);
    if (msg.isNotEmpty()) showMessage ("Note", msg);
}

#if JUCE_IOS
static File kitsDir()
{
    auto d = File::getSpecialLocation (File::userDocumentsDirectory).getChildFile ("Kits");
    d.createDirectory();
    return d;
}

// Menu: saved kits, then a file or a folder from the Files app.
void OmniEditor::showLoadMenu (Component& target, bool withKits)
{
    PopupMenu m;
    Array<File> kits;
    if (withKits)
    {
        kits = kitsDir().findChildFiles (File::findFiles, false, "*.omni");
        kits.sort();
        for (int i = 0; i < kits.size(); ++i) m.addItem (100 + i, kits[i].getFileNameWithoutExtension());
        if (! kits.isEmpty()) m.addSeparator();
    }
    m.addItem (1, withKits ? "File (.omni kit, .sf2, .dslibrary, .zip...)..." : "File (.sf2, .dslibrary, .zip...)...");
    m.addItem (2, "Folder (SFZ or DecentSampler with its samples)...");
    m.showMenuAsync (PopupMenu::Options().withTargetComponent (&target),
                     [safe = Component::SafePointer<OmniEditor> (this), kits] (int r)
    {
        if (safe == nullptr || r == 0) return;
        if (r >= 100) { safe->instrumentChosen (kits[r - 100]); return; }
        omni::ios::pickAndImport (*safe, r == 2 ? omni::ios::Pick::folder : omni::ios::Pick::files, false,
                                  [safe] (Array<File> files) { if (safe != nullptr && ! files.isEmpty()) safe->instrumentChosen (files[0]); });
    });
}

// Asks for a name, then saves into the app's Documents folder (it shows up in the Files app).
void OmniEditor::askNameThen (const String& title, const String& initial, std::function<void (String)> then)
{
    auto* w = new AlertWindow (title, "Saved in the Files app under On My iPhone > Omnisampler.", MessageBoxIconType::NoIcon, this);
    w->addTextEditor ("name", initial);
    w->addButton ("Save", 1, KeyPress (KeyPress::returnKey));
    w->addButton ("Cancel", 0, KeyPress (KeyPress::escapeKey));
    w->enterModalState (true, ModalCallbackFunction::create ([w, then] (int r)
    {
        const auto name = File::createLegalFileName (w->getTextEditorContents ("name").trim());
        if (r == 1 && name.isNotEmpty()) then (name);
    }), true);
}
#endif

void OmniEditor::browsePadSamples (int pad)
{
   #if JUCE_IOS
    omni::ios::pickAndImport (*this, omni::ios::Pick::files, true,
                              [safe = Component::SafePointer<OmniEditor> (this), pad] (Array<File> files) { if (safe != nullptr) safe->padFilesChosen (pad, files); });
   #else
    chooser = std::make_unique<FileChooser> ("Add samples to pad", File::getSpecialLocation (File::userMusicDirectory), "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
    chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles | FileBrowserComponent::canSelectMultipleItems,
                          [this, pad] (const FileChooser& fc) { if (! fc.getResults().isEmpty()) padFilesChosen (pad, fc.getResults()); });
   #endif
}

void OmniEditor::browseKeySamples()
{
    auto loaded = [] (OmniEditor& ed, const Array<File>& files)
    {
        const auto msg = ed.proc.loadFilesToKeys (files, false);
        ed.selectKeyZone (0);
        if (msg.isNotEmpty()) ed.showMessage ("Note", msg);
    };
   #if JUCE_IOS
    omni::ios::pickAndImport (*this, omni::ios::Pick::files, true,
                              [safe = Component::SafePointer<OmniEditor> (this), loaded] (Array<File> files) { if (safe != nullptr && ! files.isEmpty()) loaded (*safe, files); });
   #else
    chooser = std::make_unique<FileChooser> ("Add samples to the keys", File::getSpecialLocation (File::userMusicDirectory), "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
    chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles | FileBrowserComponent::canSelectMultipleItems,
                          [this, loaded] (const FileChooser& fc) { if (! fc.getResults().isEmpty()) loaded (*this, fc.getResults()); });
   #endif
}

void OmniEditor::browseInstrument()
{
   #if JUCE_IOS
    showLoadMenu (loadInstBtn, false);
   #else
    chooser = std::make_unique<FileChooser> ("Load instrument", File::getSpecialLocation (File::userDocumentsDirectory), OmniProcessor::instrumentPatterns());
    chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
                          [this] (const FileChooser& fc) { if (fc.getResult().existsAsFile()) instrumentChosen (fc.getResult()); });
   #endif
}

void OmniEditor::browseKit()
{
   #if JUCE_IOS
    showLoadMenu (loadKitBtn, true);
   #else
    chooser = std::make_unique<FileChooser> ("Load kit, SFZ or DecentSampler", File::getSpecialLocation (File::userDocumentsDirectory).getChildFile ("Omnisampler"),
                                             "*.omni;" + OmniProcessor::instrumentPatterns());
    chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
                          [this] (const FileChooser& fc) { if (fc.getResult().existsAsFile()) instrumentChosen (fc.getResult()); });
   #endif
}

void OmniEditor::saveKitAs()
{
   #if JUCE_IOS
    askNameThen ("Save kit", "My kit", [safe = Component::SafePointer<OmniEditor> (this)] (String name)
    {
        if (safe == nullptr) return;
        const auto msg = safe->proc.saveKit (kitsDir().getChildFile (name + ".omni"));
        if (msg.isNotEmpty()) safe->showMessage ("Couldn't save", msg);
    });
   #else
    auto dir = File::getSpecialLocation (File::userDocumentsDirectory).getChildFile ("Omnisampler");
    dir.createDirectory();
    chooser = std::make_unique<FileChooser> ("Save kit", dir.getChildFile ("My kit.omni"), "*.omni");
    chooser->launchAsync (FileBrowserComponent::saveMode | FileBrowserComponent::warnAboutOverwriting, [this] (const FileChooser& fc)
    {
        auto f = fc.getResult();
        if (f == File()) return;
        const auto msg = proc.saveKit (f.withFileExtension ("omni"));
        if (msg.isNotEmpty()) showMessage ("Couldn't save", msg);
    });
   #endif
}

void OmniEditor::saveSfzAs()
{
   #if JUCE_IOS
    const auto initial = shownInst->keys.name.isNotEmpty() ? shownInst->keys.name : String ("instrument");
    askNameThen ("Save keys as SFZ", initial, [safe = Component::SafePointer<OmniEditor> (this)] (String name)
    {
        if (safe == nullptr) return;
        String err;
        const auto f = File::getSpecialLocation (File::userDocumentsDirectory).getChildFile (name + ".sfz");
        if (! saveSfz (safe->proc.getInstrument()->keys, f, err)) safe->showMessage ("Couldn't save", err);
    });
   #else
    chooser = std::make_unique<FileChooser> ("Save keys as SFZ", File::getSpecialLocation (File::userDocumentsDirectory).getChildFile ("Omnisampler/instrument.sfz"), "*.sfz");
    chooser->launchAsync (FileBrowserComponent::saveMode | FileBrowserComponent::warnAboutOverwriting, [this] (const FileChooser& fc)
    {
        auto f = fc.getResult();
        if (f == File()) return;
        String err;
        if (! saveSfz (proc.getInstrument()->keys, f.withFileExtension ("sfz"), err)) showMessage ("Couldn't save", err);
    });
   #endif
}

void OmniEditor::saveDecentSamplerAs()
{
   #if JUCE_IOS
    const auto initial = shownInst->keys.name.isNotEmpty() ? shownInst->keys.name : String ("instrument");
    askNameThen ("Save keys as DecentSampler", initial, [safe = Component::SafePointer<OmniEditor> (this)] (String name)
    {
        if (safe == nullptr) return;
        const auto err = safe->proc.saveKeysAsDecentSampler (File::getSpecialLocation (File::userDocumentsDirectory).getChildFile (name + ".dspreset"));
        if (err.isNotEmpty()) safe->showMessage ("Couldn't save", err);
    });
   #else
    chooser = std::make_unique<FileChooser> ("Save keys as DecentSampler", File::getSpecialLocation (File::userDocumentsDirectory).getChildFile ("Omnisampler/instrument.dspreset"), "*.dspreset");
    chooser->launchAsync (FileBrowserComponent::saveMode | FileBrowserComponent::warnAboutOverwriting, [this] (const FileChooser& fc)
    {
        auto f = fc.getResult();
        if (f == File()) return;
        const auto err = proc.saveKeysAsDecentSampler (f.withFileExtension ("dspreset"));
        if (err.isNotEmpty()) showMessage ("Couldn't save", err);
    });
   #endif
}

void OmniEditor::showSaveMenu()
{
    PopupMenu m;
    m.addItem (1, "Save as SFZ");
    m.addItem (2, "Save as DecentSampler");
    m.showMenuAsync (PopupMenu::Options().withTargetComponent (&saveSfzBtn), [safe = Component::SafePointer<OmniEditor> (this)] (int r)
    {
        if (safe == nullptr) return;
        if (r == 1) safe->saveSfzAs();
        else if (r == 2) safe->saveDecentSamplerAs();
    });
}

void OmniEditor::applyTouchVelocity()
{
    // JUCE's keyboard: with mouse-position velocity on, a tap nearer the front (bottom) of a key is louder.
    keyboard.setVelocity (1.0f, proc.touchVelocity.load());
}

void OmniEditor::showOptionsMenu()
{
    PopupMenu m;
    m.addItem (3, "Velocity-sensitive on-screen pads and keys", true, proc.touchVelocity.load());
    m.addItem (4, "(pads: louder towards the top; keys: louder towards the bottom)", false, false);
    m.addSeparator();
    m.addItem (1, "Stream long samples from disk", true, proc.isStreaming());
    m.addItem (2, "(applies to samples loaded afterwards)", false, false);
    m.showMenuAsync (PopupMenu::Options().withTargetComponent (&optionsBtn), [safe = Component::SafePointer<OmniEditor> (this)] (int r)
    {
        if (safe == nullptr) return;
        if (r == 1) safe->proc.setStreaming (! safe->proc.isStreaming());
        if (r == 3) { safe->proc.touchVelocity = ! safe->proc.touchVelocity.load(); safe->applyTouchVelocity(); }
    });
}

// Popup of drum models; picking one adds that synth layer to the selected pad (works on an empty pad too).
void OmniEditor::addSynthLayerMenu (Component& target)
{
    PopupMenu m;
    for (int i = 0; i < (int) DrumModel::NumModels; ++i) m.addItem (i + 1, drumModelName ((DrumModel) i));
    m.showMenuAsync (PopupMenu::Options().withTargetComponent (&target), [safe = Component::SafePointer<OmniEditor> (this), pad = selectedPad] (int r)
    {
        if (safe == nullptr || r <= 0) return;
        const auto model = (DrumModel) (r - 1);
        int added = -1;
        safe->proc.edit ([&] (Instrument& in)
        {
            auto& p = in.pads[pad];
            if ((int) p.synths.size() >= kMaxSynthLayers) return;
            if (p.isEmpty() && p.name.isEmpty()) p.name = drumModelName (model);
            p.synths.push_back (defaultSynthLayer (model));
            added = (int) p.synths.size() - 1;
        });
        if (safe->selectedPad == pad && added >= 0) safe->layersPanel.showLayer (added);
        safe->refreshAll();
    });
}

void OmniEditor::selectPad (int pad)
{
    selectedPad = jlimit (0, kPads - 1, pad);
    bank = selectedPad / kPadsPerBank;
    for (int b = 0; b < 4; ++b) bankBtn[b].setToggleState (b == bank, dontSendNotification);
    selectedPadZone = 0;
    layersPanel.showLayer (0);
    refreshAll();
}

void OmniEditor::selectKeyZone (int z)
{
    selectedKeyZone = z;
    refreshAll();
}

const std::vector<Zone>& OmniEditor::currentZones() const
{
    return showingKeys ? shownInst->keys.zones : shownInst->pads[selectedPad].zones;
}

void OmniEditor::refreshAll()
{
    shownInst = proc.getInstrument();
    const auto& p = shownInst->pads[selectedPad];
    padTitle.setText (p.name.isNotEmpty() ? p.name : "Pad " + String::charToString ((juce_wchar) "ABCD"[bank]) + String (selectedPad % kPadsPerBank + 1), dontSendNotification);
    padNote.setSelectedId (p.note + 1, dontSendNotification);
    padOutput.setSelectedId (p.output + 1, dontSendNotification);
    keysOutput.setSelectedId (shownInst->keys.output + 1, dontSendNotification);
    layersPanel.refresh();
    chokeBox.setSelectedId (p.chokeGroup + 1, dontSendNotification);
    oneShot.setToggleState (p.oneShot, dontSendNotification);
    keysTitle.setText (shownInst->keys.name.isNotEmpty() ? shownInst->keys.name + "  (" + String ((int) shownInst->keys.zones.size()) + " zones)" : String ("Keys"),
                       dontSendNotification);
    const auto& zs = currentZones();
    int& sel = showingKeys ? selectedKeyZone : selectedPadZone;
    sel = zs.empty() ? -1 : jlimit (0, (int) zs.size() - 1, sel);
    zoneList.updateContent();
    if (sel >= 0) zoneList.selectRow (sel, false, true); else zoneList.deselectAllRows();
    padZoneEditor.refresh();
    keyZoneEditor.refresh();
    padVoicing.refresh();
    keyVoicing.refresh();
    removeZoneBtn.setEnabled (sel >= 0);
    for (auto& pb : padButtons) pb->repaint();
    zoneMap.repaint();
    if (samplingPanel.isVisible()) samplingPanel.refresh();
}

int OmniEditor::getNumRows() { return (int) currentZones().size(); }

void OmniEditor::paintListBoxItem (int row, Graphics& g, int w, int h, bool selected)
{
    const auto& zs = currentZones();
    if (row < 0 || row >= (int) zs.size()) return;
    const auto& z = zs[(size_t) row];
    if (selected) g.fillAll (lnf.accent.withAlpha (0.3f));
    else if (row % 2) g.fillAll (Colours::white.withAlpha (0.02f));
    g.setFont (FontOptions (12.5f));
    g.setColour (look::text);
    g.drawText (z.sample ? z.sample->file.getFileNameWithoutExtension() : String ("?"), 6, 0, w - 200, h, Justification::centredLeft, true);
    g.setColour (look::textDim);
    String info = "vel " + String (z.loVel) + "-" + String (z.hiVel);
    if (showingKeys) info = noteName (z.loKey) + "-" + noteName (z.hiKey) + " root " + noteName (z.root) + "  " + info;
    if (z.rrPos > 0) info << "  RR " << z.rrPos;
    g.drawText (info, w - 200, 0, 194, h, Justification::centredRight);
}

void OmniEditor::selectedRowsChanged (int row)
{
    if (row < 0) return;
    int& sel = showingKeys ? selectedKeyZone : selectedPadZone;
    if (sel == row) return;
    sel = row;
    padZoneEditor.refresh();
    keyZoneEditor.refresh();
    zoneMap.repaint();
}

void OmniEditor::deleteKeyPressed (int row)
{
    if (row < 0) return;
    const bool keys = showingKeys;
    const int pad = selectedPad;
    proc.edit ([&] (Instrument& in)
    {
        auto& zs = keys ? in.keys.zones : in.pads[pad].zones;
        if (row < (int) zs.size()) zs.erase (zs.begin() + row);
    });
    refreshAll();
}

void OmniEditor::timerCallback()
{
    auto inst = proc.getInstrument();
    for (auto& pb : padButtons)
    {
        const auto hits = proc.engine.padHits[pb->padIndex()].load();
        if (hits != pb->lastHits) { pb->lastHits = hits; pb->flash = 1.0f; }
        if (pb->flash > 0.0f) { pb->flash = jmax (0.0f, pb->flash - 0.12f); pb->repaint(); }
    }
    voices.setText (String (proc.voiceCount.load()) + (isCompact() ? " v" : " voices"), dontSendNotification);
    // A recording or an Auto Sampler run has landed on a pad or the keys.
    if (const int serial = proc.samplingSerial.load(); serial != lastSamplingSerial)
    {
        lastSamplingSerial = serial;
        refreshAll();
        samplingPanel.news();
    }
   #if JUCE_IOS
    // The notch changes sides when the phone is turned over; lay out again when the margins change.
    if (proc.wrapperType == AudioProcessor::wrapperType_Standalone)
        if (auto* d = Desktop::getInstance().getDisplays().getPrimaryDisplay())
            if (d->safeAreaInsets != lastSafeInsets) resized();
   #endif
}

void OmniEditor::paint (Graphics& g) { g.fillAll (look::bg); }

void OmniEditor::resized()
{
    auto r = getLocalBounds().reduced (10);
    const bool c = isCompact();
    if (c != wasCompact) { wasCompact = c; applyVisibility(); if (c) keyboard.setLowestVisibleKey (48); }
   #if JUCE_IOS
    // The standalone app fills the screen, so keep clear of the notch and the home bar.
    if (proc.wrapperType == AudioProcessor::wrapperType_Standalone)
        if (auto* d = Desktop::getInstance().getDisplays().getPrimaryDisplay())
        {
            lastSafeInsets = d->safeAreaInsets;
            r = lastSafeInsets.subtractedFrom (getLocalBounds()).reduced (6);
        }
   #endif
    if (c)
    {
        auto top = r.removeFromTop (40);
        masterKnob.setVisible (r.getWidth() >= 420);
        // The MIDI channel pickers sit in the top bar when they fit (phone on its side, iPad), dropping
        // the logo first if that makes the room; on a narrow screen they get a row of their own.
        const int chNeed = 324;   // two MIDI channel pickers and the voice count
        const int fixedW = 64 + 4 + 64 + (sampleTab.isVisible() ? 68 : 0) + 76 + 4 + 76 + 4 + 34 + (masterKnob.isVisible() ? 54 : 0) + 16;
        const bool chInline = r.getWidth() - fixedW >= chNeed;
        logo.setVisible (r.getWidth() >= 520 && (! chInline || r.getWidth() - fixedW - 132 >= chNeed));
        if (logo.isVisible()) logo.setBounds (top.removeFromLeft (132));
        padsTab.setBounds (top.removeFromLeft (64).reduced (0, 3)); top.removeFromLeft (4);
        keysTab.setBounds (top.removeFromLeft (64).reduced (0, 3));
        if (sampleTab.isVisible()) { top.removeFromLeft (4); sampleTab.setBounds (top.removeFromLeft (64).reduced (0, 3)); }
        if (masterKnob.isVisible()) { masterKnob.setBounds (top.removeFromRight (48)); top.removeFromRight (6); }
        optionsBtn.setBounds (top.removeFromRight (34).reduced (0, 3)); top.removeFromRight (4);
        saveKitBtn.setBounds (top.removeFromRight (76).reduced (0, 3)); top.removeFromRight (4);
        loadKitBtn.setBounds (top.removeFromRight (76).reduced (0, 3));
        top.removeFromRight (8); top.removeFromLeft (8);
        auto chRow = top;
        if (! chInline) { r.removeFromTop (4); chRow = r.removeFromTop (32); }
        chRow = chRow.withSizeKeepingCentre (chRow.getWidth(), 32);
        voices.setJustificationType (Justification::centredRight);
        voices.setBounds (chRow.removeFromRight (jmin (44, chRow.getWidth() / 6)));
        auto padsHalf = chRow.removeFromLeft (chRow.getWidth() / 2), keysHalf = chRow;
        keysHalf.removeFromLeft (6);
        padsChLabel.setBounds (padsHalf.removeFromLeft (58)); padsHalf.removeFromLeft (4); padsCh.setBounds (padsHalf.removeFromLeft (jmin (88, padsHalf.getWidth())).reduced (0, 1));
        keysChLabel.setBounds (keysHalf.removeFromLeft (58)); keysHalf.removeFromLeft (4); keysCh.setBounds (keysHalf.removeFromLeft (jmin (88, keysHalf.getWidth())).reduced (0, 1));
        r.removeFromTop (6);
        if (showingSample) layoutSample (r);
        else if (showingKeys) layoutKeysCompact (r); else layoutPadsCompact (r);
        return;
    }
    masterKnob.setVisible (true);
    auto top = r.removeFromTop (48);
    // The logo gives way first when the window is narrow.
    const int logoW = proc.fx ? 196 : 160;
    logo.setVisible (r.getWidth() >= logoW + 940);
    if (logo.isVisible()) logo.setBounds (top.removeFromLeft (logoW));
    padsTab.setBounds (top.removeFromLeft (76).reduced (0, 9)); top.removeFromLeft (4);
    keysTab.setBounds (top.removeFromLeft (76).reduced (0, 9));
    if (sampleTab.isVisible()) { top.removeFromLeft (4); sampleTab.setBounds (top.removeFromLeft (76).reduced (0, 9)); }
    top.removeFromLeft (14);
    voices.setBounds (top.removeFromLeft (72));
    voices.setJustificationType (Justification::centredLeft);
    masterKnob.setBounds (top.removeFromRight (60)); top.removeFromRight (12);
    optionsBtn.setBounds (top.removeFromRight (34).reduced (0, 9)); top.removeFromRight (4);
    saveKitBtn.setBounds (top.removeFromRight (80).reduced (0, 9)); top.removeFromRight (4);
    loadKitBtn.setBounds (top.removeFromRight (80).reduced (0, 9)); top.removeFromRight (12);
    keysCh.setBounds (top.removeFromRight (80).reduced (0, 10)); keysChLabel.setBounds (top.removeFromRight (70)); top.removeFromRight (6);
    padsCh.setBounds (top.removeFromRight (80).reduced (0, 10)); padsChLabel.setBounds (top.removeFromRight (70));
    r.removeFromTop (6);
    if (showingSample) layoutSample (r);
    else if (showingKeys) layoutKeys (r); else layoutPads (r);
}

// The Sample page: the sampling panel with the keyboard under it.
void OmniEditor::layoutSample (Rectangle<int> r)
{
    if (r.getHeight() > 420)
    {
        keyboard.setBounds (r.removeFromBottom (jmin (70, r.getHeight() / 8)));
        keyboard.setKeyWidth (jmax (8.0f, (float) keyboard.getWidth() / 52.0f));
        r.removeFromBottom (6);
    }
    else keyboard.setBounds ({});
    samplingPanel.setBounds (r);
}

// Small screens. Wide (phone on its side): pads or key map on the left, one detail panel on the
// right. Tall (iPad upright, narrow AUv3 window): the same two blocks stacked.
void OmniEditor::layoutPadsCompact (Rectangle<int> r)
{
    const bool wide = r.getWidth() > r.getHeight() * 5 / 4;
    Rectangle<int> padArea;
    if (wide)
    {
        padArea = r.removeFromLeft (jmin (r.getHeight() + 44, r.getWidth() / 2));
        r.removeFromLeft (10);
        auto banks = padArea.removeFromLeft (40);
        padArea.removeFromLeft (4);
        const int bh = jmin (44, banks.getHeight() / 4);
        for (auto& b : bankBtn) b.setBounds (banks.removeFromTop (bh).reduced (0, 2));
    }
    else
    {
        padArea = r.removeFromTop (jmin (r.getWidth(), r.getHeight() * 9 / 20) + 38);
        r.removeFromTop (8);
        auto banks = padArea.removeFromTop (34);
        padArea.removeFromTop (4);
        for (auto& b : bankBtn) { b.setBounds (banks.removeFromLeft (56)); banks.removeFromLeft (4); }
    }
    const int side = jmin (padArea.getWidth(), padArea.getHeight()) / 4;
    auto grid = padArea.withSizeKeepingCentre (side * 4, side * 4).withY (padArea.getY());
    for (int i = 0; i < kPadsPerBank; ++i)
    {
        const int col = i % 4, row = 3 - i / 4;
        padButtons[i]->setBounds (grid.getX() + col * side, grid.getY() + row * side, side, side);
    }

    auto head = r.removeFromTop (34);
    addToPadBtn.setBounds (head.removeFromRight (jmin (128, head.getWidth() / 3)));
    head.removeFromRight (4);
    addSynthBtn.setBounds (head.removeFromRight (jmin (84, head.getWidth() / 4)));
    head.removeFromRight (6);
    padTitle.setBounds (head);
    r.removeFromTop (4);
    auto tabs = r.removeFromTop (32);
    const int tw = tabs.getWidth() / 4;
    for (auto& b : detailBtn) b.setBounds (tabs.removeFromLeft (tw).reduced (1, 0));
    r.removeFromTop (6);

    if (detail == 0)
    {
        auto row = r.removeFromTop (30);
        padNoteLabel.setBounds (row.removeFromLeft (62)); padNote.setBounds (row.removeFromLeft (jmin (90, row.getWidth() / 4))); row.removeFromLeft (8);
        chokeLabel.setBounds (row.removeFromLeft (44)); chokeBox.setBounds (row.removeFromLeft (jmin (84, row.getWidth() / 3))); row.removeFromLeft (8);
        oneShot.setBounds (row);
        r.removeFromTop (4);
        auto btns = r.removeFromBottom (32);
        const int bw = btns.getWidth() / 4;
        removeZoneBtn.setBounds (btns.removeFromLeft (bw).reduced (1, 0));
        clearPadBtn.setBounds (btns.removeFromLeft (bw).reduced (1, 0));
        remapBtn.setBounds (btns.reduced (1, 0));
        r.removeFromBottom (4);
        auto drop = r.removeFromBottom (30);
        dropLabel.setBounds (drop.removeFromLeft (140));
        dropModeBox.setBounds (drop);
        r.removeFromBottom (4);
        zoneList.setBounds (r);
    }
    else if (detail == 1) padZoneEditor.setBounds (r);
    else if (detail == 2)
    {
        if (padOutput.isVisible())
        {
            auto out = r.removeFromTop (30);
            padOutputLabel.setBounds (out.removeFromLeft (56)); padOutput.setBounds (out.removeFromLeft (jmin (110, out.getWidth())));
            r.removeFromTop (4);
        }
        padVoicing.setBounds (r.withHeight (jmin (r.getHeight(), 260)));
    }
    else                  layersPanel.setBounds (r);
}

void OmniEditor::layoutKeysCompact (Rectangle<int> r)
{
    auto head = r.removeFromTop (34);
    const int bw = jmin (130, head.getWidth() / 5);
    clearKeysBtn.setBounds (head.removeFromRight (bw).reduced (1, 0));
    saveSfzBtn.setBounds (head.removeFromRight (bw).reduced (1, 0));
    addKeysBtn.setBounds (head.removeFromRight (bw).reduced (1, 0));
    saveDsBtn.setBounds ({});
    loadInstBtn.setBounds (head.removeFromRight (bw + 20).reduced (1, 0));
    head.removeFromRight (6);
    keysTitle.setBounds (head);
    r.removeFromTop (6);

    const bool wide = r.getWidth() > r.getHeight() * 5 / 4;
    auto mapArea = wide ? r.removeFromLeft (r.getWidth() / 2) : r.removeFromTop (r.getHeight() * 2 / 5);
    if (wide) r.removeFromLeft (10); else r.removeFromTop (8);
    keyboard.setBounds (mapArea.removeFromBottom (jmin (84, mapArea.getHeight() / 3)));
    keyboard.setKeyWidth (jmax (14.0f, keyboard.getWidth() / 22.0f));   // about three octaves, wide enough for fingers; swipe to scroll
    mapArea.removeFromBottom (4);
    zoneMap.setBounds (mapArea);

    auto tabs = r.removeFromTop (32);
    const int tw = tabs.getWidth() / 3;
    for (auto& b : detailBtn) b.setBounds (tabs.removeFromLeft (tw).reduced (1, 0));
    r.removeFromTop (6);
    if (detail == 0)
    {
        removeZoneBtn.setBounds (r.removeFromBottom (32).removeFromLeft (110));
        r.removeFromBottom (4);
        zoneList.setBounds (r);
    }
    else if (detail == 1) keyZoneEditor.setBounds (r);
    else
    {
        if (keysOutput.isVisible())
        {
            auto out = r.removeFromTop (30);
            keysOutputLabel.setBounds (out.removeFromLeft (56)); keysOutput.setBounds (out.removeFromLeft (jmin (110, out.getWidth())));
            r.removeFromTop (4);
        }
        keyVoicing.setBounds (r.withHeight (jmin (r.getHeight(), 260)));
    }
}

void OmniEditor::layoutPads (Rectangle<int> r)
{
    auto left = r.removeFromLeft (jmin (520, r.getWidth() / 2));
    auto banks = left.removeFromTop (30);
    for (auto& b : bankBtn) { b.setBounds (banks.removeFromLeft (44)); banks.removeFromLeft (4); }
    left.removeFromTop (6);
    auto drop = left.removeFromBottom (26);
    dropLabel.setBounds (drop.removeFromLeft (140));
    dropModeBox.setBounds (drop);
    left.removeFromBottom (6);
    const int side = jmin (left.getWidth(), left.getHeight()) / 4;
    auto grid = left.withSizeKeepingCentre (side * 4, side * 4).withY (left.getY());
    for (int i = 0; i < kPadsPerBank; ++i)
    {
        // Row 1 at the bottom, like hardware pads.
        const int col = i % 4, row = 3 - i / 4;
        padButtons[i]->setBounds (grid.getX() + col * side, grid.getY() + row * side, side, side);
    }

    r.removeFromLeft (12);
    auto head = r.removeFromTop (28);
    padTitle.setBounds (head.removeFromLeft (head.getWidth() / 2));
    oneShot.setBounds (head.removeFromRight (90));
    auto row2 = r.removeFromTop (26);
    r.removeFromTop (6);
    padNoteLabel.setBounds (row2.removeFromLeft (64)); padNote.setBounds (row2.removeFromLeft (110)); row2.removeFromLeft (8);
    chokeLabel.setBounds (row2.removeFromLeft (44)); chokeBox.setBounds (row2.removeFromLeft (90)); row2.removeFromLeft (8);
    padOutputLabel.setBounds (row2.removeFromLeft (46)); padOutput.setBounds (row2.removeFromLeft (jmin (84, row2.getWidth())));
    auto voicing = r.removeFromBottom (jmin (262, r.getHeight() / 2));
    auto bottomTabs = voicing.removeFromTop (24);
    voicing.removeFromTop (2);
    padBottomTab[0].setBounds (bottomTabs.removeFromLeft (90)); bottomTabs.removeFromLeft (4);
    padBottomTab[1].setBounds (bottomTabs.removeFromLeft (90));
    padVoicing.setBounds (voicing);
    layersPanel.setBounds (voicing);
    r.removeFromBottom (6);
    auto listCol = r.removeFromLeft (r.getWidth() / 2);
    auto btns = listCol.removeFromBottom (26);
    addToPadBtn.setBounds (btns.removeFromLeft (90)); btns.removeFromLeft (4);
    removeZoneBtn.setBounds (btns.removeFromLeft (70)); btns.removeFromLeft (4);
    clearPadBtn.setBounds (btns);
    listCol.removeFromBottom (4);
    auto remap = listCol.removeFromBottom (26);
    remapBtn.setBounds (remap);
    listCol.removeFromBottom (4);
    zoneList.setBounds (listCol);
    r.removeFromLeft (10);
    padZoneEditor.setBounds (r);
}

void OmniEditor::layoutKeys (Rectangle<int> r)
{
    auto head = r.removeFromTop (28);
    keysTitle.setBounds (head.removeFromLeft (head.getWidth() / 4));
    clearKeysBtn.setBounds (head.removeFromRight (90)); head.removeFromRight (4);
    saveDsBtn.setBounds (head.removeFromRight (176)); head.removeFromRight (4);
    saveSfzBtn.setBounds (head.removeFromRight (110)); head.removeFromRight (4);
    addKeysBtn.setBounds (head.removeFromRight (110)); head.removeFromRight (4);
    loadInstBtn.setBounds (head.removeFromRight (150));
    r.removeFromTop (6);
    auto map = r.removeFromTop (jmax (160, r.getHeight() / 3));
    zoneMap.setBounds (map);
    r.removeFromTop (4);
    keyboard.setBounds (r.removeFromTop (70));
    keyboard.setKeyWidth (jmax (8.0f, keyboard.getWidth() / 52.0f));
    r.removeFromTop (8);
    auto bottom = r;
    auto voicingCol = bottom.removeFromRight (bottom.getWidth() * 5 / 11);
    if (keysOutput.isVisible())
    {
        auto outRow = voicingCol.removeFromTop (26);
        voicingCol.removeFromTop (4);
        keysOutputLabel.setBounds (outRow.removeFromLeft (56)); keysOutput.setBounds (outRow.removeFromLeft (110));
    }
    keyVoicing.setBounds (voicingCol);
    bottom.removeFromRight (10);
    auto listCol = bottom.removeFromLeft (bottom.getWidth() / 2);
    removeZoneBtn.setBounds (listCol.removeFromBottom (26).removeFromLeft (90));
    listCol.removeFromBottom (4);
    zoneList.setBounds (listCol);
    bottom.removeFromLeft (10);
    keyZoneEditor.setBounds (bottom);
}
