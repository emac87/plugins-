#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include "Model.h"
#include "Formats.h"
#include "Engine.h"
#include "Streamer.h"
#include "MoreFormats.h"
#include "Capture.h"

#ifndef OMNI_FX
 #define OMNI_FX 0     // 1 = this build is Omnisampler FX, the version for an effects slot
#endif
// Loading another instrument plug-in inside Omnisampler, for the Auto Sampler (Mac and Windows).
#if (JUCE_PLUGINHOST_VST3 || JUCE_PLUGINHOST_AU) && ! JUCE_IOS && ! JUCE_ANDROID
 #define OMNI_HOSTING 1
#else
 #define OMNI_HOSTING 0
#endif

class OmniProcessor : public juce::AudioProcessor, private juce::Timer
{
public:
    // fx = Omnisampler FX: has an audio input, passes it through and can sample from it.
    explicit OmniProcessor (bool fx = OMNI_FX != 0);
    ~OmniProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return fx ? "Omnisampler FX" : "Omnisampler"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return fx; }   // the Auto Sampler's notes
    double getTailLengthSeconds() const override { return 2.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // ---- message thread ----
    juce::AudioProcessorValueTreeState apvts;
    juce::MidiKeyboardState keyboardState;   // on-screen keyboard (Keys only); also lights keys played over MIDI
    juce::MidiKeyboardState padState;        // on-screen pads (Pads only)
    omni::SampleCache cache;
    omni::Streamer streamer;
    omni::Engine engine;

    omni::InstrumentPtr getInstrument() const { return current; }
    void edit (const std::function<void (omni::Instrument&)>& fn);   // copy, change, swap in
    void replaceInstrument (omni::Instrument);

    // Loading. Each returns a message for the user ("" = fine).
    juce::String loadFilesToPad (int pad, const juce::Array<juce::File>& files, omni::DropMode mode);
    juce::String loadFilesToKeys (const juce::Array<juce::File>& files, bool replace);
    juce::String loadInstrumentFile (const juce::File& f);   // .sfz / .dspreset / .dslibrary / .sf2 / .exs / .adv / .nki / .zip into Keys
    juce::String saveKeysAsDecentSampler (const juce::File& f);
    static bool canLoadInstrument (const juce::File& f) { return omni::isInstrumentFile (f) || omni::isMoreFormatsFile (f); }
    static juce::String instrumentPatterns() { return "*.sfz;*.dspreset;*.dslibrary;*.sf2;*.exs;*.adv;*.nki;*.zip"; }

    // Disk streaming of long samples (applies to samples loaded after the change).
    void setStreaming (bool on) { cache.streaming = on; }
    bool isStreaming() const { return cache.streaming.load(); }
    int streamUnderruns() const { return streamer.underruns.load(); }
    static juce::String outputName (int i) { return i == 0 ? juce::String ("Main") : "Out " + juce::String (i + 1); }
    juce::String loadKit (const juce::File& f);
    juce::String saveKit (const juce::File& f);

    int padsChannel() const { return (int) apvts.getRawParameterValue ("padsch")->load(); }
    int keysChannel() const { return (int) apvts.getRawParameterValue ("keysch")->load(); }
    std::atomic<int> voiceCount { 0 };
    // On-screen pads and keys: true = how hard you hit depends on where you tap; false = fixed velocity.
    std::atomic<bool> touchVelocity { false };
    bool hasOutputPicker() const { return wrapperType != wrapperType_Standalone && ! fx; }

    // ---- sampling ----------------------------------------------------------------------------
    const bool fx;
    // The Sample page (recording and the Auto Sampler). Not on iPhone/iPad yet.
    static bool canSample()
    {
       #if JUCE_IOS || JUCE_ANDROID
        return false;
       #else
        return true;
       #endif
    }
    bool hasAudioInput() const { return getBusCount (true) > 0; }   // Omnisampler FX, and the standalone app
    std::atomic<bool> monitorInput;                                 // pass the input through to the output

    // Recording from the input. The finished take is trimmed, saved in the library and put on a pad or the keys.
    enum class TakeTarget { PadReplace, PadLayer, Keys };
    struct RecordSetup
    {
        omni::Recorder::Mode mode = omni::Recorder::Mode::Manual;
        float thresholdDb = -30.0f;
        int bars = 1;
        bool trim = true, normalize = true;
        bool keepGoing = false;          // "start on sound" only: move to the next pad and wait for the next hit
        TakeTarget target = TakeTarget::PadReplace;
        int pad = 0;
    };
    bool startRecording (const RecordSetup&);
    void stopRecording();                // finishes the take; while waiting to start, cancels
    const RecordSetup& recordSetup() const { return rec; }
    omni::Recorder recorder;

    // Auto Sampler. useHosted: play the plug-in loaded with loadHosted (rendered faster than real time).
    // Otherwise the notes go out as MIDI and the audio input is recorded.
    juce::String startAutoSampler (const omni::AutoSettings&, bool useHosted);   // "" = started
    void cancelAutoSampler();
    omni::AutoSampler autoSampler;
    juce::StringArray midiOutputChoices();     // where the Auto Sampler's notes go, besides the plug-in's own MIDI out
    void setMidiOutput (int index);            // an index into midiOutputChoices()
    int midiOutputIndex() const { return midiOutIndex; }

    // Files a finished take or Auto Sampler run. Runs on the processor's timer; the tests call it directly.
    void pollSampling();
    std::atomic<int> samplingSerial { 0 };     // goes up whenever sampling has changed the instrument or has news
    juce::String samplingMessage;              // the latest result, for the Sample page
    int lastSampledPad = -1;                   // where the last take or run landed (-1 = the Keys)
    omni::SamplePtr lastTake;                  // the last recording, for the Sample page's waveform
    int virtualMidiChoice() const;             // index of the "Virtual port" entry in midiOutputChoices(), -1 if there is none

   #if OMNI_HOSTING
    struct PluginChoice { juce::String name, format, id; };
    juce::Array<PluginChoice> instrumentPlugins (bool rescan = false);   // Audio Unit instruments and VST3s on this computer
    void loadHosted (const PluginChoice&, std::function<void (const juce::String& error)> done);
    void unloadHosted();                       // close its window first
    juce::AudioPluginInstance* hosted() const { return hostedInst.get(); }
    juce::String hostedName;
    std::atomic<bool> audition { false };      // the keyboard and Keys MIDI play the loaded plug-in instead of the Keys
   #endif

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    static BusesProperties makeBuses (bool fx);
    void timerCallback() override;
    void swapIn (omni::InstrumentPtr p);
    void finishTake();
    void finishAutoSample();

    omni::InstrumentPtr current;
    juce::SpinLock instLock;
    std::vector<std::pair<omni::InstrumentPtr, juce::uint32>> graveyard;
    juce::CriticalSection graveLock;
    juce::SmoothedValue<float> master { 1.0f };
    juce::MidiBuffer localPads, localKeys;   // taps on the on-screen pads / keyboard for this block
    int timerTicks = 0;
    double rateNow = 0.0;                    // from the last prepareToPlay (0 = audio not set up yet)
    int blockNow = 512;

    RecordSetup rec;
    juce::AudioBuffer<float> inBuf;          // this block's input, kept while the buffer is reused for output
    juce::MidiBuffer autoMidi;               // the Auto Sampler's notes for this block
    std::unique_ptr<juce::MidiOutput> midiOut;
    juce::Array<juce::MidiDeviceInfo> midiDevices;
    int midiOutIndex = 0;

   #if OMNI_HOSTING
    void setHosted (std::unique_ptr<juce::AudioPluginInstance>, const juce::String& name);
    void renderHosted (juce::AudioBuffer<float>& out, int channel, int n);
    juce::AudioPluginFormatManager hostFormats;
    juce::Array<PluginChoice> pluginList;
    std::unique_ptr<juce::AudioPluginInstance> hostedInst;
    juce::SpinLock hostLock;                 // whoever is processing the loaded plug-in holds this
    juce::AudioBuffer<float> hostBuf;
    juce::MidiBuffer hostMidi;
    std::shared_ptr<std::atomic<bool>> alive { std::make_shared<std::atomic<bool>> (true) };
   #endif

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OmniProcessor)
};
