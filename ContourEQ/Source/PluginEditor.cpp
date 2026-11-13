#include "PluginEditor.h"

using namespace contour;

static constexpr double kMinF = 10.0, kMaxF = 30000.0;

static juce::String freqText (double f)
{
    return f >= 1000.0 ? juce::String (f / 1000.0, f >= 10000.0 ? 1 : 2) + " kHz" : (f < 100 ? juce::String (f, 1) : juce::String (juce::roundToInt (f))) + " Hz";
}

static juce::String noteText (double f)
{
    static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const double midi = 69.0 + 12.0 * std::log2 (f / 440.0);
    const int m = (int) std::round (midi);
    const int cents = (int) std::round ((midi - m) * 100.0);
    return juce::String (names[((m % 12) + 12) % 12]) + juce::String (m / 12 - 1) + (cents >= 0 ? " +" : " ") + juce::String (cents) + "c";
}

// =============================================================================================
EqDisplay::EqDisplay (ContourProcessor& p, ContourEditor& e) : proc (p), ed (e)
{
    setWantsKeyboardFocus (false);
}

float EqDisplay::freqToX (double f) const
{
    return plot.getX() + (float) (std::log (f / kMinF) / std::log (kMaxF / kMinF)) * plot.getWidth();
}
double EqDisplay::xToFreq (float x) const
{
    return kMinF * std::pow (kMaxF / kMinF, juce::jlimit (0.0, 1.0, (double) ((x - plot.getX()) / plot.getWidth())));
}
float EqDisplay::dbToY (double db) const { return plot.getCentreY() - (float) (db / rangeDb) * plot.getHeight() * 0.5f; }
double EqDisplay::yToDb (float y) const { return (plot.getCentreY() - y) / (plot.getHeight() * 0.5) * rangeDb; }
float EqDisplay::specDbToY (double db) const
{
    // Analyser: -96 dB at the bottom, +6 dB at the top.
    return plot.getBottom() - (float) juce::jlimit (0.0, 1.0, (db + 96.0) / 102.0) * plot.getHeight();
}

juce::Point<float> EqDisplay::nodePos (int b) const
{
    const auto s = proc.getSpec (b);
    float y = dbToY (0.0);
    if (typeUsesGain (s.type)) y = dbToY (juce::jlimit (-(double) rangeDb, (double) rangeDb, s.gainDb));
    return { freqToX (s.freq), y };
}

int EqDisplay::hitNode (juce::Point<float> p) const
{
    int best = -1; float bestD = 11.0f;
    for (int b = 0; b < kMaxBands; ++b)
        if (proc.isBandOn (b))
        {
            const float d = nodePos (b).getDistanceFrom (p);
            if (d < bestD) { bestD = d; best = b; }
        }
    return best;
}

void EqDisplay::paint (juce::Graphics& g)
{
    plot = getLocalBounds().toFloat().reduced (2.0f).withTrimmedBottom (18.0f);
    const double sr = proc.getSampleRateSafe();

    juce::ColourGradient bgGrad (juce::Colour (0xff101317), 0, plot.getY(), juce::Colour (0xff181c23), 0, plot.getBottom(), false);
    g.setGradientFill (bgGrad);
    g.fillRoundedRectangle (plot, 6.0f);

    // Grid.
    g.setFont (juce::FontOptions (11.0f));
    const double gridF[] = { 20, 30, 40, 50, 60, 80, 100, 200, 300, 400, 500, 600, 800, 1000, 2000, 3000, 4000, 5000, 6000, 8000, 10000, 20000 };
    for (double f : gridF)
    {
        const float x = freqToX (f);
        const bool major = f == 100 || f == 1000 || f == 10000;
        g.setColour (juce::Colours::white.withAlpha (major ? 0.10f : 0.045f));
        g.drawVerticalLine ((int) x, plot.getY(), plot.getBottom());
        if (f == 20 || f == 50 || f == 100 || f == 200 || f == 500 || f == 1000 || f == 2000 || f == 5000 || f == 10000 || f == 20000)
        {
            g.setColour (look::textDim);
            g.drawText (f >= 1000 ? juce::String ((int) (f / 1000)) + "k" : juce::String ((int) f), (int) x - 20, (int) plot.getBottom() - 15, 40, 14, juce::Justification::centred);
        }
    }
    const float step = rangeDb <= 6 ? 1.0f : rangeDb <= 12 ? 3.0f : 6.0f;
    for (float d = -rangeDb; d <= rangeDb + 0.01f; d += step)
    {
        const float y = dbToY (d);
        g.setColour (juce::Colours::white.withAlpha (d == 0 ? 0.16f : 0.05f));
        g.drawHorizontalLine ((int) y, plot.getX(), plot.getRight());
        if (d != -rangeDb && d != rangeDb)
        {
            g.setColour (look::textDim);
            g.drawText ((d > 0 ? "+" : "") + juce::String ((int) d), (int) plot.getRight() - 30, (int) y - 7, 26, 14, juce::Justification::centredRight);
        }
    }

    // Piano-roll ruler: C notes along the bottom strip.
    {
        auto strip = getLocalBounds().toFloat().removeFromBottom (18.0f);
        for (int oct = 0; oct <= 9; ++oct)
        {
            const double f = 440.0 * std::pow (2.0, ((oct + 1) * 12 - 69) / 12.0);
            if (f < kMinF || f > kMaxF) continue;
            const float x = freqToX (f);
            g.setColour (look::textDim.withAlpha (0.7f));
            g.fillRect (x, strip.getY() + 2, 1.0f, 5.0f);
            g.drawText ("C" + juce::String (oct), (int) x - 12, (int) strip.getY() + 5, 24, 12, juce::Justification::centred);
        }
    }

    g.saveState();
    g.reduceClipRegion (plot.toNearestInt());

    // Analyser, tilted +4.5 dB/oct around 1 kHz so pink-ish music reads flat.
    auto spectrumPath = [this] (const Grid& db, bool closed)
    {
        juce::Path p;
        for (int i = 0; i < kGridPoints; ++i)
        {
            const double f = gridFreq (i);
            const float x = freqToX (f);
            const float y = specDbToY (db[(size_t) i] + 4.5 * std::log2 (f / 1000.0));
            if (i == 0) { if (closed) { p.startNewSubPath (x, plot.getBottom()); p.lineTo (x, y); } else p.startNewSubPath (x, y); }
            else p.lineTo (x, y);
        }
        if (closed) { p.lineTo (freqToX (gridFreq (kGridPoints - 1)), plot.getBottom()); p.closeSubPath(); }
        return p;
    };
    if (analyserMode == 1 || analyserMode == 3)
    {
        g.setColour (juce::Colours::white.withAlpha (0.22f));
        g.strokePath (spectrumPath (proc.preTap.getDisplay(), false), juce::PathStrokeType (1.0f));
    }
    if (analyserMode >= 2)
    {
        auto p = spectrumPath (proc.postTap.getDisplay(), true);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0x5535c9b4), 0, plot.getY(), juce::Colour (0x1035c9b4), 0, plot.getBottom(), false));
        g.fillPath (p);
        g.setColour (juce::Colour (0x8835c9b4));
        g.strokePath (spectrumPath (proc.postTap.getDisplay(), false), juce::PathStrokeType (1.0f));
    }
    if (proc.sidechainActive() && analyserMode != 0)
    {
        g.setColour (juce::Colour (0x66ff9f43));
        g.strokePath (spectrumPath (proc.scTap.getDisplay(), false), juce::PathStrokeType (1.0f));
    }

    // Match EQ curves (spectrum scale for captures, EQ scale for the target).
    if (ed.matchVisible())
    {
        if (matchRef) { g.setColour (juce::Colour (0xccff9f43)); g.strokePath (spectrumPath (*matchRef, false), juce::PathStrokeType (1.5f)); }
        if (matchSrc) { g.setColour (juce::Colour (0xcc8fb3ff)); g.strokePath (spectrumPath (*matchSrc, false), juce::PathStrokeType (1.5f)); }
        if (matchTarget)
        {
            juce::Path p;
            for (int i = 0; i < kGridPoints; ++i)
            {
                const float x = freqToX (gridFreq (i)), y = dbToY ((*matchTarget)[(size_t) i]);
                if (i == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
            }
            g.setColour (juce::Colours::yellow.withAlpha (0.7f));
            g.strokePath (p, juce::PathStrokeType (1.2f, juce::PathStrokeType::curved), juce::AffineTransform());
        }
    }

    // Per-band and total curves, with live dynamic gain.
    const int W = (int) plot.getWidth();
    const int stepPx = 2;
    std::vector<double> total ((size_t) (W / stepPx + 2), 0.0);
    const float scale = proc.apvts.getRawParameterValue ("gainscale")->load() / 100.0f;
    for (int b = 0; b < kMaxBands; ++b)
    {
        if (! proc.isBandOn (b)) continue;
        auto spec = proc.getSpec (b);
        spec.gainDb += proc.dynGainDb[b].load() * scale;
        const auto d = designBand (spec, sr);
        juce::Path bp;
        const float y0 = dbToY (0.0);
        bp.startNewSubPath (plot.getX(), y0);
        for (int i = 0, x = 0; x <= W; x += stepPx, ++i)
        {
            const double f = xToFreq (plot.getX() + (float) x);
            const double m = magnitudeDb (d, f, sr);
            total[(size_t) i] += m;
            bp.lineTo (plot.getX() + (float) x, dbToY (juce::jlimit (-rangeDb * 2.0, rangeDb * 2.0, m)));
        }
        bp.lineTo (plot.getRight(), y0);
        bp.closeSubPath();
        const auto col = bandColour (b);
        const bool sel = b == ed.getSelectedBand();
        g.setColour (col.withAlpha (sel ? 0.22f : 0.08f));
        g.fillPath (bp);
        g.setColour (col.withAlpha (sel ? 0.9f : 0.35f));
        g.strokePath (bp, juce::PathStrokeType (sel ? 1.4f : 1.0f));
    }
    {
        juce::Path tp;
        for (int i = 0, x = 0; x <= W; x += stepPx, ++i)
        {
            const float px = plot.getX() + (float) x, py = dbToY (juce::jlimit (-rangeDb * 2.0, rangeDb * 2.0, total[(size_t) i]));
            if (i == 0) tp.startNewSubPath (px, py); else tp.lineTo (px, py);
        }
        g.setColour (juce::Colours::white.withAlpha (0.92f));
        g.strokePath (tp, juce::PathStrokeType (2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // Nodes.
    for (int b = 0; b < kMaxBands; ++b)
    {
        if (! proc.isBandOn (b)) continue;
        const auto p = nodePos (b);
        const auto col = bandColour (b);
        const bool sel = b == ed.getSelectedBand(), hov = b == hoverBand;
        const auto spec = proc.getSpec (b);
        if (proc.apvts.getRawParameterValue (ContourProcessor::pid (b, "dyn"))->load() > 0.5f && typeUsesGain (spec.type))
        {
            const float range = proc.apvts.getRawParameterValue (ContourProcessor::pid (b, "range"))->load() * scale;
            const float live = proc.dynGainDb[b].load() * scale;
            g.setColour (col.withAlpha (0.35f));
            g.fillRoundedRectangle (juce::Rectangle<float> (p.x - 2.5f, juce::jmin (p.y, dbToY (spec.gainDb + range)), 5.0f, std::abs (dbToY (spec.gainDb + range) - p.y)), 2.0f);
            g.setColour (col);
            g.fillEllipse (juce::Rectangle<float> (6, 6).withCentre ({ p.x, dbToY (spec.gainDb + live) }));
        }
        const float r = sel ? 9.0f : hov ? 8.0f : 7.0f;
        g.setColour (col.withAlpha (sel ? 1.0f : 0.85f));
        g.fillEllipse (juce::Rectangle<float> (r * 2, r * 2).withCentre (p));
        g.setColour (juce::Colours::black.withAlpha (0.75f));
        g.setFont (juce::FontOptions (10.5f, juce::Font::bold));
        g.drawText (juce::String (b + 1), juce::Rectangle<float> (r * 2, r * 2).withCentre (p), juce::Justification::centred);
        if (proc.soloBand.load() == b)
        {
            g.setColour (juce::Colours::white);
            g.drawEllipse (juce::Rectangle<float> (r * 2 + 6, r * 2 + 6).withCentre (p), 1.5f);
        }
    }

    // Readouts.
    const int show = dragBand >= 0 ? dragBand : hoverBand;
    if (show >= 0)
    {
        const auto s = proc.getSpec (show);
        const auto p = nodePos (show);
        juce::String t = juce::String (typeName ((int) s.type)) + "  " + freqText (s.freq);
        if (typeUsesGain (s.type)) t << "  " << juce::String (s.gainDb, 1) << " dB";
        if (typeUsesSlope (s.type)) t << "  " << slopeDb (s.slopeIndex) << " dB/oct";
        else t << "  Q " << juce::String (s.q, 2);
        g.setFont (juce::FontOptions (12.0f));
        auto box = juce::Rectangle<float> (0, 0, (float) juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), t) + 14.0f, 20.0f)
                       .withCentre ({ p.x, p.y - 24.0f });
        box = box.constrainedWithin (plot.reduced (4));
        g.setColour (look::panel.withAlpha (0.92f));
        g.fillRoundedRectangle (box, 4.0f);
        g.setColour (bandColour (show));
        g.drawRoundedRectangle (box, 4.0f, 1.0f);
        g.setColour (look::text);
        g.drawText (t, box, juce::Justification::centred);
    }
    else if (! hoverPos.isOrigin() && plot.contains (hoverPos))
    {
        const double f = xToFreq (hoverPos.x);
        g.setColour (look::textDim);
        g.setFont (juce::FontOptions (11.5f));
        g.drawText (freqText (f) + "  " + noteText (f) + "  " + juce::String (yToDb (hoverPos.y), 1) + " dB",
                    plot.reduced (8).removeFromTop (16), juce::Justification::topLeft);
    }

    if (! [&] { for (int b = 0; b < kMaxBands; ++b) if (proc.isBandOn (b)) return true; return false; }())
    {
        g.setColour (look::textDim);
        g.setFont (juce::FontOptions (15.0f));
        g.drawText ("Click anywhere to add a band", plot, juce::Justification::centred);
    }
    g.restoreState();

    g.setColour (look::outline);
    g.drawRoundedRectangle (plot, 6.0f, 1.0f);
}

void EqDisplay::beginGestures (int b)
{
    proc.undo.beginNewTransaction ("Move band");
    for (auto n : { "freq", "gain" })
        if (auto* p = proc.apvts.getParameter (ContourProcessor::pid (b, n))) p->beginChangeGesture();
}
void EqDisplay::endGestures (int b)
{
    for (auto n : { "freq", "gain" })
        if (auto* p = proc.apvts.getParameter (ContourProcessor::pid (b, n))) p->endChangeGesture();
}

void EqDisplay::mouseDown (const juce::MouseEvent& e)
{
    const auto pos = e.position;
    if (! plot.contains (pos)) return;
    int b = hitNode (pos);
    if (b >= 0)
    {
        ed.selectBand (b);
        if (e.mods.isPopupMenu()) { showBandMenu (b); return; }
        if (e.mods.isAltDown()) proc.soloBand.store (b);
    }
    else
    {
        if (e.mods.isPopupMenu()) return;
        b = proc.findFreeBand();
        if (b < 0) return;
        proc.undo.beginNewTransaction ("Add band");
        const double f = xToFreq (pos.x);
        const int type = f < 30.0 ? (int) BandType::LowCut : f > 16000.0 ? (int) BandType::HighCut : (int) BandType::Bell;
        proc.setParam (ContourProcessor::pid (b, "type"), (float) type);
        proc.setParam (ContourProcessor::pid (b, "freq"), (float) f);
        proc.setParam (ContourProcessor::pid (b, "gain"), type == (int) BandType::Bell ? (float) juce::jlimit (-30.0, 30.0, yToDb (pos.y)) : 0.0f);
        proc.setParam (ContourProcessor::pid (b, "q"), 0.707f);
        proc.setParam (ContourProcessor::pid (b, "dyn"), 0.0f);
        proc.setParam (ContourProcessor::pid (b, "on"), 1.0f);
        ed.selectBand (b);
    }
    dragBand = b;
    dragStart = pos;
    dragFine = false;
    const auto s = proc.getSpec (b);
    dragStartFreq = (float) s.freq;
    dragStartGain = proc.apvts.getRawParameterValue (ContourProcessor::pid (b, "gain"))->load();
    beginGestures (b);
}

void EqDisplay::mouseDrag (const juce::MouseEvent& e)
{
    if (dragBand < 0) return;
    const auto s = proc.getSpec (dragBand);
    const float scale = proc.apvts.getRawParameterValue ("gainscale")->load() / 100.0f;
    double f, gdb;
    if (e.mods.isShiftDown())
    {
        // Fine adjust: a tenth of the movement.
        const double octPerPx = std::log2 (kMaxF / kMinF) / plot.getWidth();
        f = dragStartFreq * std::pow (2.0, (e.position.x - dragStart.x) * octPerPx * 0.1);
        gdb = dragStartGain - (e.position.y - dragStart.y) / (plot.getHeight() * 0.5) * rangeDb * 0.1;
    }
    else
    {
        f = xToFreq (e.position.x);
        gdb = std::abs (scale) > 0.01f ? yToDb (e.position.y) / scale : 0.0;
    }
    proc.setParam (ContourProcessor::pid (dragBand, "freq"), (float) juce::jlimit (kMinF, kMaxF, f));
    if (typeUsesGain (s.type))
        proc.setParam (ContourProcessor::pid (dragBand, "gain"), (float) juce::jlimit (-30.0, 30.0, gdb));
    repaint();
}

void EqDisplay::mouseUp (const juce::MouseEvent&)
{
    if (dragBand >= 0) endGestures (dragBand);
    dragBand = -1;
    proc.soloBand.store (-1);
}

void EqDisplay::mouseMove (const juce::MouseEvent& e)
{
    hoverPos = e.position;
    hoverBand = hitNode (e.position);
    setMouseCursor (hoverBand >= 0 ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::CrosshairCursor);
}

void EqDisplay::mouseDoubleClick (const juce::MouseEvent& e)
{
    const int b = hitNode (e.position);
    if (b < 0) return;
    proc.undo.beginNewTransaction ("Remove band");
    proc.setParam (ContourProcessor::pid (b, "on"), 0.0f);
    ed.selectBand (-1);
}

void EqDisplay::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    int b = hitNode (e.position);
    if (b < 0) b = ed.getSelectedBand();
    if (b < 0 || ! proc.isBandOn (b)) return;
    const auto s = proc.getSpec (b);
    if (typeUsesSlope (s.type) && ! e.mods.isCommandDown())
    {
        const int idx = juce::jlimit (0, kNumSlopes - 1, s.slopeIndex + (w.deltaY > 0 ? 1 : -1));
        proc.setParam (ContourProcessor::pid (b, "slope"), (float) idx);
    }
    else
        proc.setParam (ContourProcessor::pid (b, "q"), (float) juce::jlimit (0.025, 40.0, s.q * std::pow (1.25, w.deltaY > 0 ? 1.0 : -1.0)));
    repaint();
}

void EqDisplay::showBandMenu (int b)
{
    juce::PopupMenu m, types, slopes, places;
    const auto s = proc.getSpec (b);
    for (int i = 0; i < kNumTypes; ++i) types.addItem (100 + i, typeName (i), true, (int) s.type == i);
    for (int i = 0; i < kNumSlopes; ++i) slopes.addItem (200 + i, juce::String (slopeDb (i)) + " dB/oct", typeUsesSlope (s.type), s.slopeIndex == i);
    const int place = (int) proc.apvts.getRawParameterValue (ContourProcessor::pid (b, "place"))->load();
    for (int i = 0; i < 5; ++i) places.addItem (300 + i, placementName (i), true, place == i);
    const bool dyn = proc.apvts.getRawParameterValue (ContourProcessor::pid (b, "dyn"))->load() > 0.5f;
    m.addSectionHeader ("Band " + juce::String (b + 1));
    m.addSubMenu ("Shape", types);
    m.addSubMenu ("Slope", slopes, typeUsesSlope (s.type));
    m.addSubMenu ("Placement", places);
    m.addItem (1, "Dynamic", typeUsesGain (s.type), dyn);
    m.addItem (2, "Reset gain");
    m.addSeparator();
    m.addItem (3, "Remove band");
    juce::Component::SafePointer<EqDisplay> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea ({ juce::Desktop::getMousePosition(), juce::Desktop::getMousePosition() })
                                               .withParentComponent (look::menuParent (this)),
        [safe, b, dyn] (int r)
        {
            if (safe == nullptr || r == 0) return;
            auto& p = safe->proc;
            p.undo.beginNewTransaction ("Band menu");
            if (r >= 100 && r < 100 + kNumTypes) p.setParam (ContourProcessor::pid (b, "type"), (float) (r - 100));
            else if (r >= 200 && r < 200 + kNumSlopes) p.setParam (ContourProcessor::pid (b, "slope"), (float) (r - 200));
            else if (r >= 300 && r < 305) p.setParam (ContourProcessor::pid (b, "place"), (float) (r - 300));
            else if (r == 1) p.setParam (ContourProcessor::pid (b, "dyn"), dyn ? 0.0f : 1.0f);
            else if (r == 2) p.setParam (ContourProcessor::pid (b, "gain"), 0.0f);
            else if (r == 3) { p.setParam (ContourProcessor::pid (b, "on"), 0.0f); safe->ed.selectBand (-1); }
            safe->repaint();
        });
}

// =============================================================================================
BandPanel::BandPanel (ContourProcessor& p, ContourEditor& e) : proc (p), ed (e)
{
    title.setFont (juce::FontOptions (16.0f, juce::Font::bold));
    hint.setText ("Click the display to add a band. Drag to move, scroll for Q, right-click for options, "
                  "double-click to remove, Alt-drag to listen to one band, Shift-drag for fine moves.", juce::dontSendNotification);
    hint.setColour (juce::Label::textColourId, look::textDim);
    hint.setJustificationType (juce::Justification::centred);
    for (int i = 0; i < kNumTypes; ++i) typeBox.addItem (typeName (i), i + 1);
    for (int i = 0; i < kNumSlopes; ++i) slopeBox.addItem (juce::String (slopeDb (i)) + " dB/oct", i + 1);
    for (int i = 0; i < 5; ++i) placeBox.addItem (placementName (i), i + 1);
    for (auto* b : { &onBtn, &dynBtn, &scBtn, &soloBtn }) b->setClickingTogglesState (true);
    soloBtn.onClick = [this] { proc.soloBand.store (soloBtn.getToggleState() ? band : -1); };
    delBtn.onClick = [this]
    {
        if (band < 0) return;
        proc.undo.beginNewTransaction ("Remove band");
        proc.setParam (ContourProcessor::pid (band, "on"), 0.0f);
        ed.selectBand (-1);
    };
    typeBox.onChange = [this] { refreshEnablement(); };
    dynBtn.onStateChange = [this] { refreshEnablement(); };
    for (auto* c : std::initializer_list<juce::Component*> { &title, &hint, &onBtn, &dynBtn, &scBtn, &soloBtn, &delBtn, &typeBox, &slopeBox, &placeBox,
                                                             &freq, &gain, &q, &thr, &range, &att, &rel })
        addAndMakeVisible (c);
    for (auto* k : { &freq, &gain, &q, &thr, &range, &att, &rel })
        k->slider.onDragStart = [this] { proc.undo.beginNewTransaction ("Edit band"); };
    setBand (-1);
}

void BandPanel::setBand (int b)
{
    band = b;
    aFreq.reset(); aGain.reset(); aQ.reset(); aThr.reset(); aRange.reset(); aAtt.reset(); aRel.reset();
    aOn.reset(); aDyn.reset(); aSc.reset(); aType.reset(); aSlope.reset(); aPlace.reset();
    const bool has = b >= 0;
    for (auto* c : std::initializer_list<juce::Component*> { &title, &onBtn, &dynBtn, &scBtn, &soloBtn, &delBtn, &typeBox, &slopeBox, &placeBox,
                                                             &freq, &gain, &q, &thr, &range, &att, &rel })
        c->setVisible (has);
    hint.setVisible (! has);
    if (! has) { repaint(); return; }

    auto& s = proc.apvts;
    auto id = [b] (const char* n) { return ContourProcessor::pid (b, n); };
    aFreq = std::make_unique<SA> (s, id ("freq"), freq.slider);
    aGain = std::make_unique<SA> (s, id ("gain"), gain.slider);
    aQ = std::make_unique<SA> (s, id ("q"), q.slider);
    aThr = std::make_unique<SA> (s, id ("thr"), thr.slider);
    aRange = std::make_unique<SA> (s, id ("range"), range.slider);
    aAtt = std::make_unique<SA> (s, id ("att"), att.slider);
    aRel = std::make_unique<SA> (s, id ("rel"), rel.slider);
    aOn = std::make_unique<BA> (s, id ("on"), onBtn);
    aDyn = std::make_unique<BA> (s, id ("dyn"), dynBtn);
    aSc = std::make_unique<BA> (s, id ("sc"), scBtn);
    aType = std::make_unique<CA> (s, id ("type"), typeBox);
    aSlope = std::make_unique<CA> (s, id ("slope"), slopeBox);
    aPlace = std::make_unique<CA> (s, id ("place"), placeBox);
    title.setText ("Band " + juce::String (b + 1), juce::dontSendNotification);
    title.setColour (juce::Label::textColourId, EqDisplay::bandColour (b));
    soloBtn.setToggleState (proc.soloBand.load() == b, juce::dontSendNotification);
    refreshEnablement();
    repaint();
}

void BandPanel::refreshEnablement()
{
    if (band < 0) return;
    const auto t = (BandType) (typeBox.getSelectedId() - 1);
    const bool dyn = dynBtn.getToggleState() && typeUsesGain (t);
    gain.setEnabled (typeUsesGain (t));
    slopeBox.setEnabled (typeUsesSlope (t));
    dynBtn.setEnabled (typeUsesGain (t));
    for (auto* k : { &thr, &range, &att, &rel }) k->setEnabled (dyn);
    scBtn.setEnabled (dyn);
    scBtn.setTooltip (proc.sidechainActive() ? "Detect from the sidechain input" : "Turn on the plug-in's sidechain input in your DAW first");
}

void BandPanel::paint (juce::Graphics& g) { look::drawPanel (g, getLocalBounds().toFloat()); }

void BandPanel::resized()
{
    auto r = getLocalBounds().reduced (10, 8);
    hint.setBounds (r);
    auto left = r.removeFromLeft (170);
    title.setBounds (left.removeFromTop (24));
    left.removeFromTop (4);
    auto row = [&left] { auto x = left.removeFromTop (26); left.removeFromTop (5); return x; };
    typeBox.setBounds (row());
    slopeBox.setBounds (row());
    placeBox.setBounds (row());
    auto btns = left.removeFromTop (26);
    onBtn.setBounds (btns.removeFromLeft (52)); btns.removeFromLeft (6);
    soloBtn.setBounds (btns.removeFromLeft (52)); btns.removeFromLeft (6);
    delBtn.setBounds (btns);

    r.removeFromLeft (14);
    const int kw = juce::jmin (90, r.getWidth() / 8);
    for (auto* k : { &freq, &gain, &q }) k->setBounds (r.removeFromLeft (kw));
    r.removeFromLeft (16);
    auto dynCol = r.removeFromLeft (80);
    dynBtn.setBounds (dynCol.removeFromTop (28).reduced (0, 2));
    dynCol.removeFromTop (6);
    scBtn.setBounds (dynCol.removeFromTop (28).reduced (0, 2));
    r.removeFromLeft (8);
    for (auto* k : { &thr, &range, &att, &rel }) k->setBounds (r.removeFromLeft (kw));
}

// =============================================================================================
MatchPanel::MatchPanel (ContourProcessor& p, ContourEditor& e) : proc (p), ed (e)
{
    title.setText ("Match EQ", juce::dontSendNotification);
    title.setFont (juce::FontOptions (16.0f, juce::Font::bold));
    refSource.addItem ("Reference: sidechain input", 1);
    refSource.addItem ("Reference: audio file", 2);
    refSource.setSelectedId (2, juce::dontSendNotification);
    refSource.onChange = [this] { resized(); };
    smoothBox.addItem ("1/12 oct", 1); smoothBox.addItem ("1/6 oct", 2); smoothBox.addItem ("1/3 oct", 3);
    smoothBox.addItem ("1/2 oct", 4); smoothBox.addItem ("1 oct", 5);
    smoothBox.setSelectedId (3, juce::dontSendNotification);
    smoothBox.onChange = [this] { updateTarget(); };
    bands.setSliderStyle (juce::Slider::LinearHorizontal);
    bands.setRange (1, 24, 1); bands.setValue (12, juce::dontSendNotification);
    bands.setTextBoxStyle (juce::Slider::TextBoxRight, false, 40, 20);
    amount.setSliderStyle (juce::Slider::LinearHorizontal);
    amount.setRange (0, 150, 1); amount.setValue (100, juce::dontSendNotification);
    amount.setTextValueSuffix (" %");
    amount.setTextBoxStyle (juce::Slider::TextBoxRight, false, 50, 20);
    amount.onValueChange = [this] { updateTarget(); };
    bandsLabel.setText ("Bands", juce::dontSendNotification);
    amountLabel.setText ("Amount", juce::dontSendNotification);
    smoothLabel.setText ("Smoothing", juce::dontSendNotification);
    status.setColour (juce::Label::textColourId, look::textDim);
    status.setJustificationType (juce::Justification::topLeft);
    capRefBtn.setClickingTogglesState (true);
    capSrcBtn.setClickingTogglesState (true);

    capRefBtn.onClick = [this]
    {
        if (capRefBtn.getToggleState()) proc.scTap.startCapture();
        else { proc.scTap.stopCapture(); haveRef = proc.scTap.getCapture (ref); updateTarget(); }
    };
    capSrcBtn.onClick = [this]
    {
        if (capSrcBtn.getToggleState()) proc.preTap.startCapture();
        else { proc.preTap.stopCapture(); haveSrc = proc.preTap.getCapture (src); updateTarget(); }
    };
    loadBtn.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> ("Choose a reference track", juce::File::getSpecialLocation (juce::File::userMusicDirectory),
                                                       "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& fc)
        {
            auto f = fc.getResult();
            if (! f.existsAsFile()) return;
            juce::AudioFormatManager fm; fm.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
            if (r == nullptr) { status.setText ("Couldn't read that file.", juce::dontSendNotification); return; }
            const int len = (int) std::min<juce::int64> (r->lengthInSamples, (juce::int64) (r->sampleRate * 240));
            juce::AudioBuffer<float> buf ((int) std::min (2u, r->numChannels), len);
            r->read (&buf, 0, len, 0, true, true);
            haveRef = fileSpectrum (buf, r->sampleRate, ref);
            fileName = f.getFileName();
            updateTarget();
        });
    };
    matchBtn.onClick = [this]
    {
        if (! haveTarget) return;
        proc.applyMatch (fitMatch (target, (int) bands.getValue()));
        ed.selectBand (-1);
    };
    clearBtn.onClick = [this] { haveRef = haveSrc = haveTarget = false; fileName.clear(); updateTarget(); };

    for (auto* c : std::initializer_list<juce::Component*> { &title, &status, &bandsLabel, &amountLabel, &smoothLabel, &refSource, &smoothBox,
                                                             &loadBtn, &capRefBtn, &capSrcBtn, &matchBtn, &clearBtn, &bands, &amount })
        addAndMakeVisible (c);
}

void MatchPanel::updateTarget()
{
    static const double sm[] = { 1.0 / 12, 1.0 / 6, 1.0 / 3, 0.5, 1.0 };
    haveTarget = haveRef && haveSrc;
    if (haveTarget) target = matchTarget (ref, src, amount.getValue() / 100.0, sm[juce::jlimit (0, 4, smoothBox.getSelectedId() - 1)]);
}

void MatchPanel::tick()
{
    const bool useSc = refSource.getSelectedId() == 1;
    loadBtn.setVisible (! useSc);
    capRefBtn.setVisible (useSc);
    juce::String s;
    if (proc.scTap.isCapturing()) s << "Capturing reference... " << proc.scTap.getCapturedFrames() << " frames\n";
    else if (haveRef) s << "Reference ready" << (fileName.isNotEmpty() ? " (" + fileName + ")" : juce::String()) << "\n";
    else s << (useSc ? "Play the reference into the sidechain, then capture.\n" : "Load a reference track.\n");
    if (proc.preTap.isCapturing()) s << "Capturing this track... " << proc.preTap.getCapturedFrames() << " frames\n";
    else if (haveSrc) s << "This track captured\n";
    else s << "Play this track and capture it (10-30 s works well).\n";
    if (haveTarget) s << "Yellow line = correction. Press Match.";
    if (useSc && ! proc.sidechainActive()) s << "\nSidechain is off: enable it in your DAW.";
    status.setText (s, juce::dontSendNotification);
    matchBtn.setEnabled (haveTarget);
}

void MatchPanel::paint (juce::Graphics& g)
{
    look::drawPanel (g, getLocalBounds().toFloat());
    g.setFont (juce::FontOptions (11.0f));
    auto r = getLocalBounds().reduced (12).removeFromBottom (36);
    g.setColour (juce::Colour (0xffff9f43)); g.drawText ("- reference", r.removeFromLeft (80), juce::Justification::centredLeft);
    g.setColour (juce::Colour (0xff8fb3ff)); g.drawText ("- this track", r.removeFromLeft (80), juce::Justification::centredLeft);
    g.setColour (juce::Colours::yellow); g.drawText ("- correction", r, juce::Justification::centredLeft);
}

void MatchPanel::resized()
{
    auto r = getLocalBounds().reduced (12);
    title.setBounds (r.removeFromTop (24));
    r.removeFromTop (6);
    refSource.setBounds (r.removeFromTop (26)); r.removeFromTop (6);
    auto refRow = r.removeFromTop (28);
    loadBtn.setBounds (refRow); capRefBtn.setBounds (refRow);
    r.removeFromTop (6);
    capSrcBtn.setBounds (r.removeFromTop (28)); r.removeFromTop (10);
    auto line = [&r] (juce::Label& l, juce::Component& c) { auto x = r.removeFromTop (26); l.setBounds (x.removeFromLeft (76)); c.setBounds (x); r.removeFromTop (4); };
    line (bandsLabel, bands);
    line (amountLabel, amount);
    line (smoothLabel, smoothBox);
    r.removeFromTop (6);
    auto b = r.removeFromTop (30);
    clearBtn.setBounds (b.removeFromRight (70)); b.removeFromRight (8);
    matchBtn.setBounds (b);
    r.removeFromTop (8);
    status.setBounds (r.withTrimmedBottom (30));
}

// =============================================================================================
ContourEditor::ContourEditor (ContourProcessor& p)
    : AudioProcessorEditor (p), proc (p), display (p, *this), bandPanel (p, *this), matchPanel (p, *this)
{
    setLookAndFeel (&lnf);
    logo.setText ("CONTOUR EQ", juce::dontSendNotification);
    logo.setFont (juce::FontOptions (19.0f, juce::Font::bold));
    logo.setColour (juce::Label::textColourId, lnf.accent);

    analyserBox.addItem ("Analyser off", 1); analyserBox.addItem ("Analyser: pre", 2);
    analyserBox.addItem ("Analyser: post", 3); analyserBox.addItem ("Analyser: pre + post", 4);
    rangeBox.addItem ("+/- 6 dB", 1); rangeBox.addItem ("+/- 12 dB", 2); rangeBox.addItem ("+/- 24 dB", 3); rangeBox.addItem ("+/- 30 dB", 4);
    const auto ui = proc.apvts.state.getOrCreateChildWithName ("ui", nullptr);
    analyserBox.setSelectedId ((int) ui.getProperty ("analyser", 4), juce::dontSendNotification);
    rangeBox.setSelectedId ((int) ui.getProperty ("range", 3), juce::dontSendNotification);
    auto applyView = [this]
    {
        static const float ranges[] = { 6, 12, 24, 30 };
        display.analyserMode = analyserBox.getSelectedId() - 1;
        display.rangeDb = ranges[juce::jlimit (0, 3, rangeBox.getSelectedId() - 1)];
        auto ui2 = proc.apvts.state.getOrCreateChildWithName ("ui", nullptr);
        ui2.setProperty ("analyser", analyserBox.getSelectedId(), nullptr);
        ui2.setProperty ("range", rangeBox.getSelectedId(), nullptr);
        display.repaint();
    };
    analyserBox.onChange = applyView;
    rangeBox.onChange = applyView;
    applyView();

    aScale = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, "gainscale", scaleKnob.slider);
    aOut = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, "out", outKnob.slider);
    aPan = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, "pan", panKnob.slider);
    for (auto* k : { &scaleKnob, &outKnob, &panKnob })
    {
        k->slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        k->slider.setPopupDisplayEnabled (true, true, this);
        k->label.setFont (juce::FontOptions (10.0f));
    }

    undoBtn.onClick = [this] { proc.undo.undo(); };
    redoBtn.onClick = [this] { proc.undo.redo(); };
    abBtn.onClick = [this] { proc.switchAB(); abBtn.setButtonText (proc.currentAB == 0 ? "A" : "B");
                             abCopyBtn.setButtonText (proc.currentAB == 0 ? "Copy A>B" : "Copy B>A"); selectBand (-1); };
    abCopyBtn.onClick = [this] { proc.copyCurrentToOther(); };
    saveBtn.onClick = [this] { savePreset(); };
    loadBtn.onClick = [this] { loadPreset(); };
    matchBtn.setClickingTogglesState (true);
    matchBtn.onClick = [this] { matchPanel.setVisible (matchBtn.getToggleState()); resized(); };
    phaseLabel.setText ("Zero latency", juce::dontSendNotification);
    phaseLabel.setColour (juce::Label::textColourId, look::textDim);
    phaseLabel.setFont (juce::FontOptions (12.0f));

    for (auto* c : std::initializer_list<juce::Component*> { &logo, &analyserBox, &rangeBox, &scaleKnob, &outKnob, &panKnob, &undoBtn, &redoBtn,
                                                             &abBtn, &abCopyBtn, &saveBtn, &loadBtn, &matchBtn, &phaseLabel, &display, &bandPanel })
        addAndMakeVisible (c);
    addChildComponent (matchPanel);

    display.matchRef = &matchPanel.ref;
    display.matchSrc = &matchPanel.src;
    display.matchTarget = &matchPanel.target;

    setResizable (true, true);
    setResizeLimits (820, 520, 2200, 1400);
    setSize (1120, 680);
    startTimerHz (30);
}

ContourEditor::~ContourEditor()
{
    proc.soloBand.store (-1);
    setLookAndFeel (nullptr);
}

void ContourEditor::selectBand (int b)
{
    selected = b;
    bandPanel.setBand (b);
    display.repaint();
}

void ContourEditor::timerCallback()
{
    proc.preTap.tick();
    proc.postTap.tick();
    proc.scTap.tick();
    if (selected >= 0 && ! proc.isBandOn (selected)) selectBand (-1);
    display.matchRef = matchPanel.haveRef ? &matchPanel.ref : nullptr;
    display.matchSrc = matchPanel.haveSrc ? &matchPanel.src : nullptr;
    display.matchTarget = matchPanel.haveTarget ? &matchPanel.target : nullptr;
    if (matchPanel.isVisible()) matchPanel.tick();
    undoBtn.setEnabled (proc.undo.canUndo());
    redoBtn.setEnabled (proc.undo.canRedo());
    display.repaint();
}

void ContourEditor::paint (juce::Graphics& g) { g.fillAll (look::bg); }

void ContourEditor::resized()
{
    auto r = getLocalBounds().reduced (8);
    auto top = r.removeFromTop (46);
    logo.setBounds (top.removeFromLeft (130));
    analyserBox.setBounds (top.removeFromLeft (170).reduced (0, 9)); top.removeFromLeft (8);
    rangeBox.setBounds (top.removeFromLeft (110).reduced (0, 9)); top.removeFromLeft (8);
    phaseLabel.setBounds (top.removeFromLeft (80));
    for (auto* k : { &panKnob, &outKnob, &scaleKnob }) k->setBounds (top.removeFromRight (56));
    top.removeFromRight (10);
    matchBtn.setBounds (top.removeFromRight (86).reduced (0, 9)); top.removeFromRight (6);
    loadBtn.setBounds (top.removeFromRight (54).reduced (0, 9)); top.removeFromRight (4);
    saveBtn.setBounds (top.removeFromRight (54).reduced (0, 9)); top.removeFromRight (10);
    abCopyBtn.setBounds (top.removeFromRight (78).reduced (0, 9)); top.removeFromRight (4);
    abBtn.setBounds (top.removeFromRight (34).reduced (0, 9)); top.removeFromRight (10);
    redoBtn.setBounds (top.removeFromRight (50).reduced (0, 9)); top.removeFromRight (4);
    undoBtn.setBounds (top.removeFromRight (50).reduced (0, 9));

    r.removeFromTop (4);
    bandPanel.setBounds (r.removeFromBottom (150));
    r.removeFromBottom (8);
    if (matchPanel.isVisible())
    {
        matchPanel.setBounds (r.removeFromRight (300));
        r.removeFromRight (8);
    }
    display.setBounds (r);
}

void ContourEditor::savePreset()
{
    auto dir = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("Contour EQ/Presets");
    dir.createDirectory();
    chooser = std::make_unique<juce::FileChooser> ("Save preset", dir.getChildFile ("My preset.contour"), "*.contour");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting, [this] (const juce::FileChooser& fc)
    {
        auto f = fc.getResult();
        if (f == juce::File()) return;
        if (auto xml = proc.apvts.copyState().createXml()) xml->writeTo (f.withFileExtension ("contour"));
    });
}

void ContourEditor::loadPreset()
{
    auto dir = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("Contour EQ/Presets");
    chooser = std::make_unique<juce::FileChooser> ("Load preset", dir, "*.contour");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& fc)
    {
        auto f = fc.getResult();
        if (! f.existsAsFile()) return;
        if (auto xml = juce::XmlDocument::parse (f))
            if (xml->hasTagName (proc.apvts.state.getType()))
            {
                proc.undo.beginNewTransaction ("Load preset");
                proc.apvts.replaceState (juce::ValueTree::fromXml (*xml));
                selectBand (-1);
            }
    });
}
