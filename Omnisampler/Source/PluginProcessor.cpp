#include "PluginProcessor.h"
#include "PluginEditor.h"

using namespace juce;
using namespace omni;

AudioProcessorValueTreeState::ParameterLayout OmniProcessor::createLayout()
{
    AudioProcessorValueTreeState::ParameterLayout l;
    StringArray chans { "Any" };
    for (int i = 1; i <= 16; ++i) chans.add ("Ch " + String (i));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "master", 1 }, "Master", NormalisableRange<float> (-48, 12, 0.1f), 0.0f,
           AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (v, 1) + " dB"; })));
    l.add (std::make_unique<AudioParameterChoice> (ParameterID { "padsch", 1 }, "Pads MIDI Channel", chans, 0));
    l.add (std::make_unique<AudioParameterChoice> (ParameterID { "keysch", 1 }, "Keys MIDI Channel", chans, 0));
    return l;
}

OmniProcessor::OmniProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "Omnisampler", createLayout())
{
    current = std::make_shared<const Instrument>();
    startTimer (500);
}

OmniProcessor::~OmniProcessor() { stopTimer(); }

bool OmniProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    const auto& out = l.getMainOutputChannelSet();
    return out == AudioChannelSet::stereo() || out == AudioChannelSet::mono();
}

void OmniProcessor::prepareToPlay (double sr, int)
{
    engine.prepare (sr);
    master.reset (sr, 0.05);
    keyboardState.reset();
}

void OmniProcessor::processBlock (AudioBuffer<float>& buffer, MidiBuffer& midi)
{
    ScopedNoDenormals noDenormals;
    buffer.clear();
    const int n = buffer.getNumSamples();
    {
        GenericScopedTryLock<SpinLock> lock (instLock);
        if (lock.isLocked()) engine.setInstrument (current);
    }
    keyboardState.processNextMidiBuffer (midi, 0, n, true);
    const int padsCh = padsChannel(), keysCh = keysChannel();
    int pos = 0;
    for (const auto meta : midi)
    {
        const int at = jlimit (0, n, meta.samplePosition);
        if (at > pos) { engine.render (buffer, pos, at - pos); pos = at; }
        engine.handleMidi (meta.getMessage(), padsCh, keysCh);
    }
    if (pos < n) engine.render (buffer, pos, n - pos);

    master.setTargetValue (Decibels::decibelsToGain (apvts.getRawParameterValue ("master")->load()));
    for (int i = 0; i < n; ++i)
    {
        const float g = master.getNextValue();
        for (int c = 0; c < buffer.getNumChannels(); ++c) buffer.getWritePointer (c)[i] *= g;
    }
    voiceCount.store (engine.activeVoices());
}

void OmniProcessor::swapIn (InstrumentPtr p)
{
    InstrumentPtr old;
    {
        GenericScopedLock<SpinLock> lock (instLock);
        old = std::move (current);
        current = std::move (p);
    }
    // Keep the old one around a moment so playing voices never free it on the audio thread.
    const ScopedLock sl (graveLock);
    if (old) graveyard.push_back ({ std::move (old), Time::getMillisecondCounter() });
}

void OmniProcessor::timerCallback()
{
    const auto now = Time::getMillisecondCounter();
    const ScopedLock sl (graveLock);
    graveyard.erase (std::remove_if (graveyard.begin(), graveyard.end(),
                     [now] (const auto& e) { return e.first.use_count() == 1 && now - e.second > 3000; }), graveyard.end());
}

void OmniProcessor::edit (const std::function<void (Instrument&)>& fn)
{
    auto copy = std::make_shared<Instrument> (*current);
    fn (*copy);
    swapIn (std::move (copy));
}

void OmniProcessor::replaceInstrument (Instrument inst) { swapIn (std::make_shared<const Instrument> (std::move (inst))); }

String OmniProcessor::loadFilesToPad (int pad, const Array<File>& files, DropMode mode)
{
    std::vector<SamplePtr> samples;
    String errs;
    for (auto& f : files)
    {
        String e;
        if (auto s = cache.get (f, e)) samples.push_back (s); else errs << e << "\n";
    }
    if (samples.empty()) return errs.isNotEmpty() ? errs : String ("No audio files");
    auto zones = mapPadFiles (samples, mode);
    edit ([&] (Instrument& in)
    {
        auto& p = in.pads[pad];
        p.zones = zones;
        if (p.name.isEmpty() || files.size() == 1) p.name = files[0].getFileNameWithoutExtension();
        if (mode == DropMode::RoundRobin) p.voicing.rr = RRMode::Sequential;
    });
    return errs;
}

String OmniProcessor::loadFilesToKeys (const Array<File>& files, bool replace)
{
    Array<File> audio;
    for (auto& f : files)
    {
        if (isInstrumentFile (f)) return loadInstrumentFile (f);
        if (f.isDirectory()) audio.addArray (f.findChildFiles (File::findFiles, true, "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg"));
        else if (isAudioFile (f)) audio.add (f);
    }
    std::vector<SamplePtr> samples;
    String errs;
    for (auto& f : audio)
    {
        String e;
        if (auto s = cache.get (f, e)) samples.push_back (s); else errs << e << "\n";
    }
    if (samples.empty()) return errs.isNotEmpty() ? errs : String ("No audio files");
    auto zones = mapKeyFiles (samples);
    edit ([&] (Instrument& in)
    {
        if (replace) in.keys.zones.clear();
        for (auto& z : zones) in.keys.zones.push_back (z);
        if (in.keys.name.isEmpty() || replace) in.keys.name = audio.size() == 1 ? audio[0].getFileNameWithoutExtension() : audio[0].getParentDirectory().getFileName();
    });
    return errs;
}

String OmniProcessor::loadInstrumentFile (const File& f)
{
    KeysMap km;
    km.voicing = current->keys.voicing;
    String err;
    bool ok = false;
    if (f.hasFileExtension ("sfz")) ok = loadSfz (f, cache, km, err);
    else if (f.hasFileExtension ("dspreset")) ok = loadDecentSampler (f, cache, km, err);
    else if (f.hasFileExtension ("dslibrary")) ok = loadDsLibrary (f, cache, km, err);
    else err = "Unsupported file type";
    if (! ok) return err.isNotEmpty() ? err : String ("Couldn't load");
    edit ([&] (Instrument& in) { in.keys = km; });
    return err;   // may hold a "some samples missing" note
}

String OmniProcessor::loadKit (const File& f)
{
    auto xml = XmlDocument::parse (f);
    if (xml == nullptr || ! xml->hasTagName ("Omnisampler")) return "Not an Omnisampler kit";
    StringArray missing;
    replaceInstrument (instrumentFromXml (*xml, cache, missing));
    return missing.isEmpty() ? String() : String (missing.size()) + " sample file(s) are missing, e.g. " + missing[0];
}

String OmniProcessor::saveKit (const File& f)
{
    auto xml = instrumentToXml (*current);
    return xml->writeTo (f) ? String() : "Couldn't write " + f.getFullPathName();
}

void OmniProcessor::getStateInformation (MemoryBlock& dest)
{
    auto state = apvts.copyState();
    auto xml = state.createXml();
    xml->addChildElement (instrumentToXml (*current).release());
    copyXmlToBinary (*xml, dest);
}

void OmniProcessor::setStateInformation (const void* data, int size)
{
    auto xml = getXmlFromBinary (data, size);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType())) return;
    if (auto* inst = xml->getChildByName ("Omnisampler"))
    {
        StringArray missing;
        replaceInstrument (instrumentFromXml (*inst, cache, missing));
        xml->removeChildElement (inst, true);
    }
    apvts.replaceState (ValueTree::fromXml (*xml));
}

AudioProcessorEditor* OmniProcessor::createEditor() { return new OmniEditor (*this); }
AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new OmniProcessor(); }
