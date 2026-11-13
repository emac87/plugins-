#pragma once
// Sampling: recording from the audio input (Recorder), tidying a take (trim, level, loop, save),
// and the Auto Sampler, which plays a range of notes at several velocities, records each one and
// turns the lot into an instrument for the Keys or a kit for the Pads.
#include "Model.h"
#include "Formats.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>

namespace omni
{
    // Where recordings and auto-sampled instruments are kept. Omnisampler and Omnisampler FX share it,
    // so something sampled in the effect can be opened in the instrument.
    juce::File libraryFolder();
    juce::File recordingsFolder();     // libraryFolder()/Recordings
    juce::File autoSampledFolder();    // libraryFolder()/Auto Sampled
    juce::File uniqueFile (const juce::File& folder, const juce::String& base, const juce::String& ext);
    juce::String nextTakeName (const juce::File& folder);   // "Take 0007"
    void setLibraryFolder (const juce::File&);               // for the tests

    // ---- tidying a take -------------------------------------------------------------------
    struct TakeBounds { int start = 0, end = 0; float peak = 0.0f; bool silent = true; };
    // Finds the sound inside the first `frames` frames: start just before the onset, end after the tail.
    TakeBounds findTakeBounds (const juce::AudioBuffer<float>& b, int frames, double sr, bool trimStart, bool trimEnd);
    // Copies [start, end) with a gain and short fades at the trimmed ends.
    juce::AudioBuffer<float> cutTake (const juce::AudioBuffer<float>& b, const TakeBounds&, double sr, float gain, bool fades = true);
    // Looks for a loop inside [from, to) of a sustained note and crossfades the audio so it repeats
    // without a click. False when the sound doesn't hold steady enough to loop.
    bool findLoop (juce::AudioBuffer<float>& data, double sr, int from, int to, int& loopStart, int& loopEnd);
    // 24-bit WAV. root >= 0 and the loop are stored in the file's sampler chunk.
    bool writeTake (const juce::File& f, const juce::AudioBuffer<float>& b, double sr, int root, int loopStart = -1, int loopEnd = -1);
    // Pitch of a buffer as a MIDI note, -1 if unclear.
    int detectRoot (const juce::AudioBuffer<float>& b, double sr);
    // A stereo take whose two sides are the same is kept as mono (half the size).
    void monoIfSame (juce::AudioBuffer<float>& b);
    // Key ranges split halfway between the zones' root notes; zones sharing a root become velocity layers by loudness.
    void splitKeysByRoot (std::vector<Zone>& zones);

    // ---- recording from the input ----------------------------------------------------------
    // arm / stop / clear are called from the message thread, process from the audio thread.
    class Recorder
    {
    public:
        enum class Mode { Manual, Threshold, Bars };
        enum class State { Idle, Armed, Recording, Done };
        static constexpr int kMaxSeconds = 60;

        void prepare (double sampleRate);                    // drops any take in progress
        bool arm (Mode, float thresholdDb, int bars);        // false if a take is already under way
        void stop();                                         // recording: finish the take; armed: cancel
        void clear();                                        // Done -> Idle, once the take has been used
        State state() const noexcept { return st.load(); }
        Mode mode() const noexcept { return md; }
        int frames() const noexcept { return length.load(); }
        const juce::AudioBuffer<float>& take() const noexcept { return buf; }   // read it only while Done
        double rate() const noexcept { return sr; }
        float takeLevel() noexcept { return level.exchange (0.0f); }            // input peak since the last call

        struct Transport { bool valid = false, playing = false; double ppq = 0, barStartPpq = 0, bpm = 120, quartersPerBar = 4; };
        void process (const float* L, const float* R, int n, const Transport&);

    private:
        void append (const float* L, const float* R, int n);
        static constexpr int kPre = 4096;                    // ring of the newest input, so a take keeps its attack
        juce::AudioBuffer<float> buf, pre;
        double sr = 44100.0;
        std::atomic<State> st { State::Idle };
        std::atomic<int> length { 0 };
        std::atomic<bool> wantArm { false }, wantStop { false };
        std::atomic<float> level { 0.0f };
        Mode md = Mode::Manual;
        float threshold = 0.03f;
        int bars = 1, prePos = 0, preFilled = 0, quiet = 0, target = 0;
    };

    // ---- Auto Sampler ----------------------------------------------------------------------
    struct AutoSettings
    {
        int loNote = 36, hiNote = 84, step = 4;      // every `step` semitones from loNote up to hiNote
        int velLayers = 3, roundRobins = 1;
        float holdSecs = 2.0f, tailSecs = 1.5f;      // note held, then recorded on after the note-off
        int channel = 1;
        bool toPads = false;                         // false: an instrument for the Keys; true: one pad per note
        int firstPad = 0;
        bool loop = false, normalize = true;
        juce::String name { "Auto Sampled" };
    };
    struct AutoNote { int note = 60, vel = 127, layer = 0, rr = 0; };
    struct AutoCapture { AutoNote n; juce::File file; float peak = 0.0f; float fileGain = 1.0f; int loopStart = -1, loopEnd = -1; };

    std::vector<int> autoVelocities (int layers);                 // e.g. 3 -> 42, 85, 127
    std::vector<AutoNote> autoNotes (const AutoSettings&);        // every note, velocity and round robin, in playing order
    // The engine's velocity curve, so a sample played at the velocity it was recorded at comes out at the recorded level.
    inline float velocityGain (int vel, float velSens) { const float v = (float) vel / 127.0f; return 1.0f - velSens + velSens * v * v; }
    // Zones for the captured notes: key splits halfway between the sampled notes, one velocity range per
    // layer, round robins, and gains that undo the engine's own velocity curve at each sampled velocity.
    std::vector<Zone> autoZones (const AutoSettings&, const std::vector<AutoCapture>&, SampleCache&, float velSens);

    class AutoSampler : private juce::Thread
    {
    public:
        enum class State { Idle, Running, Finished, Cancelled };
        AutoSampler() : Thread ("Omnisampler auto sampler") {}
        ~AutoSampler() override { cancel(); }

        // Plays `synth` (a plug-in loaded inside Omnisampler) directly, faster than real time, on a
        // background thread. `lock` is held for the whole run so nothing else processes the plug-in.
        bool startOffline (juce::AudioProcessor& synth, juce::SpinLock& lock, const AutoSettings&, const juce::File& folder);
        // Real time: notes go out as MIDI (into process()'s buffer and to `device`, if any) and the
        // audio input is recorded. process() must then be called from the audio thread.
        bool startRealtime (const AutoSettings&, double sampleRate, const juce::File& folder, juce::MidiOutput* device);
        void cancel();                                // stops and waits; captured notes are kept on disk but not used
        void reset() { if (! isThreadRunning()) st = State::Idle; }
        void process (const float* L, const float* R, int n, juce::MidiBuffer& midiOut);

        State state() const noexcept { return st.load(); }
        bool isRealtime() const noexcept { return realtime; }
        int done() const noexcept { return doneCount.load(); }
        int total() const noexcept { return (int) notes.size(); }
        AutoNote current() const;                     // the note being sampled now
        int silentNotes() const noexcept { return silentCount.load(); }
        const AutoSettings& settings() const noexcept { return set; }
        const std::vector<AutoCapture>& results() const noexcept { return captured; }   // once Finished
        juce::File folder() const { return dir; }
        double estimatedSeconds() const;              // real-time runs only

    private:
        void run() override;
        void runOffline();
        void runRealtime();
        void keepNote (const AutoNote&, int frames);  // noteBuf[0..frames) -> trimmed file + result
        void sendToDevice (const juce::MidiMessage&);

        AutoSettings set;
        std::vector<AutoNote> notes;
        std::vector<AutoCapture> captured;
        juce::File dir;
        juce::AudioBuffer<float> noteBuf;
        double sr = 44100.0;
        std::atomic<State> st { State::Idle };
        std::atomic<int> doneCount { 0 }, silentCount { 0 };
        bool realtime = false;

        // offline
        juce::AudioProcessor* synth = nullptr;
        juce::SpinLock* synthLock = nullptr;

        // real time (audio thread owns phase / pos / index while rtActive)
        enum class Phase { Gap, Hold, Tail, Wait };
        std::atomic<bool> rtActive { false }, rtStop { false }, noteReady { false }, noteTaken { false };
        std::atomic<int> rtIndex { 0 }, readyFrames { 0 };
        Phase phase = Phase::Gap;
        int pos = 0, recLen = 0, gapFrames = 0, holdFrames = 0, tailFrames = 0;
        bool noteIsOn = false;
        juce::MidiOutput* device = nullptr;
        // audio thread -> worker thread: MIDI for the output device
        juce::AbstractFifo deviceFifo { 256 };
        struct RawMidi { juce::uint8 bytes[3]; };
        RawMidi deviceQueue[256];
    };
}
