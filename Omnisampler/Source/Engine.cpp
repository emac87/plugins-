#include "Engine.h"

namespace omni
{
    void Engine::prepare (double sr)
    {
        sampleRate = sr;
        for (auto& v : voices) { v.env.setSampleRate (sr); v.active = false; v.sample.reset(); v.owner.reset(); }
        candidates.reserve (512);
    }

    void Engine::handleMidi (const juce::MidiMessage& m, int padsCh, int keysCh)
    {
        if (m.isNoteOn()) noteOn (m.getChannel(), m.getNoteNumber(), m.getVelocity(), padsCh, keysCh);
        else if (m.isNoteOff()) noteOff (m.getChannel(), m.getNoteNumber());
        else if (m.isAllNotesOff() || m.isAllSoundOff()) allNotesOff();
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

    void Engine::noteOn (int ch, int note, int vel, int padsCh, int keysCh)
    {
        if (! inst) return;
        if (padsCh == 0 || ch == padsCh)
            for (int i = 0; i < kPads; ++i)
            {
                const auto& p = inst->pads[i];
                if (p.note != note || p.zones.empty()) continue;
                if (p.chokeGroup > 0)
                    for (auto& v : voices)
                        if (v.active && v.fromPad && v.chokeGroup == p.chokeGroup) v.fading = true;
                if (pickZone (p.zones, note, vel, false, p.voicing.rr, i) == nullptr) continue;
                const Zone* zs[8]; const int nz = (int) candidates.size();
                std::copy (candidates.begin(), candidates.end(), zs);
                for (int k = 0; k < nz; ++k) startVoice (*zs[k], p.voicing, note, vel, ch, true, i, p.chokeGroup, p.oneShot);
                padHits[i].fetch_add (1);
            }
        // Keys on "Any" leave the pads' own channel to the pads.
        if ((keysCh == 0 && (padsCh == 0 || ch != padsCh)) || ch == keysCh)
        {
            const auto& k = inst->keys;
            if (! k.zones.empty() && pickZone (k.zones, note, vel, true, k.voicing.rr, kPads + note) != nullptr)
            {
                const Zone* zs[8]; const int nz = (int) candidates.size();
                std::copy (candidates.begin(), candidates.end(), zs);
                for (int j = 0; j < nz; ++j) startVoice (*zs[j], k.voicing, note, vel, ch, false, -1, 0, false);
            }
        }
    }

    void Engine::noteOff (int ch, int note)
    {
        for (auto& v : voices)
            if (v.active && ! v.released && ! v.oneShot && v.note == note && v.channel == ch)
            {
                v.env.noteOff();
                v.released = true;
            }
    }

    void Engine::startVoice (const Zone& z, const Voicing& vc, int note, int vel, int ch, bool fromPad, int padIndex, int choke, bool oneShot)
    {
        Voice* v = nullptr;
        for (auto& x : voices) if (! x.active) { v = &x; break; }
        if (v == nullptr)
        {
            v = &voices[0];
            for (auto& x : voices) if (x.age < v->age) v = &x;
        }
        v->active = true; v->released = false; v->fading = false; v->fastFade = 1.0f;
        v->fromPad = fromPad; v->oneShot = oneShot; v->note = note; v->channel = ch; v->padIndex = padIndex; v->chokeGroup = choke;
        v->age = ++ageCounter;
        v->owner = inst;
        v->sample = z.sample;
        v->zone = z;
        v->voicing = &vc;
        const double semis = (fromPad ? 0.0 : (double) (note - z.root)) + z.tuneCents / 100.0 + vc.tuneSemis;
        v->inc = std::pow (2.0, semis / 12.0) * z.sample->sampleRate / sampleRate;
        v->pos = juce::jlimit (0, juce::jmax (0, z.sample->data.getNumSamples() - 2), z.start);
        const float vn = vel / 127.0f;
        const float velGain = 1.0f - vc.velSens + vc.velSens * vn * vn;
        const float g = juce::Decibels::decibelsToGain (z.gainDb + vc.volumeDb) * velGain;
        const float pan = juce::jlimit (-1.0f, 1.0f, z.pan + vc.pan);
        const float ang = (pan + 1.0f) * juce::MathConstants<float>::pi * 0.25f;
        v->gainL = g * std::cos (ang) * 1.41421356f;
        v->gainR = g * std::sin (ang) * 1.41421356f;
        juce::ADSR::Parameters ap;
        ap.attack = vc.env.attack; ap.decay = vc.env.decay; ap.sustain = vc.env.sustain; ap.release = vc.env.release;
        v->env.setParameters (ap);
        v->env.reset();
        v->env.noteOn();
        for (auto& f : v->filt) { f.reset(); f.set (vc.filter.cutoff, vc.filter.resonance, sampleRate); }
    }

    static inline float hermite (const float* d, int len, double pos) noexcept
    {
        const int i = (int) pos;
        const float t = (float) (pos - i);
        const float xm1 = d[juce::jmax (0, i - 1)], x0 = d[i], x1 = d[juce::jmin (len - 1, i + 1)], x2 = d[juce::jmin (len - 1, i + 2)];
        const float c1 = 0.5f * (x1 - xm1), c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2, c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        return ((c3 * t + c2) * t + c1) * t + x0;
    }

    void Engine::render (juce::AudioBuffer<float>& out, int start, int n)
    {
        float* L = out.getWritePointer (0, start);
        float* R = out.getNumChannels() > 1 ? out.getWritePointer (1, start) : nullptr;
        const float fadeStep = 1.0f / (float) (0.004 * sampleRate);
        for (auto& v : voices)
        {
            if (! v.active) continue;
            const auto& data = v.sample->data;
            const int len = data.getNumSamples();
            const float* dl = data.getReadPointer (0);
            const float* dr = data.getNumChannels() > 1 ? data.getReadPointer (1) : dl;
            const bool loop = v.zone.loop && v.zone.loopEnd > v.zone.loopStart + 4 && v.zone.loopEnd < len;
            const auto ft = v.voicing->filter.type;
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
            if (! v.active) { v.sample.reset(); v.owner.reset(); v.voicing = nullptr; }
        }
    }
}
