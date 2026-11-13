#pragma once
// Sampling: Omnisampler FX recording from its audio input (manual, start-on-sound, bars), takes landing
// on pads and keys, and the Auto Sampler (a synth played directly, and the real-time MIDI-out /
// audio-in loop). Header-only; uses CHECK from TestCommon.h.
#include "TestCommon.h"
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cmath>
#include <functional>
#include <thread>

namespace sampling_test
{
    using namespace omni;
    constexpr double kSr = 44100.0;

    inline juce::AudioBuffer<float> silence (double secs) { juce::AudioBuffer<float> b (1, (int) (kSr * secs)); b.clear(); return b; }

    // Adds a decaying sine burst to `b`, starting `at` seconds in.
    inline void addBurst (juce::AudioBuffer<float>& b, double at, double freq, float amp, double len)
    {
        const int start = (int) (at * kSr), n = (int) (len * kSr);
        for (int i = 0; i < n && start + i < b.getNumSamples(); ++i)
            b.addSample (0, start + i, amp * (float) (std::sin (2.0 * juce::MathConstants<double>::pi * freq * i / kSr) * std::exp (-8.0 * i / n)));
    }

    // Runs `in` (mono) through the processor block by block and returns its stereo output.
    inline juce::AudioBuffer<float> run (OmniProcessor& p, const juce::AudioBuffer<float>& in, const std::function<void()>& afterBlock = {},
                                         juce::MidiBuffer* midiOut = nullptr, int block = 512)
    {
        juce::AudioBuffer<float> out (2, in.getNumSamples());
        for (int pos = 0; pos < in.getNumSamples(); pos += block)
        {
            const int n = juce::jmin (block, in.getNumSamples() - pos);
            juce::AudioBuffer<float> b (2, n);
            b.copyFrom (0, 0, in, 0, pos, n);
            b.copyFrom (1, 0, in, 0, pos, n);
            juce::MidiBuffer m;
            p.processBlock (b, m);
            if (midiOut != nullptr) midiOut->addEvents (m, 0, n, pos);
            out.copyFrom (0, pos, b, 0, 0, n);
            out.copyFrom (1, pos, b, 1, 0, n);
            if (afterBlock) afterBlock();
        }
        return out;
    }

    // Plays one note on a processor with no input and returns `secs` of its left channel.
    inline juce::AudioBuffer<float> playNote (OmniProcessor& p, int ch, int note, int vel, double secs, double holdSecs)
    {
        juce::AudioBuffer<float> out (1, (int) (secs * kSr));
        const int hold = (int) (holdSecs * kSr);
        bool on = false, off = false;
        for (int pos = 0; pos < out.getNumSamples(); pos += 512)
        {
            const int n = juce::jmin (512, out.getNumSamples() - pos);
            juce::AudioBuffer<float> b (2, n);
            b.clear();
            juce::MidiBuffer m;
            if (! on) { m.addEvent (juce::MidiMessage::noteOn (ch, note, (juce::uint8) vel), 0); on = true; }
            if (! off && pos + n > hold) { m.addEvent (juce::MidiMessage::noteOff (ch, note), juce::jlimit (0, n - 1, hold - pos)); off = true; }
            p.processBlock (b, m);
            out.copyFrom (0, pos, b, 0, 0, n);
        }
        // let it die away before the next note
        juce::AudioBuffer<float> rest (2, 44100); juce::MidiBuffer none; rest.clear(); p.processBlock (rest, none);
        return out;
    }

    inline double freqOf (const juce::AudioBuffer<float>& b, int start, int n)
    {
        int zc = 0;
        for (int i = start + 1; i < start + n; ++i) if ((b.getSample (0, i - 1) < 0) != (b.getSample (0, i) < 0)) ++zc;
        return zc / 2.0 / (n / kSr);
    }

    // A synth to sample: one steady, looped sine on the keys, so every key and velocity sounds and holds.
    inline void makeSynth (OmniProcessor& synth, const juce::File& dir)
    {
        juce::AudioBuffer<float> b (1, 44100);
        for (int i = 0; i < b.getNumSamples(); ++i) b.setSample (0, i, 0.4f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * i / 168.0));   // 262.5 Hz, a whole number of frames per cycle
        auto f = dir.getChildFile ("steady_C4.wav");
        writeWav (f, b, kSr);
        synth.prepareToPlay (kSr, 512);
        synth.loadFilesToKeys ({ f }, true);
        synth.edit ([] (Instrument& in)
        {
            auto& z = in.keys.zones[0];
            z.root = 60; z.loKey = 0; z.hiKey = 127; z.loop = true; z.loopStart = 16800; z.loopEnd = 33600;
            in.keys.voicing.env = { 0.002f, 0.1f, 1.0f, 0.05f };
        });
    }

    struct Head : juce::AudioPlayHead
    {
        double ppq = 0.0, bpm = 120.0;
        juce::Optional<PositionInfo> getPosition() const override
        {
            PositionInfo i;
            i.setBpm (bpm); i.setPpqPosition (ppq); i.setIsPlaying (true);
            i.setTimeSignature (TimeSignature { 4, 4 });
            i.setPpqPositionOfLastBarStart (std::floor (ppq / 4.0) * 4.0);
            return i;
        }
    };
}

inline void testSampling (const juce::File& dir, const juce::File& out)
{
    using namespace sampling_test;
    using Setup = OmniProcessor::RecordSetup;
    using Target = OmniProcessor::TakeTarget;
    dir.createDirectory();
    setLibraryFolder (dir.getChildFile ("library"));

    CHECK (autoVelocities (1) == std::vector<int> ({ 127 }) && autoVelocities (3) == std::vector<int> ({ 42, 85, 127 }), "velocity layers are spread evenly up to 127");
    { AutoSettings a; a.loNote = 36; a.hiNote = 60; a.step = 12; a.velLayers = 2; a.roundRobins = 3;
      CHECK (autoNotes (a).size() == 3 * 2 * 3, "the Auto Sampler plays every note at every velocity and round robin"); }

    // ---- Omnisampler FX: pass-through and recording --------------------------------------------
    OmniProcessor inst;
    CHECK (! inst.hasAudioInput() && ! inst.producesMidi() && inst.getName() == "Omnisampler", "the instrument keeps its layout: no audio input");
    OmniProcessor fx (true);
    CHECK (fx.hasAudioInput() && fx.producesMidi() && fx.getName() == "Omnisampler FX" && fx.getBusCount (false) == 1, "Omnisampler FX is a stereo effect with a MIDI output");
    fx.prepareToPlay (kSr, 512);
    {
        auto in = silence (1.0); addBurst (in, 0.2, 330.0, 0.5f, 0.5);
        const auto o = run (fx, in);
        float diff = 0.0f;
        for (int i = 0; i < in.getNumSamples(); ++i) diff = juce::jmax (diff, std::abs (o.getSample (0, i) - in.getSample (0, i)), std::abs (o.getSample (1, i) - in.getSample (0, i)));
        CHECK (diff < 1.0e-6f, "audio passes through Omnisampler FX untouched");
    }
    auto record = [&fx] (const Setup& s, const juce::AudioBuffer<float>& in)
    {
        const bool armed = fx.startRecording (s);
        run (fx, in);
        fx.stopRecording();
        run (fx, silence (0.05));
        fx.pollSampling();
        return armed;
    };
    {
        Setup s; s.pad = 3;
        auto in = silence (2.0); addBurst (in, 0.5, 220.0, 0.25f, 0.6);
        CHECK (record (s, in), "recording starts on Omnisampler FX");
        const auto& pad = fx.getInstrument()->pads[3];
        CHECK (pad.zones.size() == 1, "a take lands on the chosen pad");
        if (pad.zones.size() == 1)
        {
            const auto smp = pad.zones[0].sample;
            std::printf ("    take: %s, %d frames, peak %.2f dB\n", smp->file.getFileName().toRawUTF8(), smp->length(), smp->peakDb);
            CHECK (smp->length() > (int) (kSr * 0.3) && smp->length() < (int) (kSr * 0.75), "the silence is trimmed off both ends");
            CHECK (std::abs (smp->peakDb + 1.0f) < 0.3f, "the take is brought up to -1 dB");
            CHECK (smp->file.existsAsFile() && smp->file.isAChildOf (recordingsFolder()) && smp->file.getFileName().contains ("A3"), "it is saved in the library, named after the note it heard");
            juce::MidiBuffer m; m.addEvent (juce::MidiMessage::noteOn (10, 39, (juce::uint8) 127), 0);
            juce::AudioBuffer<float> b (2, 8192); b.clear(); fx.processBlock (b, m);
            CHECK (std::abs (freqOf (b, 400, 4096) - 220.0) < 12.0, "the pad plays the recording back");
            juce::AudioBuffer<float> rest (2, 44100); juce::MidiBuffer none; rest.clear(); fx.processBlock (rest, none);
        }
        CHECK (! inst.startRecording (s), "the instrument plug-in, with no input, refuses to record");
    }
    // Start on sound, stop on silence, keep going: three hits fill three pads hands-free.
    {
        Setup s; s.mode = Recorder::Mode::Threshold; s.thresholdDb = -30.0f; s.keepGoing = true; s.pad = 8;
        auto in = silence (5.0);
        addBurst (in, 0.3, 150.0, 0.5f, 0.3); addBurst (in, 1.8, 300.0, 0.5f, 0.3); addBurst (in, 3.3, 600.0, 0.5f, 0.3);
        fx.startRecording (s);
        run (fx, in, [&fx] { fx.pollSampling(); });
        const auto i8 = fx.getInstrument();
        bool three = true, attack = true;
        for (int k = 0; k < 3; ++k)
        {
            const auto& z = i8->pads[8 + k].zones;
            three = three && z.size() == 1;
            if (z.size() != 1) continue;
            const auto& d = z[0].sample->data;
            std::printf ("    pad %d: %d frames\n", 9 + k, d.getNumSamples());
            three = three && d.getNumSamples() > 2000 && d.getNumSamples() < (int) (kSr * 0.6);
            attack = attack && d.getMagnitude (0, 0, juce::jmin (d.getNumSamples(), 1200)) > 0.5f * d.getMagnitude (0, 0, d.getNumSamples());
        }
        CHECK (three, "start-on-sound records three hits onto three pads in a row");
        CHECK (attack, "each take keeps its attack");
        CHECK (fx.recorder.state() == Recorder::State::Armed && fx.recordSetup().pad == 11, "it then waits for the next sound, on the next pad");
        fx.stopRecording(); run (fx, silence (0.05)); fx.pollSampling();
        CHECK (fx.recorder.state() == Recorder::State::Idle, "stopping while it waits cancels cleanly");
    }
    // Soft then hard onto one pad: two velocity layers, sorted by loudness.
    {
        Setup s; s.pad = 20; s.target = Target::PadLayer;
        auto soft = silence (1.0); addBurst (soft, 0.2, 200.0, 0.04f, 0.4);
        auto hard = silence (1.0); addBurst (hard, 0.2, 200.0, 0.6f, 0.4);
        record (s, hard); record (s, soft);
        const auto& z = fx.getInstrument()->pads[20].zones;
        bool ok = z.size() == 2;
        if (ok)
        {
            const auto& quiet = z[0].sample->loudDb < z[1].sample->loudDb ? z[0] : z[1];
            const auto& loud = z[0].sample->loudDb < z[1].sample->loudDb ? z[1] : z[0];
            ok = quiet.loVel == 1 && quiet.hiVel < 127 && loud.loVel == quiet.hiVel + 1 && loud.hiVel == 127 && loud.sample->peakDb - quiet.sample->peakDb > 12.0f;
        }
        CHECK (ok, "two takes on one pad become soft and hard velocity layers");
    }
    // Onto the keys at the detected note.
    {
        Setup s; s.target = Target::Keys;
        auto a = silence (1.2); addBurst (a, 0.2, 220.0, 0.4f, 0.8);
        auto b = silence (1.2); addBurst (b, 0.2, 440.0, 0.4f, 0.8);
        record (s, a); record (s, b);
        const auto& z = fx.getInstrument()->keys.zones;
        bool ok = z.size() == 2;
        if (ok)
        {
            const auto& lo = z[0].root < z[1].root ? z[0] : z[1];
            const auto& hi = z[0].root < z[1].root ? z[1] : z[0];
            std::printf ("    keys: root %d keys %d-%d, root %d keys %d-%d\n", lo.root, lo.loKey, lo.hiKey, hi.root, hi.loKey, hi.hiKey);
            ok = lo.root == 57 && hi.root == 69 && lo.loKey == 0 && lo.hiKey == 63 && hi.loKey == 64 && hi.hiKey == 127;
        }
        CHECK (ok, "takes sent to the Keys sit at the notes they were heard at, split halfway");
    }
    // Bars: starts on the host's next bar line and is exactly that long.
    {
        Head head; head.ppq = 1.0;   // one beat into a bar, 120 bpm: the next bar is 66150 frames away, a bar is 88200
        fx.setPlayHead (&head);
        juce::AudioBuffer<float> in (1, (int) (kSr * 5.0));
        for (int i = 0; i < in.getNumSamples(); ++i) in.setSample (0, i, 0.3f * (float) std::sin (i * 0.05) * (float) std::sin (i * 0.00037));
        Setup s; s.mode = Recorder::Mode::Bars; s.bars = 1; s.pad = 30; s.normalize = false;
        fx.startRecording (s);
        run (fx, in, [&] { head.ppq += 512.0 * head.bpm / (60.0 * kSr); fx.pollSampling(); });
        fx.setPlayHead (nullptr);
        const auto& z = fx.getInstrument()->pads[30].zones;
        bool ok = z.size() == 1 && z[0].sample->length() == 88200;
        float diff = 0.0f;
        if (ok) for (int i = 0; i < 88200; ++i) diff = juce::jmax (diff, std::abs (z[0].sample->data.getSample (0, i) - in.getSample (0, 66150 + i)));
        std::printf ("    bar take: %d frames, max difference from the input %.6f\n", z.size() == 1 ? z[0].sample->length() : 0, diff);
        CHECK (ok && diff < 1.0e-4f, "a one-bar take starts on the bar line and is exactly one bar long");
    }
    {
        fx.monitorInput = false;
        juce::MemoryBlock mb; fx.getStateInformation (mb);
        OmniProcessor again (true); again.setStateInformation (mb.getData(), (int) mb.getSize());
        CHECK (! again.monitorInput.load() && again.getInstrument()->pads[3].zones.size() == 1, "the effect's session keeps its recordings and the pass-through setting");
        auto in = silence (0.2); addBurst (in, 0.0, 500.0, 0.5f, 0.2);
        again.prepareToPlay (kSr, 512);
        CHECK (run (again, in).getMagnitude (0, in.getNumSamples()) < 1.0e-6f, "with pass-through off, the input is not heard");
        fx.monitorInput = true;
    }

    // ---- Auto Sampler: a synth played directly, faster than real time --------------------------
    OmniProcessor synth;
    makeSynth (synth, dir);
    auto waitFor = [] (OmniProcessor& p)
    {
        for (int i = 0; i < 6000 && p.autoSampler.state() == AutoSampler::State::Running; ++i) juce::Thread::sleep (5);
        p.pollSampling();
    };
    {
        OmniProcessor dst; dst.prepareToPlay (kSr, 512);
        juce::SpinLock lock;
        AutoSettings a; a.loNote = 48; a.hiNote = 72; a.step = 12; a.velLayers = 2; a.holdSecs = 0.5f; a.tailSecs = 0.4f; a.normalize = false; a.name = "Test Synth";
        const auto folder = autoSampledFolder().getChildFile ("Test Synth");
        CHECK (dst.autoSampler.startOffline (synth, lock, a, folder), "the Auto Sampler starts on a synth");
        waitFor (dst);
        const auto in = dst.getInstrument();
        const auto& z = in->keys.zones;
        std::printf ("    %s\n", dst.samplingMessage.toRawUTF8());
        for (auto& q : z) std::printf ("      %-28s root %d keys %d-%d vel %d-%d gain %.1f dB\n", q.sample->file.getFileName().toRawUTF8(), q.root, q.loKey, q.hiKey, q.loVel, q.hiVel, q.gainDb);
        CHECK (z.size() == 6, "three notes at two velocities make six zones");
        bool map = z.size() == 6, pitch = true;
        for (auto& q : z)
        {
            map = map && (q.root == 48 ? (q.loKey == 0 && q.hiKey == 54) : q.root == 60 ? (q.loKey == 55 && q.hiKey == 66) : (q.root == 72 && q.loKey == 67 && q.hiKey == 127));
            map = map && ((q.loVel == 1 && q.hiVel == 64) || (q.loVel == 65 && q.hiVel == 127));
            pitch = pitch && detectRoot (*q.sample) == q.root;
        }
        CHECK (map, "keys split halfway between the sampled notes, one velocity range per layer");
        CHECK (pitch, "every sample holds the note it is mapped to");
        CHECK (folder.getChildFile ("Test Synth.sfz").existsAsFile() && folder.findChildFiles (juce::File::findFiles, false, "*.wav").size() == 6, "the instrument and its samples are saved in the library");
        // The copy should sound like the synth: same level at the sampled velocities, right pitch in between.
        bool level = true;
        for (int vel : { 127, 64 })
        {
            const auto want = playNote (synth, 1, 60, vel, 0.4, 0.3), got = playNote (dst, 1, 60, vel, 0.4, 0.3);
            const double a0 = rms (want, 0, 2000, 8000), a1 = rms (got, 0, 2000, 8000);
            std::printf ("    C4 at velocity %d: synth rms %.4f, sampled copy %.4f\n", vel, a0, a1);
            level = level && a0 > 0.001 && std::abs (20.0 * std::log10 (a1 / a0)) < 0.7;
        }
        CHECK (level, "the sampled copy plays at the synth's level at each sampled velocity");
        const auto d4 = playNote (dst, 1, 62, 100, 0.4, 0.3);
        CHECK (std::abs (freqOf (d4, 2000, 8000) - 262.5 * std::pow (2.0, 2.0 / 12.0)) < 6.0, "notes between the sampled ones are repitched from the nearest");
        OmniProcessor reload; reload.prepareToPlay (kSr, 512);
        CHECK (reload.loadInstrumentFile (folder.getChildFile ("Test Synth.sfz")).isEmpty() && reload.getInstrument()->keys.zones.size() == 6, "the saved instrument opens again");
    }
    // Loop the held part: a long note keeps sounding past the end of the sample, with no click.
    {
        OmniProcessor dst; dst.prepareToPlay (kSr, 512);
        juce::SpinLock lock;
        AutoSettings a; a.loNote = 60; a.hiNote = 60; a.velLayers = 1; a.holdSecs = 1.0f; a.tailSecs = 0.3f; a.loop = true; a.normalize = false; a.name = "Looped";
        dst.autoSampler.startOffline (synth, lock, a, autoSampledFolder().getChildFile ("Looped"));
        waitFor (dst);
        const auto& z = dst.getInstrument()->keys.zones;
        const bool looped = z.size() == 1 && z[0].loop && z[0].loopEnd > z[0].loopStart + 1000;
        if (z.size() == 1) std::printf ("    loop %d to %d of %d frames\n", z[0].loopStart, z[0].loopEnd, z[0].sample->length());
        CHECK (looped, "a held note gets a loop");
        if (looped)
        {
            const auto o = playNote (dst, 1, 60, 127, 3.0, 2.9);
            const double early = rms (o, 0, 10000, 10000), late = rms (o, 0, 110000, 10000);
            float step = 0.0f;
            for (int i = 4411; i < 120000; ++i) step = juce::jmax (step, std::abs (o.getSample (0, i) - o.getSample (0, i - 1)));
            const float peak = o.getMagnitude (0, 4410, 115000);
            const float sineStep = peak * (float) (2.0 * juce::MathConstants<double>::pi * 262.5 / kSr);
            std::printf ("    looped note: rms %.4f early, %.4f at 2.5 s; biggest step %.5f (a clean sine: %.5f)\n", early, late, step, sineStep);
            CHECK (late > early * 0.7 && step < sineStep * 1.6f, "the loop holds the note with no click");
        }
    }

    // ---- Auto Sampler in real time: notes out as MIDI, the sound back in through the input ------
    {
        OmniProcessor fx2 (true); fx2.prepareToPlay (kSr, 512);
        fx2.monitorInput = false;
        AutoSettings a; a.loNote = 48; a.hiNote = 72; a.step = 12; a.velLayers = 2; a.holdSecs = 0.4f; a.tailSecs = 0.3f; a.name = "Hardware"; a.toPads = true; a.firstPad = 16;
        CHECK (fx2.startAutoSampler (a, false).isEmpty(), "the real-time Auto Sampler starts on Omnisampler FX");
        juce::AudioBuffer<float> loopback (2, 512); loopback.clear();
        juce::MidiBuffer sent;
        int ons = 0, offs = 0;
        for (int blockNo = 0; blockNo < 400000 && fx2.autoSampler.state() == AutoSampler::State::Running; ++blockNo)
        {
            // The effect hears the synth one block late, as it would through an audio interface.
            juce::AudioBuffer<float> b (2, 512);
            b.makeCopyOf (loopback);
            juce::MidiBuffer m;
            fx2.processBlock (b, m);
            for (const auto meta : m) { if (meta.getMessage().isNoteOn()) ++ons; else if (meta.getMessage().isNoteOff()) ++offs; }
            loopback.clear();
            synth.processBlock (loopback, m);
            if (m.isEmpty() && blockNo % 64 == 0) juce::Thread::sleep (1);   // give the saving thread a turn
        }
        fx2.pollSampling();
        std::printf ("    %s\n", fx2.samplingMessage.toRawUTF8());
        CHECK (ons == 6 && offs == 6, "six notes went out as MIDI, each with its note-off");
        const auto in = fx2.getInstrument();
        bool pads = true, onset = true;
        const int want[3] = { 48, 60, 72 };
        for (int k = 0; k < 3; ++k)
        {
            const auto& p = in->pads[16 + k];
            pads = pads && p.note == want[k] && p.zones.size() == 2;
            for (auto& q : p.zones)
            {
                pads = pads && detectRoot (*q.sample) == want[k];
                const auto& d = q.sample->data;
                onset = onset && d.getMagnitude (0, 0, juce::jmin (d.getNumSamples(), 441)) > 0.2f * d.getMagnitude (0, 0, d.getNumSamples());
            }
        }
        CHECK (pads, "sampling to the pads gives one pad per note, each with its velocity layers");
        CHECK (onset, "the round-trip delay is trimmed, so every sample starts on its attack");
        CHECK (fx2.autoSampler.folder().getChildFile ("Hardware.omni").existsAsFile(), "the kit is saved in the library");

        // Cancelling stops the notes and leaves the instrument alone.
        const auto before = fx2.getInstrument();
        a.name = "Cancelled"; a.toPads = false;
        fx2.startAutoSampler (a, false);
        juce::MidiBuffer m2;
        for (int i = 0; i < 40; ++i) { juce::AudioBuffer<float> b (2, 512); b.clear(); juce::MidiBuffer m; fx2.processBlock (b, m); m2.addEvents (m, 0, 512, i * 512); }
        std::thread stopper ([&fx2] { fx2.cancelAutoSampler(); });
        bool sawOff = false;
        for (int i = 0; i < 2000 && fx2.autoSampler.state() == AutoSampler::State::Running; ++i)
        {
            juce::AudioBuffer<float> b (2, 512); b.clear(); juce::MidiBuffer m; fx2.processBlock (b, m);
            for (const auto meta : m) sawOff = sawOff || meta.getMessage().isNoteOff();
            juce::Thread::sleep (1);
        }
        stopper.join();
        fx2.pollSampling();
        CHECK (sawOff && fx2.autoSampler.state() == AutoSampler::State::Idle && fx2.getInstrument() == before, "cancelling sends the note-off and changes nothing");
    }

    // Optional: a real plug-in loaded inside Omnisampler and sampled (set OMNI_HOST_VST3 to an Omnisampler.vst3;
    // it is given the test synth's state so it has something to play).
   #if OMNI_HOSTING
    if (const auto path = juce::SystemStats::getEnvironmentVariable ("OMNI_HOST_VST3", {}); path.isNotEmpty())
    {
        OmniProcessor h; h.prepareToPlay (kSr, 512);
        std::printf ("    plug-ins found on this machine: %d\n", h.instrumentPlugins().size());
        bool done = false; juce::String err;
        // Loading is asynchronous: run the message loop until the plug-in has loaded (or 20 s have passed).
        h.loadHosted ({ "Hosted Omnisampler", "VST3", path }, [&] (const juce::String& e) { done = true; err = e; juce::MessageManager::getInstance()->stopDispatchLoop(); });
        juce::Timer::callAfterDelay (20000, [] { juce::MessageManager::getInstance()->stopDispatchLoop(); });
        juce::MessageManager::getInstance()->runDispatchLoop();
        std::printf ("    loadHosted: %s %s\n", done ? "called back" : "no callback", err.toRawUTF8());
        CHECK (done && err.isEmpty() && h.hosted() != nullptr, "a VST3 instrument loads inside Omnisampler");
        if (h.hosted() != nullptr)
        {
            // Hand it the test synth's state, wrapped the way JUCE's VST3 host stores a plug-in's state.
            juce::MemoryBlock raw, mb; synth.getStateInformation (raw);
            juce::XmlElement wrap ("VST3PluginState");
            wrap.createNewChildElement ("IComponent")->addTextElement (raw.toBase64Encoding());
            juce::AudioProcessor::copyXmlToBinary (wrap, mb);
            h.hosted()->setStateInformation (mb.getData(), (int) mb.getSize());
            // Auditioning: the keyboard's notes reach the loaded plug-in and its sound comes out of Omnisampler.
            const auto heard = playNote (h, 1, 60, 127, 0.4, 0.3);
            std::printf ("    audition: rms %.4f at %.1f Hz\n", rms (heard, 0, 2000, 8000), freqOf (heard, 2000, 8000));
            CHECK (h.audition.load() && rms (heard, 0, 2000, 8000) > 0.05 && std::abs (freqOf (heard, 2000, 8000) - 262.5) < 6.0, "the keyboard plays the loaded plug-in");
            AutoSettings a; a.loNote = 48; a.hiNote = 72; a.step = 12; a.velLayers = 2; a.holdSecs = 0.5f; a.tailSecs = 0.4f; a.name = "Hosted";
            CHECK (h.startAutoSampler (a, true).isEmpty(), "the Auto Sampler starts on the loaded plug-in");
            waitFor (h);
            std::printf ("    %s\n", h.samplingMessage.toRawUTF8());
            const auto& z = h.getInstrument()->keys.zones;
            bool ok = z.size() == 6;
            for (auto& q : z) ok = ok && detectRoot (*q.sample) == q.root;
            CHECK (ok && ! h.audition.load(), "the loaded plug-in is sampled into six zones, and the keyboard switches to the copy");
            const auto copy = playNote (h, 1, 60, 127, 0.4, 0.3);
            CHECK (std::abs (freqOf (copy, 2000, 8000) - 262.5) < 6.0, "the copy plays");
            h.unloadHosted();
            CHECK (h.hosted() == nullptr, "the plug-in unloads");
        }
    }
   #endif

    // The Sample page, on the instrument and on the effect.
    snapshot (fx, out.getChildFile ("omni-fx-sample.png"));
    snapshot (inst, out.getChildFile ("omni-sample.png"), [] (juce::Component& c)
    {
        for (auto* ch : c.getChildren()) if (auto* b = dynamic_cast<juce::TextButton*> (ch)) if (b->getButtonText() == "Sample" && b->onClick) b->onClick();
    });
    setLibraryFolder ({});
}
