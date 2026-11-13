#include "Capture.h"
#include <cmath>
#include <cstring>
#include <map>

namespace omni
{
    using namespace juce;

    // ---- library --------------------------------------------------------------------------
    static File& libraryOverride() { static File f; return f; }
    void setLibraryFolder (const File& f) { libraryOverride() = f; }

    File libraryFolder()
    {
        if (libraryOverride() != File()) return libraryOverride();
       #if JUCE_IOS
        return File::getSpecialLocation (File::userDocumentsDirectory);
       #else
        return File::getSpecialLocation (File::userMusicDirectory).getChildFile ("Omnisampler");
       #endif
    }
    File recordingsFolder()  { return libraryFolder().getChildFile ("Recordings"); }
    File autoSampledFolder() { return libraryFolder().getChildFile ("Auto Sampled"); }

    File uniqueFile (const File& folder, const String& base, const String& ext)
    {
        auto f = folder.getChildFile (base + ext);
        for (int i = 2; f.exists() && i < 10000; ++i) f = folder.getChildFile (base + " " + String (i) + ext);
        return f;
    }

    // "Take 0007" for the next recording. Four digits, so the number is never read as a MIDI note.
    String nextTakeName (const File& folder)
    {
        int highest = 0;
        for (auto& f : folder.findChildFiles (File::findFiles, false, "Take *.wav"))
            highest = jmax (highest, f.getFileNameWithoutExtension().substring (5, 9).getIntValue());
        return "Take " + String (highest + 1).paddedLeft ('0', 4);
    }

    // ---- tidying a take -------------------------------------------------------------------
    TakeBounds findTakeBounds (const AudioBuffer<float>& b, int frames, double sr, bool trimStart, bool trimEnd)
    {
        TakeBounds t;
        frames = jlimit (0, b.getNumSamples(), frames);
        const int ch = b.getNumChannels();
        for (int c = 0; c < ch; ++c) t.peak = jmax (t.peak, b.getMagnitude (c, 0, frames));
        t.end = frames;
        t.silent = t.peak < 5.0e-4f || frames < 16;
        if (t.silent) return t;
        auto mag = [&b, ch] (int i) { float m = 0.0f; for (int c = 0; c < ch; ++c) m = jmax (m, std::abs (b.getSample (c, i))); return m; };
        if (trimStart)
        {
            // The onset: the first frame within 40 dB of the peak, then back 3 ms so the attack is whole.
            const float thr = jmax (t.peak * 0.01f, 2.0e-4f);
            int i = 0;
            while (i < frames && mag (i) < thr) ++i;
            t.start = jmax (0, i - (int) (sr * 0.003));
        }
        if (trimEnd)
        {
            // The end: the last frame within 60 dB of the peak, plus 30 ms.
            const float thr = jmax (t.peak * 0.001f, 1.0e-4f);
            int i = frames - 1;
            while (i > t.start && mag (i) < thr) --i;
            t.end = jmin (frames, i + 1 + (int) (sr * 0.03));
        }
        return t;
    }

    AudioBuffer<float> cutTake (const AudioBuffer<float>& b, const TakeBounds& t, double sr, float gain, bool fades)
    {
        const int n = jmax (0, t.end - t.start), ch = b.getNumChannels();
        AudioBuffer<float> out (ch, n);
        for (int c = 0; c < ch; ++c) out.copyFrom (c, 0, b, c, t.start, n);
        out.applyGain (gain);
        if (fades && n > 64)
        {
            const int in = jmin (n / 4, jmax (1, (int) (sr * 0.001))), outLen = jmin (n / 4, jmax (1, (int) (sr * 0.008)));
            out.applyGainRamp (0, in, 0.0f, 1.0f);
            out.applyGainRamp (n - outLen, outLen, 1.0f, 0.0f);
        }
        return out;
    }

    void monoIfSame (AudioBuffer<float>& b)
    {
        if (b.getNumChannels() < 2) return;
        const int n = b.getNumSamples();
        const float* l = b.getReadPointer (0); const float* r = b.getReadPointer (1);
        float diff = 0.0f, pk = 0.0f;
        for (int i = 0; i < n; ++i) { diff = jmax (diff, std::abs (l[i] - r[i])); pk = jmax (pk, std::abs (l[i])); }
        if (diff <= pk * 1.0e-4f) b.setSize (1, n, true);
    }

    bool findLoop (AudioBuffer<float>& data, double sr, int from, int to, int& loopStart, int& loopEnd)
    {
        const int n = data.getNumSamples(), ch = data.getNumChannels();
        from = jlimit (0, n, from);
        to = jlimit (0, jmax (0, n - 2), to);
        if (to - from < (int) (sr * 0.25)) return false;
        const int W = jmin (2048, (to - from) / 8);                  // how much audio two points must share to match
        const int X = jmin ((int) (sr * 0.05), (to - from) / 4);     // crossfade length
        if (W < 64 || X < 16) return false;

        std::vector<float> x ((size_t) to, 0.0f);
        for (int c = 0; c < ch; ++c)
        {
            const float* d = data.getReadPointer (c);
            for (int i = 0; i < to; ++i) x[(size_t) i] += d[i];
        }
        // The loop ends at `to`. Its start is wherever in the first half the audio leading up to
        // it looks most like the audio leading up to the end (same phase, same level).
        const int lo = from + jmax (X, W), hi = jmin (from + (to - from) / 2, lo + (int) (sr * 1.5));
        if (hi <= lo) return false;
        const float* e = x.data() + (to - W);
        double eEnd = 0.0, eA = 0.0;
        for (int i = 0; i < W; ++i) { eEnd += (double) e[i] * e[i]; eA += (double) x[(size_t) (lo - W + i)] * x[(size_t) (lo - W + i)]; }
        if (eEnd < 1.0e-9) return false;
        double best = -1.0; int bestStart = -1;
        for (int s = lo; s < hi; ++s)
        {
            const float* a = x.data() + (s - W);
            double dot = 0.0;
            for (int i = 0; i < W; ++i) dot += (double) a[i] * e[i];
            const double q = 2.0 * dot / (eA + eEnd + 1.0e-12);
            if (q > best) { best = q; bestStart = s; }
            eA += (double) x[(size_t) s] * x[(size_t) s] - (double) x[(size_t) (s - W)] * x[(size_t) (s - W)];
        }
        if (bestStart < 0 || best < 0.6) return false;
        loopStart = bestStart; loopEnd = to;
        // Fade the audio before the loop end into the audio before the loop start, so the jump back is seamless.
        for (int c = 0; c < ch; ++c)
        {
            float* d = data.getWritePointer (c);
            for (int i = 0; i < X; ++i)
            {
                const float t = (float) (i + 1) / (float) X;
                const int a = to - X + i, b = bestStart - X + i;
                d[a] = d[a] * (1.0f - t) + d[b] * t;
            }
        }
        return true;
    }

    bool writeTake (const File& f, const AudioBuffer<float>& b, double sr, int root, int loopStart, int loopEnd)
    {
        f.getParentDirectory().createDirectory();
        f.deleteFile();
        StringPairArray meta;
        const bool loop = loopStart >= 0 && loopEnd > loopStart;
        if (root >= 0 || loop)
        {
            meta.set ("MidiUnityNote", String (root >= 0 ? root : 60));
            if (loop) { meta.set ("NumSampleLoops", "1"); meta.set ("Loop0Start", String (loopStart)); meta.set ("Loop0End", String (loopEnd)); }
        }
        WavAudioFormat wav;
        auto os = std::make_unique<FileOutputStream> (f);
        if (! os->openedOk()) return false;
        std::unique_ptr<AudioFormatWriter> w (wav.createWriterFor (os.get(), sr, (unsigned) b.getNumChannels(), 24, meta, 0));
        if (w == nullptr) return false;
        os.release();
        return w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
    }

    int detectRoot (const AudioBuffer<float>& b, double sr)
    {
        Sample s;
        s.data.setDataToReferTo (const_cast<float**> (b.getArrayOfReadPointers()), b.getNumChannels(), b.getNumSamples());
        s.sampleRate = sr;
        return detectRoot (s);
    }

    void splitKeysByRoot (std::vector<Zone>& zones)
    {
        std::map<int, std::vector<Zone*>> byRoot;
        for (auto& z : zones) byRoot[z.root].push_back (&z);
        std::vector<int> roots;
        for (auto& r : byRoot) roots.push_back (r.first);
        for (size_t i = 0; i < roots.size(); ++i)
        {
            const int lo = i == 0 ? 0 : (roots[i - 1] + roots[i]) / 2 + 1;
            const int hi = i + 1 == roots.size() ? 127 : (roots[i] + roots[i + 1]) / 2;
            std::vector<Zone*> layers;
            for (auto* z : byRoot[roots[i]]) { z->loKey = lo; z->hiKey = hi; if (z->rrPos == 0 && z->sample) layers.push_back (z); }
            if (layers.size() > 1) assignVelocityByLoudness (layers);
        }
    }

    // ---- Recorder -------------------------------------------------------------------------
    void Recorder::prepare (double sampleRate)
    {
        sr = sampleRate;
        wantArm = false; wantStop = false;
        st = State::Idle; length = 0;
        pre.setSize (2, kPre); pre.clear();
        prePos = preFilled = 0;
        if (buf.getNumSamples() != 0) buf.setSize (2, (int) (sr * kMaxSeconds));
    }

    bool Recorder::arm (Mode m, float thresholdDb, int numBars)
    {
        if (st.load() != State::Idle || wantArm.load()) return false;
        const int need = (int) (sr * kMaxSeconds);
        if (buf.getNumSamples() != need) buf.setSize (2, need);
        md = m;
        threshold = Decibels::decibelsToGain (thresholdDb);
        bars = jmax (1, numBars);
        wantStop = false;
        wantArm = true;
        return true;
    }

    void Recorder::stop() { wantStop = true; }
    void Recorder::clear() { if (st.load() == State::Done) { length = 0; st = State::Idle; } }

    void Recorder::append (const float* L, const float* R, int n)
    {
        const int at = length.load();
        n = jmin (n, buf.getNumSamples() - at);
        if (n <= 0) return;
        FloatVectorOperations::copy (buf.getWritePointer (0, at), L, n);
        FloatVectorOperations::copy (buf.getWritePointer (1, at), R, n);
        length = at + n;
    }

    void Recorder::process (const float* L, const float* R, int n, const Transport& t)
    {
        float pk = 0.0f;
        for (int i = 0; i < n; ++i) pk = jmax (pk, std::abs (L[i]), std::abs (R[i]));
        if (pk > level.load()) level = pk;

        auto s = st.load();
        if (wantStop.exchange (false))
        {
            if (s == State::Recording) st = s = State::Done;
            else if (s == State::Armed) st = s = State::Idle;
        }
        if (wantArm.exchange (false) && s == State::Idle)
        {
            length = 0; quiet = 0; target = 0; prePos = 0; preFilled = 0;
            st = s = md == Mode::Manual ? State::Recording : State::Armed;
        }
        if (s == State::Idle || s == State::Done) return;

        int start = 0;
        if (s == State::Armed)
        {
            if (md == Mode::Threshold)
            {
                int i = 0;
                while (i < n && std::abs (L[i]) < threshold && std::abs (R[i]) < threshold) ++i;
                if (i == n)
                {
                    // Nothing yet: remember the newest input, so the take can start a little before the trigger.
                    for (int k = 0; k < n; ++k)
                    {
                        pre.setSample (0, prePos, L[k]); pre.setSample (1, prePos, R[k]);
                        prePos = (prePos + 1) % kPre;
                    }
                    preFilled = jmin (kPre, preFilled + n);
                    return;
                }
                // Triggered at frame i: 10 ms of what came before it goes in first.
                const int want = jmin ((int) (sr * 0.01), kPre);
                const int fromBlock = jmin (i, want), fromRing = jmin (preFilled, want - fromBlock);
                for (int k = 0; k < fromRing; ++k)
                {
                    const int idx = (prePos - fromRing + k + kPre) % kPre;
                    buf.setSample (0, k, pre.getSample (0, idx)); buf.setSample (1, k, pre.getSample (1, idx));
                }
                length = fromRing;
                start = i - fromBlock;
                st = s = State::Recording;
            }
            else   // Bars: wait for the next bar line of the host
            {
                double bpm = 120.0, quartersPerBar = 4.0;
                if (t.valid)
                {
                    if (! t.playing) return;
                    bpm = jmax (1.0, t.bpm); quartersPerBar = jmax (0.25, t.quartersPerBar);
                    const double perFrame = bpm / (60.0 * sr);            // quarter notes per frame
                    const double into = std::fmod (jmax (0.0, t.ppq - t.barStartPpq), quartersPerBar);
                    const double toBar = into < perFrame * 0.5 ? 0.0 : quartersPerBar - into;
                    const int framesToBar = (int) std::round (toBar / perFrame);
                    if (framesToBar >= n) return;
                    start = framesToBar;
                }
                target = jlimit (1, buf.getNumSamples(), (int) std::round (bars * quartersPerBar * 60.0 / bpm * sr));
                st = s = State::Recording;
            }
        }

        int m = n - start;
        if (md == Mode::Bars) m = jmin (m, target - length.load());
        append (L + start, R + start, m);
        if (md == Mode::Threshold)
        {
            // Stops by itself once the sound has died away.
            const float floor = threshold * 0.25f;
            for (int i = start; i < n; ++i) quiet = (std::abs (L[i]) < floor && std::abs (R[i]) < floor) ? quiet + 1 : 0;
            if (quiet > (int) (sr * 0.6) && length.load() > (int) (sr * 0.05)) st = State::Done;
        }
        if ((md == Mode::Bars && length.load() >= target) || length.load() >= buf.getNumSamples()) st = State::Done;
    }

    // ---- Auto Sampler ---------------------------------------------------------------------
    std::vector<int> autoVelocities (int layers)
    {
        layers = jlimit (1, 8, layers);
        std::vector<int> v;
        for (int i = 1; i <= layers; ++i) v.push_back (jlimit (1, 127, (int) std::round (127.0 * i / layers)));
        return v;
    }

    std::vector<AutoNote> autoNotes (const AutoSettings& a)
    {
        std::vector<AutoNote> out;
        const auto vels = autoVelocities (a.velLayers);
        const int lo = jlimit (0, 127, jmin (a.loNote, a.hiNote)), hi = jlimit (0, 127, jmax (a.loNote, a.hiNote));
        for (int note = lo; note <= hi; note += jmax (1, a.step))
            for (int layer = 0; layer < (int) vels.size(); ++layer)
                for (int rr = 0; rr < jlimit (1, 8, a.roundRobins); ++rr)
                    out.push_back ({ note, vels[(size_t) layer], layer, rr });
        return out;
    }

    std::vector<Zone> autoZones (const AutoSettings& a, const std::vector<AutoCapture>& caps, SampleCache& cache, float velSens)
    {
        std::vector<Zone> zones;
        std::vector<int> roots;
        float peak = 0.0f;
        for (auto& c : caps)
        {
            if (std::find (roots.begin(), roots.end(), c.n.note) == roots.end()) roots.push_back (c.n.note);
            peak = jmax (peak, c.peak);
        }
        std::sort (roots.begin(), roots.end());
        const auto vels = autoVelocities (a.velLayers);
        // One gain for the whole set, so the loudest note peaks at -1 dB and the dynamics stay as played.
        const float normDb = a.normalize && peak > 0.0f ? jlimit (-24.0f, 24.0f, -1.0f - Decibels::gainToDecibels (peak)) : 0.0f;
        for (size_t i = 0; i < roots.size(); ++i)
        {
            const int root = roots[i];
            const int lo = i == 0 ? 0 : (roots[i - 1] + root) / 2 + 1;
            const int hi = i + 1 == roots.size() ? 127 : (root + roots[i + 1]) / 2;
            std::vector<int> layers;   // the layers that made a sound for this note, quietest first
            for (auto& c : caps)
                if (c.n.note == root && std::find (layers.begin(), layers.end(), c.n.layer) == layers.end()) layers.push_back (c.n.layer);
            std::sort (layers.begin(), layers.end());
            for (size_t li = 0; li < layers.size(); ++li)
            {
                const int layer = layers[li];
                const int loVel = li == 0 ? 1 : vels[(size_t) jlimit (0, (int) vels.size() - 1, layers[li - 1])] + 1;
                const int hiVel = li + 1 == layers.size() ? 127 : vels[(size_t) jlimit (0, (int) vels.size() - 1, layer)];
                for (auto& c : caps)
                {
                    if (c.n.note != root || c.n.layer != layer) continue;
                    String err;
                    Zone z;
                    z.sample = cache.get (c.file, err);
                    if (! z.sample) continue;
                    z.root = root; z.loKey = lo; z.hiKey = hi; z.loVel = loVel; z.hiVel = jmax (loVel, hiVel);
                    z.rrPos = a.roundRobins > 1 ? c.n.rr + 1 : 0;
                    z.gainDb = normDb - Decibels::gainToDecibels (velocityGain (c.n.vel, velSens)) - Decibels::gainToDecibels (c.fileGain);
                    if (c.loopStart >= 0 && c.loopEnd > c.loopStart) { z.loop = true; z.loopStart = c.loopStart; z.loopEnd = c.loopEnd; }
                    zones.push_back (z);
                }
            }
        }
        return zones;
    }

    AutoNote AutoSampler::current() const
    {
        const int i = rtIndex.load();
        return i >= 0 && i < (int) notes.size() ? notes[(size_t) i] : AutoNote();
    }

    double AutoSampler::estimatedSeconds() const
    {
        return (double) notes.size() * (0.3 + set.holdSecs + set.tailSecs);
    }

    bool AutoSampler::startOffline (AudioProcessor& s, SpinLock& lock, const AutoSettings& a, const File& folder)
    {
        if (isThreadRunning() || rtActive.load()) return false;
        set = a; notes = autoNotes (a); captured.clear(); dir = folder;
        if (notes.empty()) return false;
        synth = &s; synthLock = &lock; realtime = false; device = nullptr;
        sr = s.getSampleRate() > 0.0 ? s.getSampleRate() : 44100.0;
        doneCount = 0; silentCount = 0; rtIndex = 0;
        st = State::Running;
        startThread();
        return true;
    }

    bool AutoSampler::startRealtime (const AutoSettings& a, double sampleRate, const File& folder, MidiOutput* dev)
    {
        if (isThreadRunning() || rtActive.load()) return false;
        set = a; notes = autoNotes (a); captured.clear(); dir = folder;
        if (notes.empty()) return false;
        device = dev; realtime = true; sr = sampleRate;
        doneCount = 0; silentCount = 0;
        gapFrames = (int) (0.25 * sr);
        holdFrames = jmax (1, (int) (a.holdSecs * sr));
        tailFrames = jmax (1, (int) (a.tailSecs * sr));
        noteBuf.setSize (2, holdFrames + tailFrames + 16);
        noteBuf.clear();
        phase = Phase::Gap; pos = 0; recLen = 0; noteIsOn = false;
        rtIndex = 0; noteReady = false; noteTaken = false; rtStop = false;
        deviceFifo.reset();
        st = State::Running;
        startThread();
        rtActive = true;
        return true;
    }

    void AutoSampler::cancel()
    {
        if (! isThreadRunning() && ! rtActive.load()) return;
        if (realtime && rtActive.load())
        {
            // Let the audio thread send the note-off, if it is running.
            rtStop = true;
            for (int i = 0; i < 100 && rtActive.load(); ++i) Thread::sleep (3);
            rtActive = false;
        }
        stopThread (20000);
        if (st.load() == State::Running) st = State::Cancelled;
    }

    void AutoSampler::sendToDevice (const MidiMessage& m)
    {
        if (device == nullptr || m.getRawDataSize() > 3) return;
        const auto w = deviceFifo.write (1);
        if (w.blockSize1 < 1) return;
        auto& q = deviceQueue[w.startIndex1];
        q.bytes[0] = q.bytes[1] = q.bytes[2] = 0;
        std::memcpy (q.bytes, m.getRawData(), (size_t) m.getRawDataSize());
    }

    void AutoSampler::process (const float* L, const float* R, int n, MidiBuffer& out)
    {
        if (! rtActive.load()) return;
        auto send = [&] (const MidiMessage& m, int at) { out.addEvent (m, at); sendToDevice (m); };
        auto note = [this] { return notes[(size_t) jlimit (0, (int) notes.size() - 1, rtIndex.load())]; };
        if (rtStop.load())
        {
            if (noteIsOn) send (MidiMessage::noteOff (set.channel, note().note), 0);
            send (MidiMessage::allNotesOff (set.channel), 0);
            noteIsOn = false;
            rtActive = false;
            return;
        }
        int at = 0;
        while (at < n)
        {
            if (phase == Phase::Gap)
            {
                if (pos >= gapFrames)
                {
                    const auto nt = note();
                    send (MidiMessage::noteOn (set.channel, nt.note, (uint8) nt.vel), at);
                    noteIsOn = true; phase = Phase::Hold; pos = 0; recLen = 0;
                    continue;
                }
                const int m = jmin (gapFrames - pos, n - at);
                pos += m; at += m;
            }
            else if (phase == Phase::Hold || phase == Phase::Tail)
            {
                const int limit = phase == Phase::Hold ? holdFrames : tailFrames;
                const int room = noteBuf.getNumSamples() - recLen;
                if (pos >= limit || room <= 0)
                {
                    if (phase == Phase::Hold)
                    {
                        send (MidiMessage::noteOff (set.channel, note().note), at);
                        noteIsOn = false; phase = Phase::Tail; pos = 0;
                    }
                    else
                    {
                        readyFrames = recLen;
                        noteReady = true;      // the worker thread trims and saves it
                        phase = Phase::Wait;
                    }
                    continue;
                }
                const int m = jmin (limit - pos, n - at, room);
                FloatVectorOperations::copy (noteBuf.getWritePointer (0, recLen), L + at, m);
                FloatVectorOperations::copy (noteBuf.getWritePointer (1, recLen), R + at, m);
                recLen += m; pos += m; at += m;
            }
            else   // Wait: nothing more to do until the note has been saved
            {
                if (! noteTaken.exchange (false)) break;
                rtIndex = rtIndex.load() + 1;
                phase = Phase::Gap; pos = 0;
            }
        }
    }

    void AutoSampler::keepNote (const AutoNote& nt, int frames)
    {
        const auto b = findTakeBounds (noteBuf, frames, sr, true, true);
        if (b.silent) { silentCount.fetch_add (1); return; }
        auto cut = cutTake (noteBuf, b, sr, 1.0f);
        monoIfSame (cut);
        AutoCapture c;
        c.n = nt; c.peak = b.peak;
        if (b.peak > 0.98f) { c.fileGain = 0.98f / b.peak; cut.applyGain (c.fileGain); }   // a 24-bit file can't go over full scale
        if (set.loop)
        {
            // Loop inside the part where the key was held, after the attack has settled.
            const int held = jmax (0, (int) (set.holdSecs * sr) - b.start);
            int ls = 0, le = 0;
            if (findLoop (cut, sr, (int) (held * 0.3), held - (int) (sr * 0.02), ls, le)) { c.loopStart = ls; c.loopEnd = le; }
        }
        String name = File::createLegalFileName (set.name).trim();
        if (name.isEmpty()) name = "Auto Sampled";
        name << " " << noteName (nt.note) << " v" << String (nt.vel).paddedLeft ('0', 3);
        if (set.roundRobins > 1) name << " rr" << (nt.rr + 1);
        c.file = dir.getChildFile (name + ".wav");
        if (writeTake (c.file, cut, sr, nt.note, c.loopStart, c.loopEnd)) captured.push_back (c);
    }

    void AutoSampler::run()
    {
        dir.createDirectory();
        if (realtime) runRealtime(); else runOffline();
        st = threadShouldExit() ? State::Cancelled : State::Finished;
    }

    void AutoSampler::runOffline()
    {
        const SpinLock::ScopedLockType sl (*synthLock);
        auto& p = *synth;
        const int block = jlimit (64, 2048, p.getBlockSize() > 0 ? p.getBlockSize() : 512);
        const int outs = p.getTotalNumOutputChannels();
        const int chans = jmax (2, p.getTotalNumInputChannels(), outs);
        const int hold = jmax (1, (int) (set.holdSecs * sr)), tail = jmax (0, (int) (set.tailSecs * sr));
        noteBuf.setSize (2, hold + tail + block);
        AudioBuffer<float> io (chans, block);
        MidiBuffer midi;
        const bool wasOffline = p.isNonRealtime();
        p.setNonRealtime (true);
        for (size_t k = 0; k < notes.size() && ! threadShouldExit(); ++k)
        {
            rtIndex = (int) k;
            const auto nt = notes[k];
            // Let whatever is still ringing from the last note die away first (two seconds at most).
            for (int b = 0, quietBlocks = 0; b < (int) (2.0 * sr / block) && quietBlocks < 4 && ! threadShouldExit(); ++b)
            {
                io.clear(); midi.clear();
                p.processBlock (io, midi);
                quietBlocks = io.getMagnitude (0, block) < 1.0e-4f ? quietBlocks + 1 : 0;
            }
            int len = 0;
            bool on = false, off = false;
            while (len < hold + tail && ! threadShouldExit())
            {
                io.clear(); midi.clear();
                if (! on) { midi.addEvent (MidiMessage::noteOn (set.channel, nt.note, (uint8) nt.vel), 0); on = true; }
                if (! off && len + block > hold) { midi.addEvent (MidiMessage::noteOff (set.channel, nt.note), jlimit (0, block - 1, hold - len)); off = true; }
                p.processBlock (io, midi);
                noteBuf.copyFrom (0, len, io, 0, 0, block);
                noteBuf.copyFrom (1, len, io, outs > 1 ? 1 : 0, 0, block);
                len += block;
            }
            if (threadShouldExit()) break;
            keepNote (nt, hold + tail);
            doneCount = (int) k + 1;
        }
        // Leave the plug-in quiet.
        io.clear(); midi.clear();
        midi.addEvent (MidiMessage::allNotesOff (set.channel), 0);
        midi.addEvent (MidiMessage::allSoundOff (set.channel), 0);
        p.processBlock (io, midi);
        p.setNonRealtime (wasOffline);
    }

    void AutoSampler::runRealtime()
    {
        auto flush = [this]
        {
            for (;;)
            {
                RawMidi m;
                {
                    const auto r = deviceFifo.read (1);
                    if (r.blockSize1 < 1) break;
                    m = deviceQueue[r.startIndex1];
                }
                if (device != nullptr) device->sendMessageNow (MidiMessage (m.bytes[0], m.bytes[1], m.bytes[2]));
            }
        };
        while (! threadShouldExit())
        {
            flush();
            if (noteReady.exchange (false))
            {
                const int idx = rtIndex.load();
                keepNote (notes[(size_t) idx], readyFrames.load());
                doneCount = idx + 1;
                if (idx + 1 >= (int) notes.size()) { rtActive = false; break; }
                noteTaken = true;
            }
            wait (1);
        }
        flush();
        if (device != nullptr && threadShouldExit()) device->sendMessageNow (MidiMessage::allNotesOff (set.channel));
    }
}
