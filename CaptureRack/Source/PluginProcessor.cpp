#include "PluginProcessor.h"
#include "PluginEditor.h"

using namespace juce;

static NormalisableRange<float> logRange (float lo, float hi)
{
    return { lo, hi,
             [] (float s, float e, float t) { return s * std::pow (e / s, t); },
             [] (float s, float e, float v) { return std::log (v / s) / std::log (e / s); } };
}

AudioProcessorValueTreeState::ParameterLayout CaptureRackProcessor::createLayout()
{
    AudioProcessorValueTreeState::ParameterLayout l;
    auto db = AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (v, 1) + " dB"; });
    auto gate = AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return v <= -99.0f ? String ("Off") : String (roundToInt (v)) + " dB"; });
    auto hz = AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return v >= 1000 ? String (v / 1000, 1) + " kHz" : String ((int) v) + " Hz"; });
    auto pct = AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String ((int) v) + " %"; });

    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "input", 1 }, "Input", NormalisableRange<float> (-24, 24, 0.1f), 0.0f, db));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "output", 1 }, "Output", NormalisableRange<float> (-24, 24, 0.1f), 0.0f, db));
    l.add (std::make_unique<AudioParameterChoice> (ParameterID { "routing", 1 }, "Routing", StringArray { "Series", "Parallel" }, 0));
    l.add (std::make_unique<AudioParameterBool> (ParameterID { "normalise", 1 }, "Normalise", true));
    for (int s = 0; s < kSlots; ++s)
    {
        const String n = "Slot " + String (s + 1) + " ";
        l.add (std::make_unique<AudioParameterBool> (ParameterID { sid (s, "on"), 1 }, n + "On", true));
        l.add (std::make_unique<AudioParameterFloat> (ParameterID { sid (s, "in"), 1 }, n + "Input", NormalisableRange<float> (-24, 24, 0.1f), 0.0f, db));
        l.add (std::make_unique<AudioParameterFloat> (ParameterID { sid (s, "out"), 1 }, n + "Output", NormalisableRange<float> (-24, 24, 0.1f), 0.0f, db));
        l.add (std::make_unique<AudioParameterFloat> (ParameterID { sid (s, "gate"), 1 }, n + "Gate", NormalisableRange<float> (-100, -20, 0.5f), -100.0f, gate));
        l.add (std::make_unique<AudioParameterFloat> (ParameterID { sid (s, "level"), 1 }, n + "Level", NormalisableRange<float> (0, 100, 1), 100.0f, pct));
        l.add (std::make_unique<AudioParameterFloat> (ParameterID { sid (s, "pan"), 1 }, n + "Pan", NormalisableRange<float> (-1, 1, 0.01f), 0.0f));
    }
    l.add (std::make_unique<AudioParameterBool> (ParameterID { "ir_on", 1 }, "IR On", true));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "ir_mix", 1 }, "IR Mix", NormalisableRange<float> (0, 100, 1), 100.0f, pct));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "ir_level", 1 }, "IR Level", NormalisableRange<float> (-24, 12, 0.1f), 0.0f, db));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "low_cut", 1 }, "Low Cut", logRange (20, 500), 20.0f, hz));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "high_cut", 1 }, "High Cut", logRange (2000, 20000), 20000.0f, hz));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "bass", 1 }, "Bass", NormalisableRange<float> (-12, 12, 0.1f), 0.0f, db));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "mid", 1 }, "Mid", NormalisableRange<float> (-12, 12, 0.1f), 0.0f, db));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "treble", 1 }, "Treble", NormalisableRange<float> (-12, 12, 0.1f), 0.0f, db));
    return l;
}

CaptureRackProcessor::CaptureRackProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", AudioChannelSet::stereo(), true)
                          .withOutput ("Output", AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "CaptureRack", createLayout())
{
}

CaptureRackProcessor::~CaptureRackProcessor() = default;

bool CaptureRackProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    const auto& in = l.getMainInputChannelSet();
    const auto& out = l.getMainOutputChannelSet();
    if (out != AudioChannelSet::mono() && out != AudioChannelSet::stereo()) return false;
    return in == AudioChannelSet::mono() || in == AudioChannelSet::stereo();
}

void CaptureRackProcessor::prepareToPlay (double sr, int block)
{
    const bool rateChanged = std::abs (sr - sampleRateHz) > 0.5 || block > maxBlock;
    sampleRateHz = sr;
    maxBlock = juce::jmax (block, 32);
    mono.setSize (1, maxBlock);
    work.setSize (2, maxBlock);
    parallelSum.setSize (2, maxBlock);
    dryStereo.setSize (2, maxBlock);
    gateGains.setSize (1, maxBlock);
    for (auto& g : gates) g.prepare (sr);

    dsp::ProcessSpec spec { sr, (uint32) maxBlock, 2 };
    conv.prepare (spec);
    for (int c = 0; c < 2; ++c)
    {
        for (auto* f : { &lowCut[c], &highCut[c], &bass[c], &mid[c], &treble[c] }) { f->reset(); }
    }
    lastLow = lastHigh = -1; lastBass = lastMid = lastTreble = 99;
    prepareFilters();

    // Models are prepared for one host rate; reload them if that changed.
    if (rateChanged)
        for (int s = 0; s < kSlots; ++s)
            if (slotFiles[s].existsAsFile())
            {
                String err;
                loadSlot (s, slotFiles[s], err);
            }
}

void CaptureRackProcessor::prepareFilters()
{
    const float lc = apvts.getRawParameterValue ("low_cut")->load();
    const float hc = apvts.getRawParameterValue ("high_cut")->load();
    const float b = apvts.getRawParameterValue ("bass")->load();
    const float m = apvts.getRawParameterValue ("mid")->load();
    const float t = apvts.getRawParameterValue ("treble")->load();
    const double sr = sampleRateHz;
    if (lc != lastLow)    { auto c = Coefs::makeHighPass (sr, lc, 0.707f); for (auto& f : lowCut) f.coefficients = c; lastLow = lc; }
    if (hc != lastHigh)   { auto c = Coefs::makeLowPass (sr, jmin (hc, (float) sr * 0.45f), 0.707f); for (auto& f : highCut) f.coefficients = c; lastHigh = hc; }
    if (b != lastBass)    { auto c = Coefs::makeLowShelf (sr, 120.0, 0.7f, Decibels::decibelsToGain (b)); for (auto& f : bass) f.coefficients = c; lastBass = b; }
    if (m != lastMid)     { auto c = Coefs::makePeakFilter (sr, 800.0, 0.7f, Decibels::decibelsToGain (m)); for (auto& f : mid) f.coefficients = c; lastMid = m; }
    if (t != lastTreble)  { auto c = Coefs::makeHighShelf (sr, 3200.0, 0.7f, Decibels::decibelsToGain (t)); for (auto& f : treble) f.coefficients = c; lastTreble = t; }
}

static void applyGain (float* x, int n, float g) noexcept { for (int i = 0; i < n; ++i) x[i] *= g; }
static float peakOf (const float* x, int n) noexcept { float p = 0; for (int i = 0; i < n; ++i) p = jmax (p, std::abs (x[i])); return p; }

void CaptureRackProcessor::processBlock (AudioBuffer<float>& buffer, MidiBuffer&)
{
    ScopedNoDenormals noDenormals;
    const int totalIn = getTotalNumInputChannels(), totalOut = getTotalNumOutputChannels();
    const int N = buffer.getNumSamples();
    if (N == 0) return;
    const bool stereoOut = totalOut > 1;

    for (int start = 0; start < N; start += maxBlock)
    {
        const int n = jmin (maxBlock, N - start);

        // Guitar in: mono sum of the inputs.
        float* m = mono.getWritePointer (0);
        if (totalIn > 1)
            for (int i = 0; i < n; ++i) m[i] = 0.5f * (buffer.getSample (0, start + i) + buffer.getSample (1, start + i));
        else
            FloatVectorOperations::copy (m, buffer.getReadPointer (0, start), n);
        applyGain (m, n, Decibels::decibelsToGain (apvts.getRawParameterValue ("input")->load()));
        inMeter.store (jmax (peakOf (m, n), inMeter.load() * 0.9f));

        const bool normalise = apvts.getRawParameterValue ("normalise")->load() > 0.5f;
        const bool parallel = apvts.getRawParameterValue ("routing")->load() > 0.5f;

        auto runSlot = [&] (int s, float* x) -> bool
        {
            if (apvts.getRawParameterValue (sid (s, "on"))->load() < 0.5f) return false;
            GenericScopedTryLock<SpinLock> lock (slotLock[s]);
            if (! lock.isLocked() || models[s] == nullptr) return false;
            auto& mdl = *models[s];
            applyGain (x, n, Decibels::decibelsToGain (apvts.getRawParameterValue (sid (s, "in"))->load()));
            float* gg = gateGains.getWritePointer (0);
            gates[s].process (x, n, apvts.getRawParameterValue (sid (s, "gate"))->load(), gg);
            gateGain[s].store (gates[s].currentGain());
            rack::processModel (mdl, x, n);
            FloatVectorOperations::multiply (x, gg, n);
            float outDb = apvts.getRawParameterValue (sid (s, "out"))->load();
            if (normalise && mdl.hasLoudness) outDb += (float) (-18.0 - mdl.loudnessDb);
            applyGain (x, n, Decibels::decibelsToGain (outDb));
            return true;
        };

        float* L = work.getWritePointer (0);
        float* R = work.getWritePointer (1);
        if (! parallel)
        {
            for (int k = 0; k < kSlots; ++k) runSlot (order[k].load(), m);
            FloatVectorOperations::copy (L, m, n);
            FloatVectorOperations::copy (R, m, n);
        }
        else
        {
            parallelSum.clear (0, n);
            bool any = false;
            float* x = work.getWritePointer (0);
            for (int s = 0; s < kSlots; ++s)
            {
                FloatVectorOperations::copy (x, m, n);
                if (! runSlot (s, x)) continue;
                any = true;
                const float level = apvts.getRawParameterValue (sid (s, "level"))->load() / 100.0f;
                const float pan = apvts.getRawParameterValue (sid (s, "pan"))->load();
                const float gl = level * std::cos ((pan + 1.0f) * MathConstants<float>::pi * 0.25f) * 1.41421356f;
                const float gr = level * std::sin ((pan + 1.0f) * MathConstants<float>::pi * 0.25f) * 1.41421356f;
                parallelSum.addFrom (0, 0, x, n, gl);
                parallelSum.addFrom (1, 0, x, n, gr);
            }
            if (any) { work.copyFrom (0, 0, parallelSum, 0, 0, n); work.copyFrom (1, 0, parallelSum, 1, 0, n); }
            else { FloatVectorOperations::copy (L, m, n); FloatVectorOperations::copy (R, m, n); }
        }

        // Cab IR with mix and level.
        if (irLoaded.load() && apvts.getRawParameterValue ("ir_on")->load() > 0.5f)
        {
            const float mix = apvts.getRawParameterValue ("ir_mix")->load() / 100.0f;
            const float lvl = Decibels::decibelsToGain (apvts.getRawParameterValue ("ir_level")->load());
            dryStereo.copyFrom (0, 0, work, 0, 0, n);
            dryStereo.copyFrom (1, 0, work, 1, 0, n);
            dsp::AudioBlock<float> blk (work.getArrayOfWritePointers(), 2, (size_t) n);
            conv.process (dsp::ProcessContextReplacing<float> (blk));
            for (int c = 0; c < 2; ++c)
            {
                auto* w = work.getWritePointer (c);
                const auto* d = dryStereo.getReadPointer (c);
                for (int i = 0; i < n; ++i) w[i] = d[i] * (1.0f - mix) + w[i] * mix * lvl;
            }
        }

        // Cuts and tone EQ.
        prepareFilters();
        for (int c = 0; c < 2; ++c)
        {
            auto* w = work.getWritePointer (c);
            for (int i = 0; i < n; ++i)
            {
                float y = w[i];
                if (lastLow > 20.5f) y = lowCut[c].processSample (y);
                if (lastHigh < 19900.0f) y = highCut[c].processSample (y);
                if (std::abs (lastBass) > 0.05f) y = bass[c].processSample (y);
                if (std::abs (lastMid) > 0.05f) y = mid[c].processSample (y);
                if (std::abs (lastTreble) > 0.05f) y = treble[c].processSample (y);
                w[i] = y;
            }
        }

        const float out = Decibels::decibelsToGain (apvts.getRawParameterValue ("output")->load());
        if (stereoOut)
        {
            buffer.copyFrom (0, start, work, 0, 0, n);
            buffer.copyFrom (1, start, work, 1, 0, n);
            buffer.applyGain (0, start, n, out);
            buffer.applyGain (1, start, n, out);
        }
        else
        {
            for (int i = 0; i < n; ++i) buffer.setSample (0, start + i, 0.5f * (L[i] + R[i]) * out);
        }
        outMeter.store (jmax (peakOf (buffer.getReadPointer (0, start), n), outMeter.load() * 0.9f));
    }
    for (int c = jmax (2, totalOut); c < buffer.getNumChannels(); ++c) buffer.clear (c, 0, N);
}

bool CaptureRackProcessor::loadSlot (int s, const File& f, String& error)
{
    auto lm = rack::loadModel (f, sampleRateHz, maxBlock, error);
    if (lm == nullptr) return false;
    std::unique_ptr<rack::LoadedModel> old;
    {
        GenericScopedLock<SpinLock> lock (slotLock[s]);
        old = std::move (models[s]);
        models[s] = std::move (lm);
        slotFiles[s] = f;
    }
    return true;   // old model is freed here, outside the lock and off the audio thread
}

void CaptureRackProcessor::clearSlot (int s)
{
    std::unique_ptr<rack::LoadedModel> old;
    GenericScopedLock<SpinLock> lock (slotLock[s]);
    old = std::move (models[s]);
    slotFiles[s] = File();
}

String CaptureRackProcessor::slotName (int s) const { return slotFiles[s].existsAsFile() ? slotFiles[s].getFileNameWithoutExtension() : String(); }
File CaptureRackProcessor::slotFile (int s) const { return slotFiles[s]; }
String CaptureRackProcessor::slotInfo (int s) const
{
    auto* self = const_cast<CaptureRackProcessor*> (this);
    GenericScopedLock<SpinLock> lock (self->slotLock[s]);
    if (models[s] == nullptr) return {};
    String i = models[s]->info;
    if (models[s]->author.isNotEmpty()) i << "  by " << models[s]->author;
    return i;
}

bool CaptureRackProcessor::loadIR (const File& f, String& error)
{
    if (! f.existsAsFile()) { error = "File not found"; return false; }
    AudioFormatManager fm; fm.registerBasicFormats();
    std::unique_ptr<AudioFormatReader> r (fm.createReaderFor (f));
    if (r == nullptr) { error = "Not an audio file"; return false; }
    conv.loadImpulseResponse (f, dsp::Convolution::Stereo::yes, dsp::Convolution::Trim::yes, 0, dsp::Convolution::Normalise::yes);
    currentIR = f;
    irLoaded.store (true);
    return true;
}

void CaptureRackProcessor::clearIR()
{
    irLoaded.store (false);
    currentIR = File();
}

std::array<int, CaptureRackProcessor::kSlots> CaptureRackProcessor::getOrder() const
{
    return { order[0].load(), order[1].load(), order[2].load() };
}

void CaptureRackProcessor::moveSlot (int s, int delta)
{
    auto o = getOrder();
    int pos = 0;
    for (int k = 0; k < kSlots; ++k) if (o[(size_t) k] == s) pos = k;
    const int to = jlimit (0, kSlots - 1, pos + delta);
    std::swap (o[(size_t) pos], o[(size_t) to]);
    for (int k = 0; k < kSlots; ++k) order[k].store (o[(size_t) k]);
}

void CaptureRackProcessor::getStateInformation (MemoryBlock& dest)
{
    auto state = apvts.copyState();
    ValueTree files ("files");
    for (int s = 0; s < kSlots; ++s) files.setProperty ("slot" + String (s), slotFiles[s].getFullPathName(), nullptr);
    files.setProperty ("ir", currentIR.getFullPathName(), nullptr);
    auto o = getOrder();
    files.setProperty ("order", String (o[0]) + "," + String (o[1]) + "," + String (o[2]), nullptr);
    state.removeChild (state.getChildWithName ("files"), nullptr);
    state.addChild (files, -1, nullptr);
    if (auto xml = state.createXml()) copyXmlToBinary (*xml, dest);
}

void CaptureRackProcessor::setStateInformation (const void* data, int size)
{
    auto xml = getXmlFromBinary (data, size);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType())) return;
    auto state = ValueTree::fromXml (*xml);
    apvts.replaceState (state);
    auto files = state.getChildWithName ("files");
    for (int s = 0; s < kSlots; ++s)
    {
        File f (files.getProperty ("slot" + String (s)).toString());
        String err;
        if (f.existsAsFile()) loadSlot (s, f, err); else clearSlot (s);
    }
    File ir (files.getProperty ("ir").toString());
    String err;
    if (ir.existsAsFile()) loadIR (ir, err); else clearIR();
    auto parts = StringArray::fromTokens (files.getProperty ("order").toString(), ",", "");
    if (parts.size() == kSlots)
        for (int k = 0; k < kSlots; ++k) order[k].store (jlimit (0, kSlots - 1, parts[k].getIntValue()));
}

AudioProcessorEditor* CaptureRackProcessor::createEditor() { return new CaptureRackEditor (*this); }
AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new CaptureRackProcessor(); }
