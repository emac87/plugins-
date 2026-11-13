// OmniProcessor: recording from the input, the Auto Sampler, and loading a plug-in to sample.
#include "PluginProcessor.h"

using namespace juce;
using namespace omni;

static String padLabel (int pad) { return String::charToString ((juce_wchar) "ABCD"[jlimit (0, 3, pad / kPadsPerBank)]) + String (pad % kPadsPerBank + 1); }

// ---- recording --------------------------------------------------------------------------------
bool OmniProcessor::startRecording (const RecordSetup& setup)
{
    if (! hasAudioInput()) return false;
    rec = setup;
    rec.pad = jlimit (0, kPads - 1, rec.pad);
    if (rec.mode != Recorder::Mode::Threshold) rec.keepGoing = false;
    return recorder.arm (rec.mode, rec.thresholdDb, rec.bars);
}

void OmniProcessor::stopRecording()
{
    rec.keepGoing = false;
    recorder.stop();
}

void OmniProcessor::pollSampling()
{
    if (recorder.state() == Recorder::State::Done) finishTake();
    const auto as = autoSampler.state();
    if (as == AutoSampler::State::Finished) finishAutoSample();
    else if (as == AutoSampler::State::Cancelled)
    {
        autoSampler.reset();
        samplingMessage = "Auto Sampler stopped. The notes it had finished are in " + autoSampler.folder().getFullPathName();
        ++samplingSerial;
    }
}

// The take is trimmed, levelled, saved in the library's Recordings folder and put where it was asked for.
void OmniProcessor::finishTake()
{
    const auto setup = rec;
    const double sr = recorder.rate();
    const bool exact = recorder.mode() == Recorder::Mode::Bars;   // a bar-long take keeps its exact length, so it loops
    const auto bounds = findTakeBounds (recorder.take(), recorder.frames(), sr, setup.trim && ! exact, setup.trim && ! exact);
    String msg;
    bool placed = false;
    if (bounds.silent) msg = "Nothing recorded: the input was silent.";
    else
    {
        // Layers on one pad keep their recorded level, so soft and hard hits sort themselves by loudness.
        const bool level = setup.normalize && setup.target != TakeTarget::PadLayer;
        auto cut = cutTake (recorder.take(), bounds, sr, level ? jmin (60.0f, 0.891f / bounds.peak) : 1.0f, ! exact);
        monoIfSame (cut);
        const int root = detectRoot (cut, sr);
        const auto folder = recordingsFolder();
        folder.createDirectory();
        String name = nextTakeName (folder);
        if (root >= 0) name << " " << noteName (root);
        const auto file = folder.getChildFile (name + ".wav");
        String err;
        SamplePtr s;
        if (! writeTake (file, cut, sr, root)) msg = "Couldn't save the recording in " + folder.getFullPathName();
        else if ((s = cache.get (file, err)) == nullptr) msg = err;
        else
        {
            const int pad = setup.pad;
            edit ([&] (Instrument& in)
            {
                Zone z;
                z.sample = s;
                if (setup.target == TakeTarget::Keys)
                {
                    z.root = root >= 0 ? root : 60;
                    in.keys.zones.push_back (z);
                    splitKeysByRoot (in.keys.zones);
                    if (in.keys.name.isEmpty()) in.keys.name = "Recorded";
                }
                else
                {
                    auto& p = in.pads[pad];
                    if (setup.target == TakeTarget::PadReplace || p.zones.empty()) { p.zones = { z }; p.name = name; }
                    else
                    {
                        p.zones.push_back (z);
                        std::vector<Zone*> layers;
                        for (auto& q : p.zones) { q.rrPos = 0; layers.push_back (&q); }
                        assignVelocityByLoudness (layers);
                    }
                }
            });
            placed = true;
            lastTake = s;
            const String secs = String ((double) cut.getNumSamples() / sr, 2) + " s";
            if (setup.target == TakeTarget::Keys)
            {
                lastSampledPad = -1;
                msg = name + " (" + secs + ") is on the Keys at " + noteName (root >= 0 ? root : 60) + (root >= 0 ? String() : String (" (no clear pitch)")) + ".";
            }
            else
            {
                lastSampledPad = pad;
                msg = name + " (" + secs + ") is on pad " + padLabel (pad) + (setup.target == TakeTarget::PadLayer ? " as a velocity layer." : ".");
            }
        }
    }
    recorder.clear();
    // "Keep going": wait for the next hit on the next pad.
    if (setup.keepGoing && placed && setup.target != TakeTarget::Keys && setup.pad + 1 < kPads)
    {
        rec.pad = setup.pad + 1;
        recorder.arm (rec.mode, rec.thresholdDb, rec.bars);
        msg << " Waiting for the next sound, for pad " << padLabel (rec.pad) << ".";
    }
    samplingMessage = msg;
    ++samplingSerial;
}

// ---- Auto Sampler -----------------------------------------------------------------------------
String OmniProcessor::startAutoSampler (const AutoSettings& a, bool useHosted)
{
    if (autoSampler.state() == AutoSampler::State::Running) return "The Auto Sampler is already running.";
    autoSampler.reset();
    if (autoNotes (a).empty()) return "Nothing to sample: check the note range.";
    auto base = File::createLegalFileName (a.name).trim();
    if (base.isEmpty()) base = "Auto Sampled";
    const auto folder = uniqueFile (autoSampledFolder(), base, {});
    if (! folder.createDirectory().wasOk()) return "Couldn't create " + folder.getFullPathName();
   #if OMNI_HOSTING
    if (useHosted)
    {
        if (hostedInst == nullptr) return "Choose a plug-in to sample first.";
        return autoSampler.startOffline (*hostedInst, hostLock, a, folder) ? String() : String ("Couldn't start the Auto Sampler.");
    }
   #else
    ignoreUnused (useHosted);
   #endif
    if (! hasAudioInput())
        return "This version has no audio input. Put Omnisampler FX in an effects slot after the instrument, or choose a plug-in to sample.";
    if (rateNow <= 0.0) return "The audio isn't running yet.";
    return autoSampler.startRealtime (a, rateNow, folder, midiOut.get()) ? String() : String ("Couldn't start the Auto Sampler.");
}

void OmniProcessor::cancelAutoSampler() { autoSampler.cancel(); }

// Builds the instrument from what the Auto Sampler captured, loads it and saves it next to the samples.
void OmniProcessor::finishAutoSample()
{
    const auto a = autoSampler.settings();
    const auto caps = autoSampler.results();
    const auto folder = autoSampler.folder();
    const int silent = autoSampler.silentNotes();
    auto base = File::createLegalFileName (a.name).trim();
    if (base.isEmpty()) base = "Auto Sampled";
    String msg;
    if (caps.empty())
        msg = "The Auto Sampler heard nothing. Check that the instrument answers on MIDI channel " + String (a.channel)
              + (autoSampler.isRealtime() ? " and that its sound reaches the input." : ".");
    else if (! a.toPads)
    {
        KeysMap km;
        km.voicing = current->keys.voicing;
        km.voicing.env.sustain = 1.0f;       // the samples carry their own shape
        km.output = current->keys.output;
        km.name = a.name;
        km.zones = autoZones (a, caps, cache, km.voicing.velSens);
        edit ([&] (Instrument& in) { in.keys = km; });
        String err;
        saveSfz (km, folder.getChildFile (base + ".sfz"), err);
        lastSampledPad = -1;
        msg = a.name + ": " + String ((int) km.zones.size()) + " samples are on the Keys. Saved in " + folder.getFullPathName();
    }
    else
    {
        // One pad per sampled note, lowest note on the first pad.
        const int first = jlimit (0, kPads - 1, a.firstPad);
        const float velSens = current->pads[first].voicing.velSens;
        const auto zones = autoZones (a, caps, cache, velSens);
        std::vector<int> roots;
        for (auto& z : zones) if (std::find (roots.begin(), roots.end(), z.root) == roots.end()) roots.push_back (z.root);
        std::sort (roots.begin(), roots.end());
        int used = 0;
        edit ([&] (Instrument& in)
        {
            for (size_t i = 0; i < roots.size() && first + (int) i < kPads; ++i, ++used)
            {
                auto& p = in.pads[first + (int) i];
                p.zones.clear();
                for (auto z : zones) if (z.root == roots[i]) { z.loKey = 0; z.hiKey = 127; p.zones.push_back (z); }
                p.note = roots[i];
                p.name = a.name + " " + noteName (roots[i]);
                p.oneShot = true;
                p.voicing.velSens = velSens;
            }
        });
        saveKit (folder.getChildFile (base + ".omni"));
        lastSampledPad = first;
        msg = a.name + ": " + String (used) + " pads filled from " + padLabel (first) + ". Saved in " + folder.getFullPathName();
        if (used < (int) roots.size()) msg << " (" << ((int) roots.size() - used) << " notes didn't fit on the pads)";
    }
    if (silent > 0 && ! caps.empty()) msg << "  " << silent << (silent == 1 ? " note was" : " notes were") << " silent and left out.";
   #if OMNI_HOSTING
    audition = false;    // the keyboard now plays the sampled copy
   #endif
    autoSampler.reset();
    samplingMessage = msg;
    ++samplingSerial;
}

// ---- MIDI output for the Auto Sampler ---------------------------------------------------------
#if JUCE_MAC || JUCE_LINUX || JUCE_BSD
 #define OMNI_VIRTUAL_MIDI 1    // a port of our own that other apps and DAWs see as a MIDI input
#else
 #define OMNI_VIRTUAL_MIDI 0
#endif

int OmniProcessor::virtualMidiChoice() const { return OMNI_VIRTUAL_MIDI ? 1 : -1; }

StringArray OmniProcessor::midiOutputChoices()
{
    StringArray s;
    s.add (fx ? "Plug-in MIDI out (route it in the host)" : "No MIDI device");
   #if OMNI_VIRTUAL_MIDI
    s.add ("Virtual port \"Omnisampler\"");
   #endif
    midiDevices = MidiOutput::getAvailableDevices();
    for (auto& d : midiDevices) s.add (d.name);
    return s;
}

void OmniProcessor::setMidiOutput (int index)
{
    if (autoSampler.state() == AutoSampler::State::Running) return;
    midiOut.reset();
    midiOutIndex = 0;
    int i = index - 1;
    if (i < 0) return;
   #if OMNI_VIRTUAL_MIDI
    if (i == 0) midiOut = MidiOutput::createNewDevice ("Omnisampler");
    --i;
   #endif
    if (midiOut == nullptr && isPositiveAndBelow (i, midiDevices.size())) midiOut = MidiOutput::openDevice (midiDevices[i].identifier);
    if (midiOut != nullptr) midiOutIndex = index;
}

// ---- a plug-in loaded inside Omnisampler, to be sampled ---------------------------------------
#if OMNI_HOSTING
// Newer JUCE (8.0.9 on) moved "add the plug-in formats this build supports" out of the manager class.
static void addPluginFormats (AudioPluginFormatManager& m)
{
   #if __has_include (<juce_audio_processors/format/juce_AudioPluginFormatManagerHelpers.h>)
    addDefaultFormatsToManager (m);
   #else
    m.addDefaultFormats();
   #endif
}

Array<OmniProcessor::PluginChoice> OmniProcessor::instrumentPlugins (bool rescan)
{
    if (! pluginList.isEmpty() && ! rescan) return pluginList;
    pluginList.clear();
    if (hostFormats.getNumFormats() == 0) addPluginFormats (hostFormats);
    for (auto* f : hostFormats.getFormats())
    {
        const bool au = f->getName() == "AudioUnit";
        for (auto& id : f->searchPathsForPlugins (f->getDefaultLocationsToSearch(), true, true))
        {
            if (au && ! id.startsWithIgnoreCase ("AudioUnit:Synths/")) continue;   // instruments only
            const auto name = au ? f->getNameOfPluginFromIdentifier (id) : File::createFileWithoutCheckingPath (id).getFileNameWithoutExtension();
            if (name.isEmpty() || name.startsWithIgnoreCase ("Omnisampler")) continue;
            pluginList.add ({ name, f->getName(), id });
        }
    }
    std::sort (pluginList.begin(), pluginList.end(), [] (const PluginChoice& a, const PluginChoice& b) { return a.name.compareIgnoreCase (b.name) < 0; });
    return pluginList;
}

void OmniProcessor::loadHosted (const PluginChoice& c, std::function<void (const String&)> done)
{
    if (autoSampler.state() == AutoSampler::State::Running) { done ("Stop the Auto Sampler first."); return; }
    if (hostFormats.getNumFormats() == 0) addPluginFormats (hostFormats);
    PluginDescription desc;
    desc.pluginFormatName = c.format; desc.fileOrIdentifier = c.id; desc.name = c.name; desc.isInstrument = true;
    if (c.format != "AudioUnit")
    {
        // One VST3 file can hold several plug-ins: take its first instrument.
        OwnedArray<PluginDescription> found;
        for (auto* f : hostFormats.getFormats()) if (f->getName() == c.format) f->findAllTypesForFile (found, c.id);
        const PluginDescription* pick = nullptr;
        for (auto* d : found) if (d->isInstrument) { pick = d; break; }
        if (pick == nullptr) { done (found.isEmpty() ? c.name + " couldn't be opened." : c.name + " isn't an instrument."); return; }
        desc = *pick;
    }
    const double sr = rateNow > 0.0 ? rateNow : 44100.0;
    const int block = blockNow;
    AudioPluginFormat* format = nullptr;
    for (auto* f : hostFormats.getFormats()) if (f->getName() == desc.pluginFormatName) format = f;
    if (format == nullptr) { done ("This build can't load " + c.format + " plug-ins."); return; }
    if (! format->requiresUnblockedMessageThreadDuringCreation (desc))
    {
        // Most plug-ins (VST3, Audio Unit v2) load straight away. Only the newer kind of Audio Unit has to load in the background.
        String err;
        auto inst = format->createInstanceFromDescription (desc, sr, block, err);
        if (inst == nullptr) { done (err.isNotEmpty() ? err : "Couldn't load " + c.name + "."); return; }
        setHosted (std::move (inst), c.name);
        done ({});
        return;
    }
    hostFormats.createPluginInstanceAsync (desc, sr, block, [this, stillHere = alive, name = c.name, done] (std::unique_ptr<AudioPluginInstance> inst, const String& err)
    {
        if (! stillHere->load()) return;
        if (inst == nullptr) { done (err.isNotEmpty() ? err : "Couldn't load " + name + "."); return; }
        setHosted (std::move (inst), name);
        done ({});
    });
}

void OmniProcessor::setHosted (std::unique_ptr<AudioPluginInstance> inst, const String& name)
{
    autoSampler.cancel();
    const double sr = rateNow > 0.0 ? rateNow : 44100.0;
    const int block = blockNow;
    if (inst != nullptr)
    {
        inst->setRateAndBufferSizeDetails (sr, block);
        inst->prepareToPlay (sr, block);
    }
    std::unique_ptr<AudioPluginInstance> old;
    {
        const SpinLock::ScopedLockType hl (hostLock);
        old = std::move (hostedInst);
        hostedInst = std::move (inst);
        if (hostedInst != nullptr)
            hostBuf.setSize (jmax (2, hostedInst->getTotalNumInputChannels(), hostedInst->getTotalNumOutputChannels()), jmax (16, block));
    }
    hostedName = hostedInst != nullptr ? name : String();
    audition = hostedInst != nullptr;
    if (old != nullptr) old->releaseResources();
}

void OmniProcessor::unloadHosted() { setHosted (nullptr, {}); }

// The loaded plug-in, played from the keyboard so its sound can be chosen and compared with the sampled copy.
void OmniProcessor::renderHosted (AudioBuffer<float>& out, int channel, int n)
{
    const SpinLock::ScopedTryLockType hl (hostLock);
    if (! hl.isLocked() || hostedInst == nullptr || n > hostBuf.getNumSamples() || n <= 0) return;
    AudioBuffer<float> view (hostBuf.getArrayOfWritePointers(), hostBuf.getNumChannels(), n);
    view.clear();
    hostedInst->processBlock (view, hostMidi);
    const int outs = hostedInst->getTotalNumOutputChannels();
    if (outs < 1) return;
    const int c = jlimit (0, jmax (0, out.getNumChannels() - 1), channel);
    out.addFrom (c, 0, view, 0, 0, n);
    if (c + 1 < out.getNumChannels()) out.addFrom (c + 1, 0, view, outs > 1 ? 1 : 0, 0, n);
}
#endif
