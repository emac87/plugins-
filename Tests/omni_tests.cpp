#include "TestCommon.h"
#include "PluginProcessor.h"
#include "PluginEditor.h"

using namespace omni;

static juce::File tone (const juce::File& f, double freq, float amp, double secs = 1.0)
{
    juce::AudioBuffer<float> b (1, (int) (44100 * secs));
    for (int i = 0; i < b.getNumSamples(); ++i) b.setSample (0, i, amp * (float) std::sin (2 * juce::MathConstants<double>::pi * freq * i / 44100.0) * std::exp (-i / (44100.0 * secs)));
    writeWav (f, b, 44100);
    return f;
}

// Plays one note and returns (rms, zero-crossing frequency) of the first 0.25 s.
static std::pair<double, double> play (OmniProcessor& p, int ch, int note, int vel, int samples = 11025)
{
    juce::AudioBuffer<float> buf (2, samples);
    juce::MidiBuffer midi;
    midi.addEvent (juce::MidiMessage::noteOn (ch, note, (juce::uint8) vel), 0);
    p.processBlock (buf, midi);
    int zc = 0;
    for (int i = 1; i < samples; ++i) if ((buf.getSample (0, i - 1) < 0) != (buf.getSample (0, i) < 0)) ++zc;
    juce::MidiBuffer off; off.addEvent (juce::MidiMessage::noteOff (ch, note), 0);
    juce::AudioBuffer<float> tail (2, 44100); p.processBlock (tail, off);
    return { rms (buf, 0, 0, samples), zc / 2.0 / (samples / 44100.0) };
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    juce::File out (argc > 1 ? argv[1] : "/tmp/test-output"); out.createDirectory();
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("omni-test");
    dir.deleteRecursively(); dir.getChildFile ("samples").createDirectory();
    auto s = dir.getChildFile ("samples");
    std::printf ("Omnisampler\n");

    CHECK (rootFromFileName ("Piano_C#3") == 49, "reads C#3 from a file name");
    CHECK (rootFromFileName ("Bass-A1 sustain") == 33, "reads A1 from a file name");
    CHECK (rootFromFileName ("vln_60") == 60, "reads a MIDI number from a file name");
    CHECK (rootFromFileName ("Kick 2") == -1, "ignores names without a note");

    auto c4 = tone (s.getChildFile ("Piano_C4.wav"), 261.63, 0.5f);
    auto c4soft = tone (s.getChildFile ("Piano_C4_soft.wav"), 261.63, 0.06f);
    auto e4 = tone (s.getChildFile ("Piano_E4.wav"), 329.63, 0.5f);
    auto g4 = tone (s.getChildFile ("Piano_G4.wav"), 392.0, 0.5f);
    auto a440 = tone (s.getChildFile ("mystery.wav"), 440.0, 0.5f);

    OmniProcessor p;
    p.prepareToPlay (44100, 512);
    {
        juce::String e;
        auto smp = p.cache.get (a440, e);
        CHECK (smp && detectRoot (*smp) == 69, "detects A4 from the audio when the name has no note");
    }

    // Keys: smart mapping.
    auto msg = p.loadFilesToKeys ({ c4, c4soft, e4, g4 }, true);
    auto inst = p.getInstrument();
    const auto& kz = inst->keys.zones;
    std::printf ("    keys: %d zones\n", (int) kz.size());
    for (auto& z : kz) std::printf ("      %-16s root %d keys %d-%d vel %d-%d\n", z.sample->file.getFileName().toRawUTF8(), z.root, z.loKey, z.hiKey, z.loVel, z.hiVel);
    CHECK (kz.size() == 4, "maps four files onto the keys");
    bool okMap = true;
    for (auto& z : kz)
    {
        if (z.sample->file == c4soft) okMap = okMap && z.root == 60 && z.hiVel < 127;
        if (z.sample->file == c4) okMap = okMap && z.root == 60 && z.loVel > 1 && z.hiVel == 127;
        if (z.sample->file == e4) okMap = okMap && z.root == 64 && z.loKey == 63 && z.hiKey == 65;
        if (z.sample->file == g4) okMap = okMap && z.root == 67 && z.hiKey == 127;
    }
    CHECK (okMap, "roots from names, key splits between roots, soft/hard C4 become velocity layers");

    auto [rmsHi, f72] = play (p, 1, 72, 120);
    std::printf ("    note 72 from the C4 sample: %.1f Hz\n", f72);
    CHECK (std::abs (f72 - 523.25) < 8, "repitches by key (C5 plays an octave up)");
    auto [rmsLo, fLo] = play (p, 1, 60, 10);
    std::printf ("    C4 vel 120 rms %.4f, vel 10 rms %.4f\n", rmsHi, rmsLo);
    CHECK (rmsLo < rmsHi * 0.3, "soft velocity picks the soft layer");

    // SFZ.
    {
        auto sfz = dir.getChildFile ("test.sfz");
        sfz.replaceWithText ("// test\n<control> default_path=samples/\n#define $LO 1\n<group> lovel=$LO hivel=127 seq_length=2\n"
                             "<region> sample=Piano_C4.wav key=60 seq_position=1\n<region> sample=Piano_E4.wav key=c4 seq_position=2\n"
                             "<group>\n<region> sample=Piano_G4.wav lokey=g4 hikey=b4 pitch_keycenter=g4 volume=-6 /* block */ ampeg_release=1.5\n"
                             "<region> sample=Piano_G4.wav key=90 trigger=release\n");
        auto r = p.loadInstrumentFile (sfz);
        const auto& z = p.getInstrument()->keys.zones;
        std::printf ("    sfz: %d zones (%s)\n", (int) z.size(), r.toRawUTF8());
        CHECK (z.size() == 3, "SFZ: loads three playable regions, skips the release trigger");
        CHECK (z.size() == 3 && z[0].rrPos == 1 && z[1].rrPos == 2 && z[1].root == 60 && z[2].loKey == 67 && z[2].hiKey == 71 && std::abs (z[2].gainDb + 6) < 0.01,
               "SFZ: groups, note names, round robins and volume carry over");
        auto [a, fa] = play (p, 1, 60, 100);
        auto [b, fb] = play (p, 1, 60, 100);
        std::printf ("    round robin on C4: %.0f Hz then %.0f Hz\n", fa, fb);
        CHECK (std::abs (fa - fb) > 30, "round robins alternate between samples");
        auto saved = dir.getChildFile ("resaved.sfz");
        juce::String err;
        CHECK (saveSfz (p.getInstrument()->keys, saved, err) && saved.loadFileAsString().contains ("seq_position=2"), "saves the keys back out as SFZ");
    }

    // DecentSampler.
    {
        auto ds = dir.getChildFile ("test.dspreset");
        ds.replaceWithText ("<DecentSampler><groups attack=\"0.01\" release=\"0.8\"><group volume=\"-3dB\">"
                            "<sample path=\"samples/Piano_C4.wav\" rootNote=\"60\" loNote=\"0\" hiNote=\"62\" loVel=\"1\" hiVel=\"127\"/>"
                            "<sample path=\"samples/Piano_E4.wav\" rootNote=\"64\" loNote=\"63\" hiNote=\"127\"/>"
                            "</group></groups></DecentSampler>");
        auto r = p.loadInstrumentFile (ds);
        const auto& z = p.getInstrument()->keys.zones;
        CHECK (z.size() == 2 && z[1].loKey == 63 && std::abs (z[0].gainDb + 3) < 0.01 && std::abs (p.getInstrument()->keys.voicing.env.release - 0.8f) < 0.01,
               "DecentSampler: zones, group volume and envelope load");
    }

    // Pads: velocity layers by loudness, round robins, choke.
    {
        auto k1 = tone (s.getChildFile ("kick_a.wav"), 60, 0.04f, 0.3), k2 = tone (s.getChildFile ("kick_b.wav"), 60, 0.2f, 0.3), k3 = tone (s.getChildFile ("kick_c.wav"), 60, 0.9f, 0.3);
        p.loadFilesToPad (0, { k3, k1, k2 }, DropMode::VelocityLayers);
        const auto& pz = p.getInstrument()->pads[0].zones;
        std::printf ("    pad 1 layers:");
        for (auto& z : pz) std::printf (" [%s %d-%d]", z.sample->file.getFileNameWithoutExtension().toRawUTF8(), z.loVel, z.hiVel);
        std::printf ("\n");
        CHECK (pz.size() == 3 && pz[0].sample->file == k1 && pz[2].sample->file == k3 && pz[0].loVel == 1 && pz[2].hiVel == 127 && pz[1].loVel == pz[0].hiVel + 1,
               "pad drop sorts layers quietest to loudest with no gaps");
        auto h1 = tone (s.getChildFile ("hat_closed.wav"), 3000, 0.3f, 0.1), h2 = tone (s.getChildFile ("hat_open.wav"), 5000, 0.3f, 2.0);
        p.loadFilesToPad (1, { h1 }, DropMode::VelocityLayers);
        p.loadFilesToPad (2, { h2 }, DropMode::VelocityLayers);
        p.edit ([] (Instrument& in) { in.pads[1].chokeGroup = 1; in.pads[2].chokeGroup = 1; in.keys.zones.clear(); });
        juce::AudioBuffer<float> buf (2, 22050);
        juce::MidiBuffer m;
        m.addEvent (juce::MidiMessage::noteOn (10, 38, (juce::uint8) 100), 0);
        m.addEvent (juce::MidiMessage::noteOn (10, 37, (juce::uint8) 100), 11025);
        p.processBlock (buf, m);
        const double tailRms = rms (buf, 0, 20000, 2000);
        std::printf ("    open hat after closed-hat choke: rms %.5f\n", tailRms);
        CHECK (tailRms < 0.02, "choke group cuts the open hat");
        auto [r, f] = play (p, 10, 36, 127);
        CHECK (r > 0.05, "pads play on channel 10");
    }

    // Pads and Keys live on separate MIDI channels by default and never trigger each other.
    {
        CHECK (p.padsChannel() == 10 && p.keysChannel() == 1, "defaults: Pads on channel 10, Keys on channel 1");
        // A key zone on the kick's note (36), so both sections could answer the same note.
        p.loadFilesToKeys ({ c4 }, true);
        p.edit ([] (Instrument& in) { for (auto& z : in.keys.zones) { z.loKey = 0; z.hiKey = 127; z.root = 36; } });
        auto setCh = [&p] (const char* id, int ch) { auto* par = p.apvts.getParameter (id); par->setValueNotifyingHost (par->convertTo0to1 ((float) ch)); };
        auto voicesAfter = [&p] (std::function<void (juce::MidiBuffer&)> fill)
        {
            juce::MidiBuffer off; off.addEvent (juce::MidiMessage::allNotesOff (1), 0);
            juce::AudioBuffer<float> quiet (2, 22050); p.processBlock (quiet, off);
            juce::MidiBuffer m; fill (m);
            juce::AudioBuffer<float> b (2, 256); p.processBlock (b, m);
            return std::pair<int, int> { p.engine.activeVoicesFrom (true), p.engine.activeVoicesFrom (false) };
        };
        auto ctl = [&] (int ch) { return voicesAfter ([ch] (juce::MidiBuffer& m) { m.addEvent (juce::MidiMessage::noteOn (ch, 36, (juce::uint8) 100), 0); }); };

        auto a = ctl (10), b = ctl (1), c = ctl (5);
        std::printf ("    controller note 36: ch10 pads %d keys %d | ch1 pads %d keys %d | ch5 pads %d keys %d\n", a.first, a.second, b.first, b.second, c.first, c.second);
        CHECK (a.first > 0 && a.second == 0, "controller on the Pads channel plays the pad only");
        CHECK (b.first == 0 && b.second > 0, "controller on the Keys channel plays the key only");
        CHECK (c.first == 0 && c.second == 0, "other channels play nothing");

        auto tapPad = voicesAfter ([&p] (juce::MidiBuffer&) { p.padState.noteOn (10, 36, 0.8f); });
        auto tapKey = voicesAfter ([&p] (juce::MidiBuffer&) { p.keyboardState.noteOn (1, 36, 0.8f); });
        CHECK (tapPad.first > 0 && tapPad.second == 0, "tapping an on-screen pad plays the pad only");
        CHECK (tapKey.first == 0 && tapKey.second > 0, "playing the on-screen keyboard plays the key only");

        // Even with both sections on "Any", the on-screen pads and keyboard stay separate.
        setCh ("padsch", 0); setCh ("keysch", 0);
        auto anyPad = voicesAfter ([&p] (juce::MidiBuffer&) { p.padState.noteOn (10, 36, 0.8f); });
        auto anyKey = voicesAfter ([&p] (juce::MidiBuffer&) { p.keyboardState.noteOn (1, 36, 0.8f); });
        auto anyCtl = ctl (3);
        CHECK (anyPad.first > 0 && anyPad.second == 0 && anyKey.first == 0 && anyKey.second > 0, "on-screen pads and keyboard stay separate on Any/Any");
        CHECK (anyCtl.first > 0 && anyCtl.second > 0, "Any/Any still lets a controller play both, if the user picks that");
        // One section on "Any" leaves the other section's channel alone.
        setCh ("padsch", 0); setCh ("keysch", 2);
        auto mix = ctl (2);
        CHECK (mix.first == 0 && mix.second > 0, "Pads on Any skip the Keys channel");

        // Old sessions (both on Any, the old default) move to the new defaults; a deliberate choice is kept.
        juce::MemoryBlock mb; p.getStateInformation (mb);
        auto xml = juce::AudioProcessor::getXmlFromBinary (mb.getData(), (int) mb.getSize());
        auto asOld = [&xml] (int pads, int keys)
        {
            juce::XmlElement x (*xml);
            x.removeAttribute ("stateVersion");
            for (auto* e : x.getChildWithTagNameIterator ("PARAM"))
            {
                if (e->getStringAttribute ("id") == "padsch") e->setAttribute ("value", (double) pads);
                if (e->getStringAttribute ("id") == "keysch") e->setAttribute ("value", (double) keys);
            }
            juce::MemoryBlock out; juce::AudioProcessor::copyXmlToBinary (x, out); return out;
        };
        { OmniProcessor q; auto o = asOld (0, 0); q.setStateInformation (o.getData(), (int) o.getSize());
          CHECK (q.padsChannel() == 10 && q.keysChannel() == 1, "old Any/Any sessions open on 10 and 1"); }
        { OmniProcessor q; auto o = asOld (0, 3); q.setStateInformation (o.getData(), (int) o.getSize());
          CHECK (q.padsChannel() == 0 && q.keysChannel() == 3, "old sessions with a chosen channel are left alone"); }
        { OmniProcessor q; q.setStateInformation (mb.getData(), (int) mb.getSize());
          CHECK (q.padsChannel() == 0 && q.keysChannel() == 2, "new sessions keep exactly what was set, Any included"); }
        setCh ("padsch", 10); setCh ("keysch", 1);
        juce::MidiBuffer off; off.addEvent (juce::MidiMessage::allNotesOff (1), 0);
        juce::AudioBuffer<float> quiet (2, 22050); p.processBlock (quiet, off);
        p.edit ([] (Instrument& in) { in.keys.zones.clear(); });
    }

    // Kit round trip.
    {
        auto kit = dir.getChildFile ("kit.omni");
        CHECK (p.saveKit (kit).isEmpty(), "saves a kit");
        OmniProcessor q;
        CHECK (q.loadKit (kit).isEmpty() && q.getInstrument()->pads[0].zones.size() == 3 && q.getInstrument()->pads[1].chokeGroup == 1, "loads the kit back");
        juce::MemoryBlock mb; p.getStateInformation (mb);
        OmniProcessor r; r.setStateInformation (mb.getData(), (int) mb.getSize());
        CHECK (r.getInstrument()->keys.zones.size() == p.getInstrument()->keys.zones.size(), "plug-in state keeps pads and keys");
    }

    p.loadFilesToKeys ({ c4, c4soft, e4, g4 }, true);
    p.edit ([] (Instrument& in) { in.pads[0].name = "Kick"; in.pads[1].name = "Closed hat"; in.pads[2].name = "Open hat"; });
    snapshot (p, out.getChildFile ("omni-pads.png"));
    snapshot (p, out.getChildFile ("omni-keys.png"), [] (juce::Component& c)
    {
        auto& ed = dynamic_cast<OmniEditor&> (c);
        for (auto* ch : c.getChildren()) if (auto* b = dynamic_cast<juce::TextButton*> (ch)) if (b->getButtonText() == "Keys") { if (b->onClick) b->onClick(); }
        ed.selectKeyZone (1);
    });
    // Small-screen (iPhone / AUv3) layouts: phone on its side, and a tall narrow window.
    auto compact = [] (int w, int h, bool keys, const char* tab)
    {
        return [=] (juce::Component& c)
        {
            auto& ed = dynamic_cast<OmniEditor&> (c);
            ed.setResizeLimits (300, 240, 4000, 4000);
            ed.setSize (w, h);
            for (auto* ch : c.getChildren())
                if (auto* b = dynamic_cast<juce::TextButton*> (ch))
                {
                    if (keys && b->getButtonText() == "Keys" && b->onClick) b->onClick();
                    if (b->getButtonText() == tab) b->setToggleState (true, juce::sendNotificationSync);
                }
            if (keys) ed.selectKeyZone (1);
        };
    };
    snapshot (p, out.getChildFile ("omni-phone-pads.png"), compact (750, 350, false, "Samples"));
    snapshot (p, out.getChildFile ("omni-phone-pads-edit.png"), compact (750, 350, false, "Edit"));
    snapshot (p, out.getChildFile ("omni-phone-pads-sound.png"), compact (750, 350, false, "Sound"));
    snapshot (p, out.getChildFile ("omni-phone-keys.png"), compact (750, 350, true, "Samples"));
    snapshot (p, out.getChildFile ("omni-phone-keys-edit.png"), compact (750, 350, true, "Edit"));
    snapshot (p, out.getChildFile ("omni-phone-keys-sound.png"), compact (750, 350, true, "Sound"));
    snapshot (p, out.getChildFile ("omni-portrait-pads.png"), compact (390, 760, false, "Samples"));
    snapshot (p, out.getChildFile ("omni-portrait-keys.png"), compact (390, 760, true, "Samples"));
    snapshot (p, out.getChildFile ("omni-tall-pads.png"), compact (800, 1100, false, "Samples"));
    snapshot (p, out.getChildFile ("omni-tall-keys.png"), compact (800, 1100, true, "Sound"));
    std::printf (failures ? "FAILURES: %d\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
