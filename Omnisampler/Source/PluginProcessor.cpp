#include "PluginProcessor.h"
#include "PluginEditor.h"

using namespace juce;
using namespace omni;

// Pads and Keys start on their own channels so one note never plays both (10 is the usual drum channel).
static constexpr int kDefaultPadsChannel = 10, kDefaultKeysChannel = 1;
static constexpr int kStateVersion = 2;   // 2: separate Pads/Keys channels by default

AudioProcessorValueTreeState::ParameterLayout OmniProcessor::createLayout()
{
    AudioProcessorValueTreeState::ParameterLayout l;
    StringArray chans { "Any" };
    for (int i = 1; i <= 16; ++i) chans.add ("Ch " + String (i));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "master", 1 }, "Master", NormalisableRange<float> (-48, 12, 0.1f), 0.0f,
           AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (v, 1) + " dB"; })));
    l.add (std::make_unique<AudioParameterChoice> (ParameterID { "padsch", 1 }, "Pads MIDI Channel", chans, kDefaultPadsChannel));
    l.add (std::make_unique<AudioParameterChoice> (ParameterID { "keysch", 1 }, "Keys MIDI Channel", chans, kDefaultKeysChannel));
    return l;
}

// Main stereo output plus Out 2..16. On the desktop they're off until the host turns them on (e.g. Logic's
// "Multi-Output"). On iPad/iPhone they're on from the start, so AUv3 hosts such as AUM and Loopy Pro list them.
// Omnisampler FX is a plain stereo effect: one input, one output. The standalone app also gets an input,
// so it can sample from an audio interface.
AudioProcessor::BusesProperties OmniProcessor::makeBuses (bool isFx)
{
    if (isFx)
        return BusesProperties().withInput ("Input", AudioChannelSet::stereo(), true).withOutput ("Output", AudioChannelSet::stereo(), true);
   #if JUCE_IOS
    constexpr bool extraOn = true;
   #else
    constexpr bool extraOn = false;
   #endif
    auto b = BusesProperties();
   #if ! JUCE_IOS && ! JUCE_ANDROID
    if (PluginHostType::jucePlugInClientCurrentWrapperType == wrapperType_Standalone)
        b = b.withInput ("Input", AudioChannelSet::stereo(), true);
   #endif
    b = b.withOutput ("Main", AudioChannelSet::stereo(), true);
    for (int i = 1; i < omni::kOutputs; ++i) b = b.withOutput ("Out " + String (i + 1), AudioChannelSet::stereo(), extraOn);
    return b;
}

OmniProcessor::OmniProcessor (bool isFx)
    : AudioProcessor (makeBuses (isFx)),
      apvts (*this, nullptr, "Omnisampler", createLayout()),
      fx (isFx),
      monitorInput (isFx)
{
    current = std::make_shared<const Instrument>();
    engine.setStreamer (&streamer);
    startTimer (60);
}

OmniProcessor::~OmniProcessor()
{
    stopTimer();
    autoSampler.cancel();
   #if OMNI_HOSTING
    alive->store (false);
    hostedInst.reset();
   #endif
}

bool OmniProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    const auto& out = l.getMainOutputChannelSet();
    if (out != AudioChannelSet::stereo() && out != AudioChannelSet::mono()) return false;
    for (int i = 1; i < l.outputBuses.size(); ++i)
        if (! l.outputBuses[i].isDisabled() && l.outputBuses[i] != AudioChannelSet::stereo()) return false;
    if (l.inputBuses.size() > 0)
    {
        const auto& in = l.getMainInputChannelSet();
        const bool monoOrStereo = in == AudioChannelSet::mono() || in == AudioChannelSet::stereo();
        if (fx) { if (! monoOrStereo || in.size() > out.size()) return false; }
        else if (! in.isDisabled() && ! monoOrStereo) return false;
    }
    return true;
}

void OmniProcessor::prepareToPlay (double sr, int block)
{
    // A real-time Auto Sampler run can't carry on across a change of sample rate.
    if (autoSampler.state() == AutoSampler::State::Running) autoSampler.cancel();
    rateNow = sr; blockNow = jmax (16, block);
    engine.prepare (sr, jmax (512, block));
    master.reset (sr, 0.05);
    keyboardState.reset();
    padState.reset();
    localPads.ensureSize (2048);
    localKeys.ensureSize (2048);
    autoMidi.ensureSize (2048);
    inBuf.setSize (2, jmax (8192, block));
    inBuf.clear();
    recorder.prepare (sr);
   #if OMNI_HOSTING
    hostMidi.ensureSize (2048);
    const SpinLock::ScopedLockType hl (hostLock);
    if (hostedInst != nullptr)
    {
        hostedInst->setRateAndBufferSizeDetails (sr, block);
        hostedInst->prepareToPlay (sr, block);
        hostBuf.setSize (jmax (2, hostedInst->getTotalNumInputChannels(), hostedInst->getTotalNumOutputChannels()), jmax (16, block));
    }
   #endif
}

void OmniProcessor::processBlock (AudioBuffer<float>& buffer, MidiBuffer& midi)
{
    ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();

    // Keep the input (Omnisampler FX, or the standalone app's audio input) before the buffer is reused for the output.
    const int inCh = jmin (getMainBusNumInputChannels(), buffer.getNumChannels());
    const bool haveIn = inCh > 0 && n <= inBuf.getNumSamples();
    float* inL = inBuf.getWritePointer (0);
    float* inR = inBuf.getWritePointer (1);
    const int inFrames = jmin (n, inBuf.getNumSamples());
    if (haveIn)
    {
        FloatVectorOperations::copy (inL, buffer.getReadPointer (0), n);
        FloatVectorOperations::copy (inR, buffer.getReadPointer (inCh > 1 ? 1 : 0), n);
    }
    else { FloatVectorOperations::clear (inL, inFrames); FloatVectorOperations::clear (inR, inFrames); }
    buffer.clear();
    {
        GenericScopedTryLock<SpinLock> lock (instLock);
        if (lock.isLocked()) engine.setInstrument (current);
    }
    const int padsCh = padsChannel(), keysCh = keysChannel();
    engine.setOffline (isNonRealtime());

    // Recording and the Auto Sampler listen to the input; the Auto Sampler also sends its notes.
    autoMidi.clear();
    if (haveIn)
    {
        Recorder::Transport tr;
        if (auto* ph = getPlayHead())
            if (const auto pos = ph->getPosition())
            {
                const auto ppq = pos->getPpqPosition();
                const auto bpm = pos->getBpm();
                if (ppq.hasValue() && bpm.hasValue())
                {
                    tr.valid = true; tr.playing = pos->getIsPlaying(); tr.ppq = *ppq; tr.bpm = *bpm;
                    if (const auto sig = pos->getTimeSignature()) tr.quartersPerBar = sig->numerator * 4.0 / jmax (1, sig->denominator);
                    const auto bar = pos->getPpqPositionOfLastBarStart();
                    tr.barStartPpq = bar.hasValue() ? *bar : std::floor (*ppq / tr.quartersPerBar) * tr.quartersPerBar;
                }
            }
        recorder.process (inL, inR, n, tr);
        autoSampler.process (inL, inR, n, autoMidi);
    }

    // Where each output's channels start in this block's buffer (-1 = output off -> main).
    // The standalone app only plays its Main output, so everything goes there.
    const bool mainOnly = wrapperType == wrapperType_Standalone;
    int busCh[kOutputs];
    for (int b = 0; b < kOutputs; ++b)
    {
        busCh[b] = -1;
        if (b < getBusCount (false) && (b == 0 || ! mainOnly))
            if (auto* bus = getBus (false, b); bus != nullptr && bus->isEnabled() && bus->getNumberOfChannels() > 0)
                busCh[b] = getChannelIndexInProcessBlockBuffer (false, b, 0);
    }
    if (busCh[0] < 0) busCh[0] = 0;

   #if OMNI_HOSTING
    const bool aud = audition.load();   // a loaded plug-in is being auditioned: it gets what the Keys would
    hostMidi.clear();
   #else
    const bool aud = false;
   #endif

    // On-screen pads and keyboard go straight to their own section, whatever the channel settings are.
    localPads.clear(); localKeys.clear();
    padState.processNextMidiBuffer (localPads, 0, n, true);
    keyboardState.processNextMidiBuffer (localKeys, 0, n, true);
    for (const auto meta : localPads) engine.handleLocal (meta.getMessage(), true);
    for (const auto meta : localKeys)
    {
       #if OMNI_HOSTING
        if (aud) { hostMidi.addEvent (meta.getMessage(), jlimit (0, jmax (0, n - 1), meta.samplePosition)); continue; }
       #endif
        engine.handleLocal (meta.getMessage(), false);
    }

    // MIDI from a controller or the host is split by channel.
    int pos = 0;
    for (const auto meta : midi)
    {
        const int at = jlimit (0, n, meta.samplePosition);
        if (at > pos) { engine.render (buffer, pos, at - pos, busCh, kOutputs); pos = at; }
        const auto msg = meta.getMessage();
        bool toPads = false, toKeys = false;
        if (msg.isNoteOnOrOff()) Engine::route (msg.getChannel(), padsCh, keysCh, toPads, toKeys);
        if (! aud || ! msg.isNoteOnOrOff()) engine.handleMidi (msg, padsCh, keysCh);
        else if (toPads) engine.handleLocal (msg, true);
       #if OMNI_HOSTING
        if (aud && (toKeys || ! msg.isNoteOnOrOff())) hostMidi.addEvent (msg, jlimit (0, jmax (0, n - 1), at));
       #endif
        if (toKeys) keyboardState.processNextMidiEvent (msg);   // light the on-screen key
    }
    if (pos < n) engine.render (buffer, pos, n - pos, busCh, kOutputs);

    master.setTargetValue (Decibels::decibelsToGain (apvts.getRawParameterValue ("master")->load()));
    for (int i = 0; i < n; ++i)
    {
        const float g = master.getNextValue();
        for (int c = 0; c < buffer.getNumChannels(); ++c) buffer.getWritePointer (c)[i] *= g;
    }
    voiceCount.store (engine.activeVoices());

   #if OMNI_HOSTING
    if (aud) renderHosted (buffer, busCh[0], n);
   #endif
    // The input passes through untouched (always in Omnisampler FX unless it is switched off).
    if (haveIn && monitorInput.load())
    {
        const int c = jlimit (0, jmax (0, buffer.getNumChannels() - 1), busCh[0]);
        if (c + 1 < buffer.getNumChannels()) { buffer.addFrom (c, 0, inL, n); buffer.addFrom (c + 1, 0, inR, n); }
        else { buffer.addFrom (c, 0, inL, n, 0.5f); buffer.addFrom (c, 0, inR, n, 0.5f); }
    }
    // Omnisampler FX has a MIDI output: only the Auto Sampler's notes go out, never an echo of what came in.
    if (fx)
    {
        midi.clear();
        if (! autoMidi.isEmpty()) midi.addEvents (autoMidi, 0, n, 0);
    }
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
    pollSampling();
    if (++timerTicks % 8 != 0) return;
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
        if (canLoadInstrument (f)) return loadInstrumentFile (f);
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
    km.output = current->keys.output;
    String err;
    bool ok = false;
    File file = f;
    if (file.hasFileExtension ("zip"))
    {
        file = unpackInstrumentZip (f, err);
        if (file == File()) return err.isNotEmpty() ? err : String ("No instrument inside the zip");
        err.clear();
    }
    if (file.hasFileExtension ("sfz")) ok = loadSfz (file, cache, km, err);
    else if (file.hasFileExtension ("dspreset")) ok = loadDecentSampler (file, cache, km, err);
    else if (file.hasFileExtension ("dslibrary")) ok = loadDsLibrary (file, cache, km, err);
    else if (file.hasFileExtension ("sf2")) ok = loadSf2 (file, cache, km, err);
    else if (file.hasFileExtension ("exs")) ok = loadExs (file, cache, km, err);
    else if (file.hasFileExtension ("adv")) ok = loadAbleton (file, cache, km, err);
    else if (file.hasFileExtension ("nki")) ok = loadKontakt (file, cache, km, err);
    else err = "Unsupported file type";
    if (! ok) return err.isNotEmpty() ? err : String ("Couldn't load");
    edit ([&] (Instrument& in) { in.keys = km; });
    return err;   // may hold a "some samples missing" note
}

String OmniProcessor::saveKeysAsDecentSampler (const File& f)
{
    String err;
    return saveDecentSampler (current->keys, f, err) ? String() : (err.isNotEmpty() ? err : String ("Couldn't save"));
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
    xml->setAttribute ("stateVersion", kStateVersion);
    xml->setAttribute ("streaming", isStreaming());
    xml->setAttribute ("touchVelocity", touchVelocity.load());
    if (hasAudioInput()) xml->setAttribute ("monitor", monitorInput.load());
    xml->addChildElement (instrumentToXml (*current).release());
    copyXmlToBinary (*xml, dest);
}

void OmniProcessor::setStateInformation (const void* data, int size)
{
    auto xml = getXmlFromBinary (data, size);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType())) return;
    setStreaming (xml->getBoolAttribute ("streaming", true));
    xml->removeAttribute ("streaming");
    touchVelocity = xml->getBoolAttribute ("touchVelocity", false);
    xml->removeAttribute ("touchVelocity");
    if (xml->hasAttribute ("monitor")) { monitorInput = xml->getBoolAttribute ("monitor", fx); xml->removeAttribute ("monitor"); }
    if (auto* inst = xml->getChildByName ("Omnisampler"))
    {
        StringArray missing;
        replaceInstrument (instrumentFromXml (*inst, cache, missing));
        xml->removeChildElement (inst, true);
    }
    // Sessions saved before Pads and Keys had separate channels left both on "Any" (the old default),
    // which made one note play both. Move those to the new defaults; anything the user chose stays.
    if (xml->getIntAttribute ("stateVersion", 1) < 2)
    {
        XmlElement* pads = nullptr; XmlElement* keys = nullptr;
        for (auto* e : xml->getChildWithTagNameIterator ("PARAM"))
        {
            if (e->getStringAttribute ("id") == "padsch") pads = e;
            if (e->getStringAttribute ("id") == "keysch") keys = e;
        }
        if (pads != nullptr && keys != nullptr && pads->getDoubleAttribute ("value") < 0.5 && keys->getDoubleAttribute ("value") < 0.5)
        {
            pads->setAttribute ("value", (double) kDefaultPadsChannel);
            keys->setAttribute ("value", (double) kDefaultKeysChannel);
        }
    }
    xml->removeAttribute ("stateVersion");
    apvts.replaceState (ValueTree::fromXml (*xml));
}

AudioProcessorEditor* OmniProcessor::createEditor() { return new OmniEditor (*this); }
AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new OmniProcessor(); }
