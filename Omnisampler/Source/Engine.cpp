#include "Engine.h"

namespace omni
{
    void Engine::prepare (double sr, int maxBlock)
    {
        sampleRate = sr;
        for (auto& v : voices) { if (v.active) endVoice (v); v.env.setSampleRate (sr); }
        candidates.reserve (512);
        scratch.assign ((size_t) juce::jmax (512, maxBlock), 0.0f);
    }

    void Engine::endVoice (Voice& v)
    {
        if (v.slot >= 0 && streamer != nullptr) streamer->close (v.slot);
        v.slot = -1;
        v.active = false;
        v.synth = false;
        v.drum.stop();
        v.sample.reset();      // the instrument (owner) and the graveyard keep samples alive, so this never frees
        v.owner.reset();
        v.voicing = nullptr;
    }

    void Engine::handleMidi (const juce::MidiMessage& m, int padsCh, int keysCh)
    {
        if (m.isNoteOn() || m.isNoteOff())
        {
            bool toPads, toKeys;
            route (m.getChannel(), padsCh, keysCh, toPads, toKeys);
            if (m.isNoteOn()) noteOn (m.getChannel(), m.getNoteNumber(), m.getVelocity(), toPads, toKeys);
            else              noteOff (m.getChannel(), m.getNoteNumber(), toPads, toKeys);
        }
        else if (m.isAllNotesOff() || m.isAllSoundOff()) allNotesOff();
    }

    void Engine::handleLocal (const juce::MidiMessage& m, bool pads)
    {
        if (m.isNoteOn())       noteOn (m.getChannel(), m.getNoteNumber(), m.getVelocity(), pads, ! pads);
        else if (m.isNoteOff()) noteOff (m.getChannel(), m.getNoteNumber(), pads, ! pads);
    }

    void Engine::allNotesOff()
    {
        for (auto& v : voices) if (v.active) { v.fading = true; }
    }

    int Engine::activeVoices() const
    {
        int n = 0;
        for (auto& v : voices) n += v.active ? 1 : 0;
        return n;
    }

    // Fills `candidates` with the zones to play: all matching non-RR zones plus one RR position.
    const Zone* Engine::pickZone (const std::vector<Zone>& zones, int note, int vel, bool keys, RRMode mode, int ci)
    {
        candidates.clear();
        int positions[64]; int numPos = 0;
        for (auto& z : zones)
        {
            if (vel < z.loVel || vel > z.hiVel) continue;
            if (keys && (note < z.loKey || note > z.hiKey)) continue;
            if (! z.sample) continue;
            candidates.push_back (&z);
            if (z.rrPos > 0)
            {
                bool seen = false;
                for (int i = 0; i < numPos; ++i) seen = seen || positions[i] == z.rrPos;
                if (! seen && numPos < 64) positions[numPos++] = z.rrPos;
            }
        }
        if (candidates.empty()) return nullptr;
        if (numPos > 0)
        {
            std::sort (positions, positions + numPos);
            int idx = 0;
            if (mode == RRMode::Sequential) idx = rrCounter[ci]++ % numPos;
            else
            {
                idx = (int) (rng() % (unsigned) numPos);
                if (mode == RRMode::RandomNoRepeat && numPos > 1 && positions[idx] == rrLast[ci]) idx = (idx + 1 + (int) (rng() % (unsigned) (numPos - 1))) % numPos;
            }
            const int chosen = positions[idx];
            rrLast[ci] = chosen;
            candidates.erase (std::remove_if (candidates.begin(), candidates.end(), [chosen] (const Zone* z) { return z->rrPos > 0 && z->rrPos != chosen; }),
                              candidates.end());
        }
        if (candidates.size() > 8) candidates.resize (8);
        return candidates.front();
    }

    void Engine::noteOn (int ch, int note, int vel, bool toPads, bool toKeys)
    {
        if (! inst) return;
        if (toPads)
            for (int i = 0; i < kPads; ++i)
            {
                const auto& p = inst->pads[i];
                if (p.note != note || p.isEmpty()) continue;
                if (p.chokeGroup > 0)
                    for (auto& v : voices)
                        if (v.active && v.fromPad && v.chokeGroup == p.chokeGroup) v.fading = true;
                for (auto& layer : p.synths)
                    if (layer.on) startSynth (layer, p, note, vel, ch, i);
                if (! p.zones.empty() && pickZone (p.zones, note, vel, false, p.voicing.rr, i) != nullptr)
                {
                    const Zone* zs[8]; const int nz = (int) candidates.size();
                    std::copy (candidates.begin(), candidates.end(), zs);
                    for (int k = 0; k < nz; ++k) startVoice (*zs[k], p.voicing, note, vel, ch, true, i, p.chokeGroup, p.oneShot, p.sampleLevelDb, p.output);
                }
                padHits[i].fetch_add (1);
            }
        if (toKeys)
        {
            const auto& k = inst->keys;
            if (! k.zones.empty() && pickZone (k.zones, note, vel, true, k.voicing.rr, kPads + note) != nullptr)
            {
                const Zone* zs[8]; const int nz = (int) candidates.size();
                std::copy (candidates.begin(), candidates.end(), zs);
                for (int j = 0; j < nz; ++j) startVoice (*zs[j], k.voicing, note, vel, ch, false, -1, 0, false, 0.0f, k.output);
            }
        }
    }

    void Engine::noteOff (int ch, int note, bool pads, bool keys)
    {
        for (auto& v : voices)
            if (v.active && ! v.released && ! v.oneShot && v.note == note && v.channel == ch && (v.fromPad ? pads : keys))
            {
                v.env.noteOff();
                v.released = true;
            }
    }

    Voice& Engine::freeVoice()
    {
        for (auto& x : voices) if (! x.active) return x;
        Voice* v = &voices[0];
        for (auto& x : voices) if (x.age < v->age) v = &x;
        endVoice (*v);
        return *v;
    }

    static void setPanGains (Voice& v, float gain, float pan)
    {
        const float ang = (juce::jlimit (-1.0f, 1.0f, pan) + 1.0f) * juce::MathConstants<float>::pi * 0.25f;
        v.gainL = gain * std::cos (ang) * 1.41421356f;
        v.gainR = gain * std::sin (ang) * 1.41421356f;
    }

    Voice* Engine::startVoice (const Zone& z, const Voicing& vc, int note, int vel, int ch, bool fromPad, int padIndex, int choke, bool oneShot, float levelDb, int output)
    {
        Voice* v = &freeVoice();
        v->active = true; v->released = false; v->fading = false; v->fastFade = 1.0f; v->synth = false;
        v->fromPad = fromPad; v->oneShot = oneShot; v->note = note; v->channel = ch; v->padIndex = padIndex; v->chokeGroup = choke;
        v->output = output;
        v->age = ++ageCounter;
        v->owner = inst;
        v->sample = z.sample;
        v->zone = z;
        v->voicing = &vc;
        const double semis = (fromPad ? 0.0 : (double) (note - z.root)) + z.tuneCents / 100.0 + vc.tuneSemis;
        v->inc = std::pow (2.0, semis / 12.0) * z.sample->sampleRate / sampleRate;
        v->pos = juce::jlimit (0, juce::jmax (0, z.sample->length() - 2), z.start);
        v->lpos = 0;
        v->slot = -1;
        if (z.sample->streamed && streamer != nullptr)
            v->slot = streamer->open (z.sample, (int) v->pos, z.loop, z.loopStart, z.loopEnd);
        const float vn = vel / 127.0f;
        const float velGain = 1.0f - vc.velSens + vc.velSens * vn * vn;
        setPanGains (*v, juce::Decibels::decibelsToGain (z.gainDb + vc.volumeDb + levelDb) * velGain, z.pan + vc.pan);
        juce::ADSR::Parameters ap;
        ap.attack = vc.env.attack; ap.decay = vc.env.decay; ap.sustain = vc.env.sustain; ap.release = vc.env.release;
        v->env.setParameters (ap);
        v->env.reset();
        v->env.noteOn();
        for (auto& f : v->filt) { f.reset(); f.set (vc.filter.cutoff, vc.filter.resonance, sampleRate); }
        return v;
    }

    void Engine::startSynth (const SynthLayer& layer, const Pad& p, int note, int vel, int ch, int padIndex)
    {
        Voice& v = freeVoice();
        v.active = true; v.released = false; v.fading = false; v.fastFade = 1.0f; v.synth = true;
        v.fromPad = true; v.oneShot = true; v.note = note; v.channel = ch; v.padIndex = padIndex; v.chokeGroup = p.chokeGroup;
        v.output = p.output;
        v.age = ++ageCounter;
        v.owner = inst;
        v.sample.reset();
        v.voicing = &p.voicing;
        v.slot = -1;
        setPanGains (v, juce::Decibels::decibelsToGain (p.voicing.volumeDb), layer.pan + p.voicing.pan);
        SynthLayer l = layer;
        l.tuneSemis += p.voicing.tuneSemis;
        v.drum.start (l, sampleRate, vel / 127.0f, (uint32_t) (ageCounter * 2654435761u + 1u));
    }

    static inline float hermite (const float* d, int len, double pos) noexcept
    {
        const int i = (int) pos;
        const float t = (float) (pos - i);
        const float xm1 = d[juce::jmax (0, i - 1)], x0 = d[i], x1 = d[juce::jmin (len - 1, i + 1)], x2 = d[juce::jmin (len - 1, i + 2)];
        const float c1 = 0.5f * (x1 - xm1), c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2, c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        return ((c3 * t + c2) * t + c1) * t + x0;
    }

    void Engine::renderSample (Voice& v, float* L, float* R, int n)
    {
        const auto& data = v.sample->data;
        const int len = data.getNumSamples();
        const float* dl = data.getReadPointer (0);
        const float* dr = data.getNumChannels() > 1 ? data.getReadPointer (1) : dl;
        const bool loop = v.zone.loop && v.zone.loopEnd > v.zone.loopStart + 4 && v.zone.loopEnd < len;
        const auto ft = v.voicing->filter.type;
        const float fadeStep = 1.0f / (float) (0.004 * sampleRate);
        for (int i = 0; i < n; ++i)
        {
            if (loop && v.pos >= v.zone.loopEnd) v.pos -= (v.zone.loopEnd - v.zone.loopStart);
            if (v.pos >= len - 1) { v.active = false; break; }
            float sl = hermite (dl, len, v.pos), sr = dr == dl ? sl : hermite (dr, len, v.pos);
            v.pos += v.inc;
            const float e = v.env.getNextSample();
            if (! v.env.isActive()) { v.active = false; break; }
            if (v.fading) { v.fastFade -= fadeStep; if (v.fastFade <= 0.0f) { v.active = false; break; } }
            if (ft != FilterType::Off) { sl = v.filt[0].process (sl, ft); sr = v.filt[1].process (sr, ft); }
            const float g = e * v.fastFade;
            if (R) { L[i] += sl * g * v.gainL; R[i] += sr * g * v.gainR; }
            else L[i] += 0.5f * (sl * v.gainL + sr * v.gainR) * g;
        }
    }

    // Long samples: the head comes from memory, the rest from the voice's Streamer slot.
    // v.lpos counts frames played (loops included); Streamer::fileFrame maps it to the file.
    void Engine::renderStreamed (Voice& v, float* L, float* R, int n)
    {
        const auto& s = *v.sample;
        const int head = s.data.getNumSamples(), total = s.totalFrames, start = (int) v.pos;
        const bool loop = v.zone.loop && v.zone.loopEnd > v.zone.loopStart + 4 && v.zone.loopEnd <= total;
        const int ls = v.zone.loopStart, le = v.zone.loopEnd;
        const int chans = s.data.getNumChannels();
        const auto ft = v.voicing->filter.type;
        const float fadeStep = 1.0f / (float) (0.004 * sampleRate);
        bool under = false;

        if (offline && v.slot >= 0 && streamer != nullptr)
        {
            // Bouncing: wait for the disk instead of dropping out (bounded so a missing file can't hang the host).
            const int64_t need = (int64_t) (v.lpos + v.inc * n) + 4;
            streamer->setReadPos (v.slot, juce::jmax<int64_t> (0, (int64_t) v.lpos - 2));
            streamer->wake();
            for (int tries = 0; tries < 2000 && ! streamer->ready (v.slot, juce::jmin<int64_t> (need, (int64_t) v.lpos + Streamer::kRing - Streamer::kChunk)); ++tries)
                juce::Thread::sleep (1);
        }

        auto frame = [&] (int64_t li, int c) -> float
        {
            if (li < 0) li = 0;
            const int f = Streamer::fileFrame (li, start, loop, ls, le);
            if (f >= total) return 0.0f;
            if (f < head) return s.data.getSample (juce::jmin (c, chans - 1), f);
            float x = 0.0f;
            if (v.slot < 0 || streamer == nullptr || ! streamer->read (v.slot, li, c, x)) { under = true; return 0.0f; }
            return x;
        };

        for (int i = 0; i < n; ++i)
        {
            const int64_t li = (int64_t) v.lpos;
            if (! loop && start + li >= total - 1) { v.active = false; break; }
            const float t = (float) (v.lpos - (double) li);
            float out[2];
            for (int c = 0; c < 2; ++c)
            {
                if (c == 1 && chans == 1) { out[1] = out[0]; break; }
                const float xm1 = frame (li - 1, c), x0 = frame (li, c), x1 = frame (li + 1, c), x2 = frame (li + 2, c);
                const float c1 = 0.5f * (x1 - xm1), c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2, c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
                out[c] = ((c3 * t + c2) * t + c1) * t + x0;
            }
            v.lpos += v.inc;
            const float e = v.env.getNextSample();
            if (! v.env.isActive()) { v.active = false; break; }
            if (v.fading) { v.fastFade -= fadeStep; if (v.fastFade <= 0.0f) { v.active = false; break; } }
            float sl = out[0], sr = out[1];
            if (ft != FilterType::Off) { sl = v.filt[0].process (sl, ft); sr = v.filt[1].process (sr, ft); }
            const float g = e * v.fastFade;
            if (R) { L[i] += sl * g * v.gainL; R[i] += sr * g * v.gainR; }
            else L[i] += 0.5f * (sl * v.gainL + sr * v.gainR) * g;
        }
        if (v.slot >= 0 && streamer != nullptr)
        {
            streamer->setReadPos (v.slot, juce::jmax<int64_t> (0, (int64_t) v.lpos - 2));
            if (under) streamer->underruns.fetch_add (1);
        }
    }

    void Engine::render (juce::AudioBuffer<float>& out, int start, int n, const int* busChannel, int numBuses)
    {
        const int nch = out.getNumChannels();
        auto ptrs = [&] (int output, float*& L, float*& R)
        {
            int c = (output >= 0 && output < numBuses && busChannel[output] >= 0) ? busChannel[output] : busChannel[0];
            c = juce::jlimit (0, juce::jmax (0, nch - 1), c);
            L = out.getWritePointer (c, start);
            R = c + 1 < nch ? out.getWritePointer (c + 1, start) : nullptr;
        };
        const float fadeStep = 1.0f / (float) (0.004 * sampleRate);
        for (auto& v : voices)
        {
            if (! v.active) continue;
            float* L; float* R;
            ptrs (v.output, L, R);
            if (v.synth)
            {
                for (int done = 0; done < n;)
                {
                    const int m = juce::jmin (n - done, (int) scratch.size());
                    const bool alive = v.drum.render (scratch.data(), m);
                    for (int i = 0; i < m; ++i)
                    {
                        if (v.fading) { v.fastFade -= fadeStep; if (v.fastFade <= 0.0f) { v.fastFade = 0.0f; v.active = false; } }
                        const float x = scratch[(size_t) i] * v.fastFade;
                        if (R) { L[done + i] += x * v.gainL; R[done + i] += x * v.gainR; }
                        else L[done + i] += 0.5f * x * (v.gainL + v.gainR);
                    }
                    done += m;
                    if (! alive) v.active = false;
                    if (! v.active) break;
                }
            }
            else if (v.sample->streamed) renderStreamed (v, L, R, n);
            else renderSample (v, L, R, n);
            if (! v.active) endVoice (v);
        }
    }
}
