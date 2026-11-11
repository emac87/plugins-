#include "PluginProcessor.h"
#include "PluginEditor.h"

using namespace contour;

static juce::NormalisableRange<float> logRange (float lo, float hi)
{
    return { lo, hi,
             [] (float s, float e, float t) { return s * std::pow (e / s, t); },
             [] (float s, float e, float v) { return std::log (v / s) / std::log (e / s); } };
}

juce::AudioProcessorValueTreeState::ParameterLayout ContourProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;
    StringArray types, slopes, places;
    for (int i = 0; i < kNumTypes; ++i) types.add (typeName (i));
    for (int i = 0; i < kNumSlopes; ++i) slopes.add (String (slopeDb (i)) + " dB/oct");
    for (int i = 0; i < 5; ++i) places.add (placementName (i));

    auto hz = AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int)
        { return v >= 1000.0f ? String (v / 1000.0f, 2) + " kHz" : (v < 100 ? String (v, 1) : String (roundToInt (v))) + " Hz"; });
    auto db = AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (v, 1) + " dB"; });
    auto ms = AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return (v < 10 ? String (v, 1) : String (roundToInt (v))) + " ms"; });
    auto qa = AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (v, 2); });

    for (int b = 0; b < kMaxBands; ++b)
    {
        const String n = "Band " + String (b + 1) + " ";
        auto group = std::make_unique<AudioProcessorParameterGroup> ("band" + String (b + 1), n.trim(), "|");
        group->addChild (std::make_unique<AudioParameterBool> (ParameterID { pid (b, "on"), 1 }, n + "On", false));
        group->addChild (std::make_unique<AudioParameterChoice> (ParameterID { pid (b, "type"), 1 }, n + "Shape", types, 0));
        group->addChild (std::make_unique<AudioParameterFloat> (ParameterID { pid (b, "freq"), 1 }, n + "Freq", logRange (10.0f, 30000.0f), 1000.0f, hz));
        group->addChild (std::make_unique<AudioParameterFloat> (ParameterID { pid (b, "gain"), 1 }, n + "Gain", NormalisableRange<float> (-30.0f, 30.0f, 0.01f), 0.0f, db));
        group->addChild (std::make_unique<AudioParameterFloat> (ParameterID { pid (b, "q"), 1 }, n + "Q", logRange (0.025f, 40.0f), 0.707f, qa));
        group->addChild (std::make_unique<AudioParameterChoice> (ParameterID { pid (b, "slope"), 1 }, n + "Slope", slopes, 1));
        group->addChild (std::make_unique<AudioParameterChoice> (ParameterID { pid (b, "place"), 1 }, n + "Placement", places, 0));
        group->addChild (std::make_unique<AudioParameterBool> (ParameterID { pid (b, "dyn"), 1 }, n + "Dynamic", false));
        group->addChild (std::make_unique<AudioParameterFloat> (ParameterID { pid (b, "thr"), 1 }, n + "Threshold", NormalisableRange<float> (-60.0f, 0.0f, 0.1f), -24.0f, db));
        group->addChild (std::make_unique<AudioParameterFloat> (ParameterID { pid (b, "range"), 1 }, n + "Range", NormalisableRange<float> (-30.0f, 30.0f, 0.1f), -6.0f, db));
        group->addChild (std::make_unique<AudioParameterFloat> (ParameterID { pid (b, "att"), 1 }, n + "Attack", logRange (0.5f, 200.0f), 10.0f, ms));
        group->addChild (std::make_unique<AudioParameterFloat> (ParameterID { pid (b, "rel"), 1 }, n + "Release", logRange (5.0f, 2000.0f), 120.0f, ms));
        group->addChild (std::make_unique<AudioParameterBool> (ParameterID { pid (b, "sc"), 1 }, n + "External Sidechain", false));
        layout.add (std::move (group));
    }
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "gainscale", 1 }, "Gain Scale", NormalisableRange<float> (-200.0f, 200.0f, 1.0f), 100.0f,
                AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String ((int) v) + " %"; })));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "out", 1 }, "Output", NormalisableRange<float> (-24.0f, 24.0f, 0.1f), 0.0f, db));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "pan", 1 }, "Output Pan", NormalisableRange<float> (-1.0f, 1.0f, 0.01f), 0.0f));
    return layout;
}

ContourProcessor::ContourProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                          .withInput ("Sidechain", juce::AudioChannelSet::stereo(), false)),
      apvts (*this, &undo, "ContourEQ", createLayout())
{
    for (int b = 0; b < kMaxBands; ++b)
    {
        auto p = [&] (const char* n) { return apvts.getRawParameterValue (pid (b, n)); };
        raw[b] = { p ("on"), p ("type"), p ("freq"), p ("gain"), p ("q"), p ("slope"), p ("place"),
                   p ("dyn"), p ("thr"), p ("range"), p ("att"), p ("rel"), p ("sc") };
    }
    gainScale = apvts.getRawParameterValue ("gainscale");
    outGain = apvts.getRawParameterValue ("out");
    outPan = apvts.getRawParameterValue ("pan");
}

bool ContourProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    const auto& in = l.getMainInputChannelSet();
    const auto& out = l.getMainOutputChannelSet();
    if (in != out) return false;
    if (in != juce::AudioChannelSet::mono() && in != juce::AudioChannelSet::stereo()) return false;
    if (l.inputBuses.size() > 1)
    {
        const auto& sc = l.inputBuses[1];
        if (! sc.isDisabled() && sc != juce::AudioChannelSet::mono() && sc != juce::AudioChannelSet::stereo()) return false;
    }
    return true;
}

bool ContourProcessor::sidechainActive() const
{
    auto* bus = getBus (true, 1);
    return bus != nullptr && bus->isEnabled() && bus->getNumberOfChannels() > 0;
}

void ContourProcessor::prepareToPlay (double sr, int maxBlock)
{
    sampleRateHz = sr;
    for (auto& s : st)
    {
        for (auto& ch : s.filt) for (auto& f : ch) f.reset();
        s.det.reset(); s.env = 0; s.wasOn = false; s.lastType = -1;
        s.freq.reset (sr, 0.04); s.q.reset (sr, 0.04); s.gain.reset (sr, 0.04);
    }
    for (auto& f : soloFilt) f.reset();
    dry.setSize (2, maxBlock);
    outSmoothed.reset (sr, 0.05);
    preTap.setSampleRate (sr); postTap.setSampleRate (sr); scTap.setSampleRate (sr);
}

BandSpec ContourProcessor::getSpec (int b) const
{
    BandSpec s;
    s.type = (BandType) (int) raw[b].type->load();
    s.freq = raw[b].freq->load();
    s.gainDb = raw[b].gain->load() * gainScale->load() / 100.0f;
    s.q = raw[b].q->load();
    s.slopeIndex = (int) raw[b].slope->load();
    return s;
}

void ContourProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    auto main = getBusBuffer (buffer, false, 0);
    const int numCh = juce::jmin (2, main.getNumChannels());
    const int n = buffer.getNumSamples();
    if (numCh == 0 || n == 0) return;
    const double sr = getSampleRateSafe();

    float* L = main.getWritePointer (0);
    float* R = numCh > 1 ? main.getWritePointer (1) : nullptr;

    const float* scL = nullptr; const float* scR = nullptr;
    if (sidechainActive())
    {
        auto sc = getBusBuffer (buffer, true, 1);
        if (sc.getNumChannels() > 0) { scL = sc.getReadPointer (0); scR = sc.getNumChannels() > 1 ? sc.getReadPointer (1) : nullptr; }
    }

    if (dry.getNumSamples() < n) dry.setSize (2, n, false, false, true);
    for (int c = 0; c < numCh; ++c) dry.copyFrom (c, 0, main, c, 0, n);
    const float* dryL = dry.getReadPointer (0);
    const float* dryR = numCh > 1 ? dry.getReadPointer (1) : nullptr;

    preTap.push (dryL, dryR, n);
    if (scL) scTap.push (scL, scR, n);

    const float scale = gainScale->load() / 100.0f;
    constexpr int kSub = 32;

    for (int b = 0; b < kMaxBands; ++b)
    {
        auto& s = st[b];
        const bool on = raw[b].on->load() > 0.5f;
        if (! on) { s.wasOn = false; dynGainDb[b].store (0.0f); continue; }

        const int type = (int) raw[b].type->load();
        const int slope = (int) raw[b].slope->load();
        if (! s.wasOn || type != s.lastType || slope != s.lastSlope)
        {
            for (auto& ch : s.filt) for (auto& f : ch) f.reset();
            s.freq.setCurrentAndTargetValue (raw[b].freq->load());
            s.q.setCurrentAndTargetValue (raw[b].q->load());
            s.gain.setCurrentAndTargetValue (raw[b].gain->load() * scale);
            s.lastType = type; s.lastSlope = slope; s.wasOn = true;
            s.lastSpec.freq = -1;   // force a redesign
        }
        s.freq.setTargetValue (raw[b].freq->load());
        s.q.setTargetValue (raw[b].q->load());
        s.gain.setTargetValue (raw[b].gain->load() * scale);

        const auto btype = (BandType) type;
        const bool dynamic = raw[b].dyn->load() > 0.5f && typeUsesGain (btype);
        const int place = (int) raw[b].place->load();
        const bool useSc = raw[b].sc->load() > 0.5f && scL != nullptr;
        const double thr = raw[b].thr->load(), range = raw[b].range->load();
        const double aA = 1.0 - std::exp (-1.0 / (raw[b].att->load() * 0.001 * sr));
        const double aR = 1.0 - std::exp (-1.0 / (raw[b].rel->load() * 0.001 * sr));
        float lastDyn = 0.0f;

        for (int start = 0; start < n; start += kSub)
        {
            const int len = juce::jmin (kSub, n - start);
            s.freq.skip (len); s.q.skip (len); s.gain.skip (len);

            double dynDb = 0.0;
            if (dynamic)
            {
                const auto detC = bandPass (std::clamp (s.freq.getCurrentValue(), 10.0, sr * 0.49), std::max (0.3, s.q.getCurrentValue()), sr);
                const float* srcL = useSc ? scL : dryL;
                const float* srcR = useSc ? scR : dryR;
                for (int i = start; i < start + len; ++i)
                {
                    const float x = srcR ? 0.5f * (srcL[i] + srcR[i]) : srcL[i];
                    const double y = s.det.process (x, detC);
                    const double p = y * y;
                    s.env += (p > s.env ? aA : aR) * (p - s.env);
                }
                const double levelDb = 10.0 * std::log10 (s.env + 1e-12);
                const double over = levelDb - thr;
                dynDb = over > 0.0 ? range * std::min (1.0, over / 12.0) : 0.0;
                lastDyn = (float) dynDb;
            }

            BandSpec spec;
            spec.type = btype; spec.slopeIndex = slope;
            spec.freq = s.freq.getCurrentValue(); spec.q = s.q.getCurrentValue();
            spec.gainDb = s.gain.getCurrentValue() + dynDb * scale;
            if (std::abs (spec.freq - s.lastSpec.freq) > 1e-4 || std::abs (spec.q - s.lastSpec.q) > 1e-5
                || std::abs (spec.gainDb - s.lastSpec.gainDb) > 1e-4)
            {
                s.design = designBand (spec, sr);
                s.lastSpec = spec;
            }
            const auto& d = s.design;
            auto run = [&d] (contour::Biquad* f, float x) noexcept
            {
                for (int k = 0; k < d.numSections; ++k) x = f[k].process (x, d.sec[k]);
                return x;
            };

            if (R == nullptr || place == (int) Placement::Stereo)
            {
                for (int i = start; i < start + len; ++i) L[i] = run (s.filt[0], L[i]);
                if (R) for (int i = start; i < start + len; ++i) R[i] = run (s.filt[1], R[i]);
            }
            else if (place == (int) Placement::Left)
                for (int i = start; i < start + len; ++i) L[i] = run (s.filt[0], L[i]);
            else if (place == (int) Placement::Right)
                for (int i = start; i < start + len; ++i) R[i] = run (s.filt[1], R[i]);
            else
            {
                const bool mid = place == (int) Placement::Mid;
                for (int i = start; i < start + len; ++i)
                {
                    float m = 0.5f * (L[i] + R[i]), sd = 0.5f * (L[i] - R[i]);
                    if (mid) m = run (s.filt[0], m); else sd = run (s.filt[0], sd);
                    L[i] = m + sd; R[i] = m - sd;
                }
            }
        }
        dynGainDb[b].store (lastDyn);
    }

    // Audition: hear only the soloed band's region (band-pass of the dry signal).
    const int solo = soloBand.load();
    if (solo >= 0 && solo < kMaxBands && raw[solo].on->load() > 0.5f)
    {
        const auto t = (BandType) (int) raw[solo].type->load();
        const double q = (t == BandType::Bell || t == BandType::Notch || t == BandType::BandPass) ? raw[solo].q->load() : 0.7;
        const auto c = bandPass (std::clamp ((double) raw[solo].freq->load(), 10.0, sr * 0.49), std::max (0.2, q), sr);
        for (int i = 0; i < n; ++i) L[i] = soloFilt[0].process (dryL[i], c);
        if (R) for (int i = 0; i < n; ++i) R[i] = soloFilt[1].process (dryR[i], c);
    }

    // Output gain and balance.
    outSmoothed.setTargetValue (juce::Decibels::decibelsToGain (outGain->load()));
    const float pan = outPan->load();
    const float gl = R ? juce::jmin (1.0f, 1.0f - pan) : 1.0f, gr = juce::jmin (1.0f, 1.0f + pan);
    for (int i = 0; i < n; ++i)
    {
        const float g = outSmoothed.getNextValue();
        L[i] *= g * gl;
        if (R) R[i] *= g * gr;
    }
    for (int c = numCh; c < buffer.getNumChannels(); ++c) buffer.clear (c, 0, n);

    postTap.push (L, R, n);
}

int ContourProcessor::findFreeBand() const
{
    for (int b = 0; b < kMaxBands; ++b)
        if (! isBandOn (b)) return b;
    return -1;
}

void ContourProcessor::setParam (const juce::String& id, float v)
{
    if (auto* p = apvts.getParameter (id))
        p->setValueNotifyingHost (p->convertTo0to1 (v));
}

void ContourProcessor::applyMatch (const std::vector<MatchBand>& bands)
{
    undo.beginNewTransaction ("Match EQ");
    for (int b = 0; b < kMaxBands; ++b) setParam (pid (b, "on"), 0.0f);
    for (size_t i = 0; i < bands.size() && i < (size_t) kMaxBands; ++i)
    {
        const int b = (int) i;
        setParam (pid (b, "type"), (float) (int) bands[i].type);
        setParam (pid (b, "freq"), (float) bands[i].freq);
        setParam (pid (b, "gain"), (float) bands[i].gainDb);
        setParam (pid (b, "q"), (float) bands[i].q);
        setParam (pid (b, "dyn"), 0.0f);
        setParam (pid (b, "place"), 0.0f);
        setParam (pid (b, "on"), 1.0f);
    }
    setParam ("gainscale", 100.0f);
}

void ContourProcessor::switchAB()
{
    abSlot[currentAB] = apvts.copyState();
    currentAB = 1 - currentAB;
    if (abSlot[currentAB].isValid())
        apvts.replaceState (abSlot[currentAB].createCopy());
}

void ContourProcessor::copyCurrentToOther()
{
    abSlot[1 - currentAB] = apvts.copyState();
}

void ContourProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    auto state = apvts.copyState();
    if (auto xml = state.createXml()) copyXmlToBinary (*xml, dest);
}

void ContourProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessorEditor* ContourProcessor::createEditor() { return new ContourEditor (*this); }
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new ContourProcessor(); }
