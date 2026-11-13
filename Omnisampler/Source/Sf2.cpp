// SoundFont 2 import (RIFF 'sfbk'): first preset -> instrument zones -> samples written as WAV files.
#include "MoreFormats.h"
#include <array>
#include <cmath>
#include <map>

namespace omni
{
    using namespace juce;

    namespace
    {
        // ---- bounds-checked little-endian reads ------------------------------------------------
        struct Bytes
        {
            const uint8* p = nullptr;
            size_t n = 0;
            bool has (size_t o, size_t len) const noexcept { return o <= n && len <= n - o; }
            uint32 u8 (size_t o) const noexcept { return has (o, 1) ? (uint32) p[o] : 0u; }
            uint32 u16 (size_t o) const noexcept { return has (o, 2) ? ((uint32) p[o] | ((uint32) p[o + 1] << 8)) : 0u; }
            uint32 u32 (size_t o) const noexcept
            {
                return has (o, 4) ? ((uint32) p[o] | ((uint32) p[o + 1] << 8) | ((uint32) p[o + 2] << 16) | ((uint32) p[o + 3] << 24)) : 0u;
            }
            int s16 (size_t o) const noexcept { return (int) (int16) (uint16) u16 (o); }
            int s8 (size_t o) const noexcept { return (int) (int8) (uint8) u8 (o); }
            String str (size_t o, size_t len) const
            {
                if (! has (o, len)) return {};
                char buf[64] = {};
                for (size_t i = 0; i < len && i < 63; ++i) buf[i] = (char) p[o + i];
                return String (buf).trim();
            }
            size_t count (size_t recSize) const noexcept { return n / recSize; }
        };

        struct Chunk { int64 pos = 0; int64 size = 0; };   // data position and size in the file

        constexpr int kStartAddrs = 0, kEndAddrs = 1, kStartLoop = 2, kEndLoop = 3, kStartCoarse = 4, kPan = 17,
                      kAttack = 34, kDecay = 36, kSustain = 37, kRelease = 38, kInstrument = 41, kKeyRange = 43,
                      kVelRange = 44, kStartLoopCoarse = 45, kAtten = 48, kEndLoopCoarse = 50, kCoarseTune = 51,
                      kFineTune = 52, kSampleID = 53, kSampleModes = 54, kRootKey = 58, kNumGens = 64;

        struct Gens
        {
            std::array<int, kNumGens> v {};
            std::array<bool, kNumGens> has {};
            bool hasKey = false, hasVel = false;
            int keyLo = 0, keyHi = 127, velLo = 0, velHi = 127;
            int terminal = -1;      // instrument / sampleID this zone points to
            bool any = false;

            int get (int g, int def) const noexcept { return has[(size_t) g] ? v[(size_t) g] : def; }
        };

        struct SampleHdr
        {
            String name;
            uint32 start = 0, end = 0, startLoop = 0, endLoop = 0, rate = 44100;
            int originalPitch = 60, correction = 0, link = 0, type = 1;
        };

        struct Sf2
        {
            Bytes phdr, pbag, pgen, inst, ibag, igen, shdr;
            Chunk smpl;
            String inam;
        };

        // Read the generators of one zone (index range into the gen list).
        Gens readGens (const Bytes& gen, size_t g0, size_t g1, bool instrumentLevel)
        {
            Gens g;
            const size_t total = gen.count (4);
            for (size_t i = g0; i < g1 && i < total; ++i)
            {
                const int oper = (int) gen.u16 (i * 4);
                const int lo = (int) gen.u8 (i * 4 + 2), hi = (int) gen.u8 (i * 4 + 3);
                const int amt = gen.s16 (i * 4 + 2);
                g.any = true;
                if (oper == kKeyRange) { g.hasKey = true; g.keyLo = lo; g.keyHi = hi; }
                else if (oper == kVelRange) { g.hasVel = true; g.velLo = lo; g.velHi = hi; }
                else if (oper == (instrumentLevel ? kSampleID : kInstrument)) { g.terminal = (int) (uint16) amt; break; }
                else if (oper >= 0 && oper < kNumGens) { g.v[(size_t) oper] = amt; g.has[(size_t) oper] = true; }
            }
            return g;
        }

        Gens overlay (const Gens& base, const Gens& local)
        {
            Gens r = base;
            for (size_t i = 0; i < (size_t) kNumGens; ++i)
                if (local.has[i]) { r.v[i] = local.v[i]; r.has[i] = true; }
            if (local.hasKey) { r.hasKey = true; r.keyLo = local.keyLo; r.keyHi = local.keyHi; }
            if (local.hasVel) { r.hasVel = true; r.velLo = local.velLo; r.velHi = local.velHi; }
            r.terminal = local.terminal;
            return r;
        }

        // Zones of a preset or instrument: bag range -> (global zone, local zones).
        struct ZoneSet { Gens global; bool hasGlobal = false; std::vector<Gens> zones; };

        ZoneSet readZones (const Bytes& bag, const Bytes& gen, size_t b0, size_t b1, bool instrumentLevel)
        {
            ZoneSet zs;
            const size_t nb = bag.count (4);
            for (size_t b = b0; b < b1 && b + 1 < nb; ++b)
            {
                const size_t g0 = bag.u16 (b * 4), g1 = bag.u16 ((b + 1) * 4);
                Gens g = readGens (gen, g0, jmax (g0, g1), instrumentLevel);
                if (g.terminal < 0)
                {
                    if (b == b0 && ! zs.hasGlobal) { zs.global = g; zs.hasGlobal = true; }
                    continue;
                }
                zs.zones.push_back (g);
            }
            return zs;
        }

        double timecentsToSeconds (int tc) { return std::pow (2.0, (double) jlimit (-12000, 8000, tc) / 1200.0); }

        struct Resolved
        {
            int sample = 0;
            int loKey = 0, hiKey = 127, loVel = 1, hiVel = 127, root = 60;
            float tune = 0.0f, gainDb = 0.0f, pan = 0.0f;
            bool loop = false;
            int loopStart = 0, loopEnd = 0, start = 0;
            Envelope env;
        };

        bool writeWav16 (const File& f, const AudioBuffer<float>& b, double sr)
        {
            f.deleteFile();
            WavAudioFormat wav;
            std::unique_ptr<OutputStream> os (new FileOutputStream (f));
            if (! static_cast<FileOutputStream*> (os.get())->openedOk()) return false;
            std::unique_ptr<AudioFormatWriter> w (wav.createWriterFor (os.get(), sr, (unsigned) b.getNumChannels(), 16, {}, 0));
            if (w == nullptr) return false;
            os.release();
            return w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
        }

        String safeName (const String& s)
        {
            return s.retainCharacters ("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.# ").trim().replaceCharacter (' ', '_').substring (0, 40);
        }
    }

    // ---------------------------------------------------------------------------------------------
    bool loadSf2 (const File& file, SampleCache& cache, KeysMap& out, String& error)
    {
        FileInputStream in (file);
        if (! in.openedOk()) { error = "Can't open the SoundFont file"; return false; }
        const int64 fileSize = in.getTotalLength();
        auto u32at = [&in] (int64 pos, uint32& v) -> bool
        {
            if (! in.setPosition (pos)) return false;
            uint8 b[4];
            if (in.read (b, 4) != 4) return false;
            v = (uint32) b[0] | ((uint32) b[1] << 8) | ((uint32) b[2] << 16) | ((uint32) b[3] << 24);
            return true;
        };
        auto idAt = [&in] (int64 pos, char* id) -> bool
        {
            if (! in.setPosition (pos)) return false;
            return in.read (id, 4) == 4;
        };

        char id[5] = {};
        uint32 riffSize = 0;
        if (fileSize < 12 || ! idAt (0, id) || String (id, 4) != "RIFF" || ! u32at (4, riffSize) || ! idAt (8, id) || String (id, 4) != "sfbk")
        { error = "Not a SoundFont file"; return false; }

        Sf2 sf;
        std::vector<MemoryBlock> store;
        store.reserve (16);
        auto grab = [&] (const Chunk& c, Bytes& dst) -> bool
        {
            if (c.size <= 0 || c.size > 256 * 1024 * 1024 || c.pos + c.size > fileSize) return false;
            store.emplace_back ((size_t) c.size);
            if (! in.setPosition (c.pos) || in.read (store.back().getData(), (int) c.size) != (int) c.size) return false;
            dst.p = static_cast<const uint8*> (store.back().getData());
            dst.n = (size_t) c.size;
            return true;
        };

        // Walk the chunks: LIST INFO / sdta / pdta.
        bool gotPdta = false;
        auto walk = [&] (int64 begin, int64 end, auto&& self, const String& listType) -> void
        {
            int64 pos = begin;
            while (pos + 8 <= end)
            {
                char cid[5] = {};
                uint32 size = 0;
                if (! idAt (pos, cid) || ! u32at (pos + 4, size)) return;
                const String name (cid, 4);
                const int64 dataPos = pos + 8;
                const int64 dataEnd = jmin<int64> (end, dataPos + (int64) size);
                if (name == "LIST")
                {
                    char lt[5] = {};
                    if (! idAt (dataPos, lt)) return;
                    const String type (lt, 4);
                    if (type == "INFO" || type == "sdta" || type == "pdta") self (dataPos + 4, dataEnd, self, type);
                }
                else
                {
                    Chunk c { dataPos, dataEnd - dataPos };
                    if (listType == "INFO" && name == "INAM")
                    {
                        MemoryBlock mb ((size_t) jlimit<int64> (0, 256, c.size));
                        if (in.setPosition (c.pos) && in.read (mb.getData(), (int) mb.getSize()) == (int) mb.getSize())
                            sf.inam = String::fromUTF8 (static_cast<const char*> (mb.getData()), (int) mb.getSize()).trim();
                    }
                    else if (listType == "sdta" && name == "smpl") sf.smpl = c;
                    else if (listType == "pdta")
                    {
                        if (name == "phdr") grab (c, sf.phdr);
                        else if (name == "pbag") grab (c, sf.pbag);
                        else if (name == "pgen") grab (c, sf.pgen);
                        else if (name == "inst") grab (c, sf.inst);
                        else if (name == "ibag") grab (c, sf.ibag);
                        else if (name == "igen") grab (c, sf.igen);
                        else if (name == "shdr") { grab (c, sf.shdr); gotPdta = true; }
                    }
                }
                pos = dataPos + (int64) size + (int64) (size & 1u);
            }
        };
        walk (12, jmin<int64> (fileSize, (int64) riffSize + 8), walk, {});

        if (! gotPdta || sf.smpl.size <= 0 || sf.phdr.count (38) < 2 || sf.inst.count (22) < 2 || sf.shdr.count (46) < 2)
        { error = "Not a SoundFont file"; return false; }

        // Sample headers.
        const size_t numSamples = sf.shdr.count (46) - 1;   // last is EOS
        std::vector<SampleHdr> hdrs (numSamples);
        for (size_t i = 0; i < numSamples; ++i)
        {
            const size_t o = i * 46;
            auto& h = hdrs[i];
            h.name = sf.shdr.str (o, 20);
            h.start = sf.shdr.u32 (o + 20); h.end = sf.shdr.u32 (o + 24);
            h.startLoop = sf.shdr.u32 (o + 28); h.endLoop = sf.shdr.u32 (o + 32);
            h.rate = sf.shdr.u32 (o + 36);
            h.originalPitch = (int) sf.shdr.u8 (o + 40);
            h.correction = sf.shdr.s8 (o + 41);
            h.link = (int) sf.shdr.u16 (o + 42);
            h.type = (int) sf.shdr.u16 (o + 44);
            if (h.rate < 400 || h.rate > 400000) h.rate = 44100;
        }
        const int64 smplFrames = sf.smpl.size / 2;
        auto usable = [&] (int idx) -> bool
        {
            if (idx < 0 || (size_t) idx >= numSamples) return false;
            const auto& h = hdrs[(size_t) idx];
            return (h.type & 0x8000) == 0 && h.end > h.start && (int64) h.end <= smplFrames && h.end - h.start < (1u << 28);
        };
        // Partner of a stereo sample, or -1.
        auto partner = [&] (int idx) -> int
        {
            const auto& h = hdrs[(size_t) idx];
            if (h.type != 4 && h.type != 2) return -1;
            if (! usable (h.link)) return -1;
            const auto& o = hdrs[(size_t) h.link];
            if ((h.type == 4 && o.type != 2) || (h.type == 2 && o.type != 4) || o.link != idx) return -1;
            return h.link;
        };

        // First preset (lowest bank, then program).
        const size_t numPresets = sf.phdr.count (38) - 1;
        size_t best = 0;
        for (size_t i = 1; i < numPresets; ++i)
        {
            const auto key = [&] (size_t k) { return (int) sf.phdr.u16 (k * 38 + 22) * 1000 + (int) sf.phdr.u16 (k * 38 + 20); };
            if (key (i) < key (best)) best = i;
        }
        const String presetName = sf.phdr.str (best * 38, 20);
        const ZoneSet pzs = readZones (sf.pbag, sf.pgen, sf.phdr.u16 (best * 38 + 24), sf.phdr.u16 ((best + 1) * 38 + 24), false);

        // Resolve preset zones -> instrument zones.
        std::vector<Resolved> res;
        const size_t numInst = sf.inst.count (22) - 1;
        for (auto& pzLocal : pzs.zones)
        {
            const Gens pz = pzs.hasGlobal ? overlay (pzs.global, pzLocal) : pzLocal;
            const size_t ii = (size_t) pz.terminal;
            if (ii >= numInst) continue;
            const ZoneSet izs = readZones (sf.ibag, sf.igen, sf.inst.u16 (ii * 22 + 20), sf.inst.u16 ((ii + 1) * 22 + 20), true);
            for (auto& izLocal : izs.zones)
            {
                const Gens iz = izs.hasGlobal ? overlay (izs.global, izLocal) : izLocal;
                if (! usable (iz.terminal)) continue;
                const auto& h = hdrs[(size_t) iz.terminal];

                Resolved r;
                r.sample = iz.terminal;
                r.loKey = jmax (iz.keyLo, pz.keyLo); r.hiKey = jmin (iz.keyHi, pz.keyHi);
                r.loVel = jmax (iz.velLo, pz.velLo, 1); r.hiVel = jmin (iz.velHi, pz.velHi, 127);
                if (r.loKey > r.hiKey || r.loVel > r.hiVel) continue;
                r.loKey = jlimit (0, 127, r.loKey); r.hiKey = jlimit (r.loKey, 127, r.hiKey);

                const int rk = iz.get (kRootKey, -1);
                r.root = (rk >= 0 && rk <= 127) ? rk : (h.originalPitch <= 127 ? h.originalPitch : 60);
                auto sum = [&] (int g, int def) { return iz.get (g, def) + pz.get (g, 0); };
                r.tune = 100.0f * (float) sum (kCoarseTune, 0) + (float) sum (kFineTune, 0) + (float) h.correction;
                // SF2 attenuation is in centibels; real players apply it at about 0.4x.
                r.gainDb = -0.4f * (float) jlimit (0, 1440, sum (kAtten, 0)) / 10.0f;
                r.pan = jlimit (-1.0f, 1.0f, (float) sum (kPan, 0) / 500.0f);

                const int64 len = (int64) h.end - (int64) h.start;
                r.start = (int) jlimit<int64> (0, len - 1, (int64) iz.get (kStartAddrs, 0) + 32768 * (int64) iz.get (kStartCoarse, 0));
                const int modes = iz.get (kSampleModes, 0) & 3;
                if (modes == 1 || modes == 3)
                {
                    const int64 ls = (int64) h.startLoop - (int64) h.start + iz.get (kStartLoop, 0) + 32768 * (int64) iz.get (kStartLoopCoarse, 0);
                    const int64 le = (int64) h.endLoop - (int64) h.start + iz.get (kEndLoop, 0) + 32768 * (int64) iz.get (kEndLoopCoarse, 0);
                    if (ls >= 0 && le > ls + 4 && le <= len)
                    {
                        r.loop = true;
                        r.loopStart = (int) ls;
                        r.loopEnd = (int) jmin<int64> (le, len - 1);
                    }
                }
                r.env.attack = jmax (0.0005f, (float) timecentsToSeconds (sum (kAttack, -12000)));
                r.env.decay = jmax (0.01f, (float) timecentsToSeconds (sum (kDecay, -12000)));
                r.env.sustain = (float) std::pow (10.0, -(double) jlimit (0, 1440, sum (kSustain, 0)) / 200.0);
                r.env.release = jmax (0.01f, (float) timecentsToSeconds (sum (kRelease, -12000)));
                res.push_back (r);
            }
        }
        if (res.empty()) { error = "This SoundFont has no playable samples (empty preset or ROM samples)"; return false; }

        // Stereo pairs: keep the left zone (it makes a stereo file), drop a right zone whose left twin is present.
        std::vector<char> used (numSamples, 0);
        for (auto& r : res) used[(size_t) r.sample] = 1;
        std::vector<Resolved> kept;
        for (auto& r : res)
        {
            const int p = partner (r.sample);
            if (p >= 0 && hdrs[(size_t) r.sample].type == 2)
            {
                if (used[(size_t) p]) continue;    // the left zone covers it
                Resolved c = r;
                c.sample = p;                       // lone right zone: play the pair as stereo from its left twin
                kept.push_back (c);
                continue;
            }
            kept.push_back (r);
        }
        // A merged stereo zone is centred (left/right zones are normally panned hard).
        for (auto& r : kept)
            if (partner (r.sample) >= 0) r.pan = 0.0f;

        // Extract samples to WAV files (each once).
        const String base = file.getFileNameWithoutExtension();
        const File folder = extractedSamplesFolder (base);
        folder.createDirectory();
        std::map<int, File> files;
        String firstErr;
        auto extract = [&] (int idx, File& dst) -> bool
        {
            if (auto it = files.find (idx); it != files.end()) { dst = it->second; return dst != File(); }
            const auto& h = hdrs[(size_t) idx];
            const int p = partner (idx);
            const size_t frames = (size_t) (p >= 0 ? jmin (h.end - h.start, hdrs[(size_t) p].end - hdrs[(size_t) p].start) : h.end - h.start);
            const int chans = p >= 0 ? 2 : 1;
            const File f = folder.getChildFile (String (idx).paddedLeft ('0', 3) + "_" + safeName (h.name) + (chans == 2 ? "_st" : "") + ".wav");
            files[idx] = File();
            const int64 expected = 44 + (int64) (frames * (size_t) chans * 2);
            if (! (f.existsAsFile() && f.getSize() >= expected && f.getSize() <= expected + 2048))
            {
                AudioBuffer<float> buf (chans, (int) frames);
                for (int c = 0; c < chans; ++c)
                {
                    const auto& hh = hdrs[(size_t) (c == 0 ? idx : p)];
                    MemoryBlock mb (frames * 2);
                    if (! in.setPosition (sf.smpl.pos + (int64) hh.start * 2) || in.read (mb.getData(), (int) mb.getSize()) != (int) mb.getSize()) return false;
                    const auto* src = static_cast<const uint8*> (mb.getData());
                    auto* d = buf.getWritePointer (c);
                    for (size_t i = 0; i < frames; ++i)
                        d[i] = (float) (int16) (uint16) ((uint32) src[i * 2] | ((uint32) src[i * 2 + 1] << 8)) / 32768.0f;
                }
                if (! writeWav16 (f, buf, (double) h.rate)) return false;
            }
            files[idx] = f;
            dst = f;
            return true;
        };

        KeysMap km;
        km.name = sf.inam.isNotEmpty() ? sf.inam : (presetName.isNotEmpty() ? presetName : base);
        km.voicing.env = out.voicing.env;
        bool envTaken = false;
        for (auto& r : kept)
        {
            File wav;
            if (! extract (r.sample, wav))
            {
                if (firstErr.isEmpty()) firstErr = hdrs[(size_t) r.sample].name;
                continue;
            }
            String err;
            auto sp = cache.get (wav, err);
            if (sp == nullptr) { if (firstErr.isEmpty()) firstErr = hdrs[(size_t) r.sample].name; continue; }
            Zone z;
            z.sample = sp;
            z.loKey = r.loKey; z.hiKey = r.hiKey; z.root = jlimit (0, 127, r.root);
            z.loVel = r.loVel; z.hiVel = r.hiVel;
            z.tuneCents = r.tune; z.gainDb = r.gainDb; z.pan = r.pan;
            z.start = r.start;
            const int len = sp->length();
            if (r.loop && r.loopEnd < len) { z.loop = true; z.loopStart = r.loopStart; z.loopEnd = r.loopEnd; }
            if (! envTaken) { km.voicing.env = r.env; envTaken = true; }
            km.zones.push_back (z);
        }
        if (km.zones.empty()) { error = firstErr.isNotEmpty() ? "Couldn't extract the samples, e.g. " + firstErr : "No playable zones found"; return false; }
        out.zones = std::move (km.zones);
        out.name = km.name;
        out.voicing.env = km.voicing.env;
        if (firstErr.isNotEmpty()) error = "Some samples were missing, e.g. " + firstErr;
        return true;
    }
}
