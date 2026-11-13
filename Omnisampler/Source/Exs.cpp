// EXS24 / Logic Sampler (.exs) import. Block layout follows the open-source ConvertWithMoss reader
// (format/exs): 84-byte block header, then zone / group / sample / params blocks.
#include "MoreFormats.h"
#include <cmath>
#include <map>

namespace omni
{
    using namespace juce;

    namespace
    {
        // Bounds-checked reads over one block's content, in the block's endianness.
        struct Blk
        {
            const uint8* p = nullptr;
            size_t n = 0;
            bool big = true;

            bool has (size_t o, size_t len) const noexcept { return o <= n && len <= n - o; }
            int u8 (size_t o) const noexcept { return has (o, 1) ? (int) p[o] : 0; }
            int s8 (size_t o) const noexcept { return (int) (int8) (uint8) u8 (o); }
            uint32 u16 (size_t o) const noexcept
            {
                if (! has (o, 2)) return 0;
                return big ? (((uint32) p[o] << 8) | (uint32) p[o + 1]) : ((uint32) p[o] | ((uint32) p[o + 1] << 8));
            }
            int s16 (size_t o) const noexcept { return (int) (int16) (uint16) u16 (o); }
            uint32 u32 (size_t o) const noexcept
            {
                if (! has (o, 4)) return 0;
                return big ? (((uint32) p[o] << 24) | ((uint32) p[o + 1] << 16) | ((uint32) p[o + 2] << 8) | (uint32) p[o + 3])
                           : ((uint32) p[o] | ((uint32) p[o + 1] << 8) | ((uint32) p[o + 2] << 16) | ((uint32) p[o + 3] << 24));
            }
            int s32 (size_t o) const noexcept { return (int) u32 (o); }
            String str (size_t o, size_t len) const
            {
                if (! has (o, len)) return {};
                std::string s;
                for (size_t i = 0; i < len && p[o + i] != 0; ++i) s.push_back ((char) p[o + i]);
                return String::fromUTF8 (s.c_str(), (int) s.size()).trim();
            }
        };

        struct ExsZone
        {
            bool oneshot = false, velOn = false;
            int key = 60, fine = 0, pan = 0, vol = 0, coarse = 0;
            int keyLow = 0, keyHigh = 127, velLow = 0, velHigh = 127;
            int sampleStart = 0, loopStart = 0, loopEnd = 0;
            bool loopOn = false;
            int group = -1, sample = -1;
        };
        struct ExsSample { String path, name; };
        struct ExsGroup
        {
            int vol = 0, pan = 0, minVel = 0, maxVel = 127, startNote = 0, endNote = 127;
            bool mute = false, releaseTrigger = false, roundRobin = false;
            int rrPos = -1;
        };

        constexpr size_t kHeader = 84;

        // Finds sample files below a folder, case-insensitively (built lazily, capped).
        struct FileIndex
        {
            std::map<String, File> byName, byStem;
            void scan (const File& root)
            {
                int count = 0;
                for (const auto& e : RangedDirectoryIterator (root, true, "*", File::findFiles))
                {
                    if (++count > 30000) break;
                    const auto f = e.getFile();
                    byName.emplace (f.getFileName().toLowerCase(), f);
                    if (isAudioFile (f)) byStem.emplace (f.getFileNameWithoutExtension().toLowerCase(), f);
                }
            }
            File find (const String& name) const
            {
                if (auto it = byName.find (name.toLowerCase()); it != byName.end()) return it->second;
                if (auto it = byStem.find (File::createFileWithoutCheckingPath (name).getFileNameWithoutExtension().toLowerCase()); it != byStem.end()) return it->second;
                return {};
            }
        };
    }

    bool loadExs (const File& file, SampleCache& cache, KeysMap& out, String& error)
    {
        MemoryBlock mb;
        if (! file.loadFileAsData (mb) || mb.getSize() < kHeader) { error = "Not an EXS24 file"; return false; }
        const auto* data = static_cast<const uint8*> (mb.getData());
        const size_t total = mb.getSize();

        std::vector<ExsZone> zones;
        std::vector<ExsSample> samples;
        std::vector<ExsGroup> groups;
        std::map<int, int> params;
        int blocksRead = 0;

        size_t pos = 0;
        while (pos + kHeader <= total)
        {
            const bool big = data[pos] == 0;
            const String magic ((const char*) data + pos + 16, 4);
            const bool magicOk = big ? (magic == "SOBT" || magic == "SOBJ") : (magic == "TBOS" || magic == "JBOS");
            if (! magicOk || (data[pos + 1] != 1 && data[pos + 2] != 0)) break;
            Blk hdr { data + pos, kHeader, big };
            const int typeFlags = hdr.u8 (3);
            const int type = typeFlags & 0x0F;
            uint32 size = hdr.u32 (4);
            if ((typeFlags & 0x80) != 0) size &= 0x7FFFu;
            const size_t start = pos + kHeader;
            if (size > total - start) break;       // truncated
            Blk b { data + start, (size_t) size, big };
            const String blockName = hdr.str (20, 64);
            ++blocksRead;

            if (type == 0x01 && zones.size() < 40000)
            {
                ExsZone z;
                const int opts = b.u8 (0);
                z.oneshot = (opts & 1) != 0;
                z.velOn = (opts & 8) != 0;
                z.key = b.u8 (1); z.fine = b.s8 (2); z.pan = b.s8 (3); z.vol = b.s8 (4);
                z.keyLow = b.u8 (6); z.keyHigh = b.u8 (7);
                z.velLow = b.u8 (9); z.velHigh = b.u8 (10);
                z.sampleStart = b.s32 (12);
                z.loopStart = b.s32 (20); z.loopEnd = b.s32 (24);
                z.loopOn = (b.u8 (33) & 1) != 0;
                z.coarse = b.s8 (80);
                z.group = b.s32 (88);
                z.sample = b.s32 (92);
                if (b.has (0, 96)) zones.push_back (z);
            }
            else if (type == 0x02)
            {
                ExsGroup g;
                g.vol = b.s8 (0); g.pan = b.s8 (1);
                g.mute = (b.u8 (3) & 16) != 0;
                g.minVel = b.u8 (5); g.maxVel = b.u8 (6);
                g.releaseTrigger = b.u8 (73) > 0;
                if (b.has (0, 92))
                {
                    g.rrPos = b.s32 (80);
                    g.roundRobin = b.u8 (84) == 2 && g.rrPos >= -1;
                    g.startNote = b.u8 (88); g.endNote = b.u8 (89);
                }
                groups.push_back (g);
            }
            else if (type == 0x03)
            {
                ExsSample s;
                s.path = b.str (80, 256);
                s.name = b.has (336, 256) ? b.str (336, 256) : blockName;
                if (s.name.isEmpty()) s.name = blockName;
                samples.push_back (s);
            }
            else if (type == 0x04)
            {
                const size_t count = b.u32 (0);
                if (count > 0 && count <= 4096 && b.has (4, count * 3))
                    for (size_t i = 0; i < count; ++i)
                    {
                        const int id = b.u8 (4 + i);
                        if (id != 0) params[id] = b.s16 (4 + count + 2 * i);
                    }
            }
            pos = start + size;
        }

        if (blocksRead == 0) { error = "Not an EXS24 file"; return false; }
        if (zones.empty() || samples.empty())
        { error = "This EXS file uses a format Omnisampler can't read yet"; return false; }

        // Some files list zones without sample indices: then zones play the samples in order.
        bool anyIndexed = false;
        for (auto& z : zones) if (z.sample >= 0) anyIndexed = true;
        if (! anyIndexed) for (size_t i = 0; i < zones.size(); ++i) zones[i].sample = (int) i;

        const auto dir = file.getParentDirectory();
        FileIndex idxHere, idxUp;
        bool scannedHere = false, scannedUp = false;
        auto locate = [&] (const ExsSample& s) -> File
        {
            const String name = s.name.replaceCharacter ('\\', '/').fromLastOccurrenceOf ("/", false, false);
            if (name.isEmpty()) return {};
            const String path = s.path.replaceCharacter ('\\', '/');
            if (path.isNotEmpty() && File::isAbsolutePath (path))
            {
                auto f = File (path).getChildFile (name);
                if (f.existsAsFile()) return f;
            }
            for (const auto& f : { dir.getChildFile (name), dir.getChildFile ("Samples").getChildFile (name), dir.getSiblingFile ("Samples").getChildFile (name) })
                if (f.existsAsFile()) return f;
            if (! scannedHere) { idxHere.scan (dir); scannedHere = true; }
            auto f = idxHere.find (name);
            if (f != File()) return f;
            if (! scannedUp && dir.getParentDirectory() != dir) { idxUp.scan (dir.getParentDirectory()); scannedUp = true; }
            return idxUp.find (name);
        };

        auto param = [&params] (int id, int def) { auto it = params.find (id); return it != params.end() ? it->second : def; };
        const float globalTune = 100.0f * (float) (param (0x2d, 0) + param (0x0e, 0)) + (float) param (0x0f, 0);

        std::map<int, SamplePtr> loaded;
        std::map<int, bool> tried;
        String firstErr;
        KeysMap km;
        km.name = file.getFileNameWithoutExtension();
        km.voicing.env = out.voicing.env;

        for (auto& ez : zones)
        {
            if (ez.sample < 0 || (size_t) ez.sample >= samples.size()) continue;
            const ExsGroup* g = (ez.group >= 0 && (size_t) ez.group < groups.size()) ? &groups[(size_t) ez.group] : nullptr;
            if (g != nullptr && (g->mute || g->releaseTrigger)) continue;

            if (! tried[ez.sample])
            {
                tried[ez.sample] = true;
                const auto& es = samples[(size_t) ez.sample];
                const auto f = locate (es);
                String err;
                if (f == File()) err = "Missing: " + es.name;
                else loaded[ez.sample] = cache.get (f, err);
                if (loaded[ez.sample] == nullptr && firstErr.isEmpty()) firstErr = es.name;
            }
            auto sp = loaded[ez.sample];
            if (sp == nullptr) continue;

            Zone z;
            z.sample = sp;
            z.loKey = ez.keyLow; z.hiKey = ez.keyHigh; z.root = ez.key;
            z.loVel = ez.velOn ? ez.velLow : 1; z.hiVel = ez.velOn ? ez.velHigh : 127;
            z.tuneCents = 100.0f * (float) ez.coarse + (float) ez.fine + globalTune;
            z.gainDb = (float) ez.vol;
            z.pan = (float) jlimit (-50, 50, ez.pan) / 50.0f;
            z.start = jmax (0, ez.sampleStart);
            if (g != nullptr)
            {
                z.gainDb += (float) g->vol;
                z.pan = jlimit (-1.0f, 1.0f, z.pan + (float) g->pan / 50.0f);
                if (g->minVel > 0) z.loVel = jmax (z.loVel, g->minVel);
                if (g->maxVel > 0) z.hiVel = jmin (z.hiVel, g->maxVel);
                if (g->startNote > 0) z.loKey = jmax (z.loKey, g->startNote);
                if (g->endNote > 0) z.hiKey = jmin (z.hiKey, g->endNote);
                if (g->roundRobin) z.rrPos = g->rrPos + 2;
            }
            z.loKey = jlimit (0, 127, z.loKey); z.hiKey = jlimit (0, 127, z.hiKey); z.root = jlimit (0, 127, z.root);
            z.loVel = jlimit (1, 127, z.loVel); z.hiVel = jlimit (1, 127, z.hiVel);
            if (z.loKey > z.hiKey || z.loVel > z.hiVel) continue;
            const int len = sp->length();
            if (ez.loopOn && ez.loopEnd > ez.loopStart + 4 && ez.loopStart >= 0)
            {
                z.loop = true;
                z.loopStart = ez.loopStart;
                z.loopEnd = jmin (ez.loopEnd, len - 1);
                if (z.loopEnd <= z.loopStart) z.loop = false;
            }
            z.start = jmin (z.start, jmax (0, len - 1));
            km.zones.push_back (z);
        }
        if (km.zones.empty()) { error = firstErr.isNotEmpty() ? "Missing samples, e.g. " + firstErr : "No playable zones found"; return false; }

        // Amp envelope (ENV1): times 10 s * (p/127)^4, sustain p/127.
        auto envTime = [] (int p) { const double x = (double) jlimit (0, 127, p) / 127.0; return (float) (10.0 * x * x * x * x); };
        if (params.count (0x52)) km.voicing.env.attack = jmax (0.0005f, envTime (param (0x52, 0)));
        if (params.count (0x54)) km.voicing.env.decay = jmax (0.01f, envTime (param (0x54, 0)));
        if (params.count (0x51)) km.voicing.env.sustain = (float) jlimit (0, 127, param (0x51, 127)) / 127.0f;
        if (params.count (0x55)) km.voicing.env.release = jmax (0.01f, envTime (param (0x55, 0)));

        out.zones = std::move (km.zones);
        out.name = km.name;
        out.voicing.env = km.voicing.env;
        if (firstErr.isNotEmpty()) error = "Some samples were missing, e.g. " + firstErr;
        return true;
    }
}
