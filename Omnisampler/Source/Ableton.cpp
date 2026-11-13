// Ableton Live Sampler / Simpler presets (.adv): a gzip-compressed XML document.
// Structure (Live 9 - 12):  <Ableton><MultiSampler|OriginalSimpler><Player><MultiSampleMap><SampleParts>
//   <MultiSamplePart> with KeyRange, VelocityRange, SelectorRange, RootKey, Detune, Panorama, Volume,
//   SampleStart, SustainLoop and SampleRef/FileRef (RelativePath / Path / Name). The element names and
//   the way the sample references are resolved follow the open-source ConvertWithMoss project
//   (git-moss/ConvertWithMoss, package format.ableton).
#include "MoreFormats.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <map>

namespace omni
{
    using namespace juce;

    namespace
    {
        constexpr int kMaxSearchFiles = 20000;     // cap for the by-name sample search

        // ----- XML helpers: most values are <Name Value="..."/> children --------------------------
        const XmlElement* child (const XmlElement* parent, const char* name)
        {
            return parent != nullptr ? parent->getChildByName (name) : nullptr;
        }

        String valueOf (const XmlElement* parent, const char* name)
        {
            if (auto* e = child (parent, name))
                return e->getStringAttribute ("Value");
            return {};
        }

        double numberOf (const XmlElement* parent, const char* name, double def)
        {
            auto s = valueOf (parent, name).trim();
            return s.isEmpty() ? def : s.getDoubleValue();
        }

        int intOf (const XmlElement* parent, const char* name, int def)
        {
            auto s = valueOf (parent, name).trim();
            return s.isEmpty() ? def : (int) s.getDoubleValue();
        }

        // Envelope parameters are <AttackTime><Manual Value="..."/></AttackTime>.
        double manualOf (const XmlElement* envelope, const char* name, double def)
        {
            auto* e = child (envelope, name);
            if (e == nullptr) return def;
            if (auto* m = e->getChildByName ("Manual"))
            {
                auto s = m->getStringAttribute ("Value").trim();
                if (s.isNotEmpty()) return s.getDoubleValue();
            }
            return def;
        }

        const XmlElement* findDescendant (const XmlElement* e, const char* name)
        {
            if (e == nullptr) return nullptr;
            if (e->hasTagName (name)) return e;
            for (auto* c : e->getChildIterator())
                if (auto* r = findDescendant (c, name))
                    return r;
            return nullptr;
        }

        // The Sampler / Simpler device: directly below <Ableton>, or inside a rack.
        const XmlElement* findDevice (const XmlElement& root)
        {
            for (auto* tag : { "MultiSampler", "OriginalSimpler" })
                if (auto* d = child (&root, tag))
                    return d;
            for (auto* tag : { "MultiSampler", "OriginalSimpler" })
                if (auto* d = findDescendant (&root, tag))
                    if (findDescendant (d, "MultiSamplePart") != nullptr)
                        return d;
            return nullptr;
        }

        // ----- sample files --------------------------------------------------------------------
        // "RelativePath" is a Value attribute in Live 11/12 and a list of <RelativePathElement Dir=".."/>
        // plus the file Name in older versions (an empty Dir means "go up one folder").
        String relativePathOf (const XmlElement* fileRef)
        {
            auto* rel = child (fileRef, "RelativePath");
            if (rel == nullptr) return {};
            auto v = rel->getStringAttribute ("Value");
            if (v.isNotEmpty()) return v;
            auto name = valueOf (fileRef, "Name");
            if (name.isEmpty()) return {};
            String path;
            for (auto* el : rel->getChildWithTagNameIterator ("RelativePathElement"))
            {
                auto dir = el->getStringAttribute ("Dir");
                path << (dir.isEmpty() ? String ("..") : dir) << "/";
            }
            return path + name;
        }

        String absolutePathOf (const XmlElement* fileRef)
        {
            auto p = valueOf (fileRef, "Path");
            if (p.isNotEmpty()) return p;
            auto name = valueOf (fileRef, "Name");
            auto* hint = child (child (fileRef, "SearchHint"), "PathHint");
            if (name.isEmpty() || hint == nullptr) return {};
            StringArray parts;
            for (auto* el : hint->getChildWithTagNameIterator ("RelativePathElement"))
                parts.add (el->getStringAttribute ("Dir"));
            if (parts.isEmpty()) return {};
            parts.add (name);
            auto joined = parts.joinIntoString ("/");
            return parts[0].endsWithChar (':') ? joined : "/" + joined;
        }

        // Finds a file by name below a folder; the search is bounded so a huge disk can't stall the load.
        File findByName (const File& folder, const String& name)
        {
            if (! folder.isDirectory()) return {};
            int visited = 0;
            for (const auto& f : RangedDirectoryIterator (folder, true, "*", File::findFiles, File::FollowSymlinks::no))
            {
                if (++visited > kMaxSearchFiles) break;
                if (f.getFile().getFileName().equalsIgnoreCase (name))
                    return f.getFile();
            }
            return {};
        }

        File resolveSample (const XmlElement* fileRef, const File& presetFile)
        {
            // 1. an absolute path that still exists
            const auto abs = absolutePathOf (fileRef);
            if (abs.isNotEmpty() && File::isAbsolutePath (abs))
            {
                File f (abs);
                if (f.existsAsFile()) return f;
            }

            const auto dir = presetFile.getParentDirectory();
            const auto rel = relativePathOf (fileRef).replaceCharacter ('\\', '/');
            const auto name = rel.isNotEmpty() ? rel.fromLastOccurrenceOf ("/", false, false)
                                               : (abs.isNotEmpty() ? abs.replaceCharacter ('\\', '/').fromLastOccurrenceOf ("/", false, false)
                                                                   : valueOf (fileRef, "Name"));

            // 2. the relative path, against the preset's folder and then each folder above it
            //    (presets sit deep inside a project or a User Library, samples live in its "Samples" folder)
            if (rel.isNotEmpty() && ! File::isAbsolutePath (rel))
            {
                auto base = dir;
                for (int up = 0; up < 8 && base.exists(); ++up)
                {
                    auto f = base.getChildFile (rel);
                    if (f.existsAsFile()) return f;
                    auto parent = base.getParentDirectory();
                    if (parent == base) break;
                    base = parent;
                }
            }

            if (name.isEmpty()) return {};

            // 3. the file name, next to the preset, in "Samples" folders above it, or anywhere below it
            for (auto base = dir; base.exists(); )
            {
                if (base.getChildFile (name).existsAsFile()) return base.getChildFile (name);
                auto samples = base.getChildFile ("Samples");
                if (samples.isDirectory())
                {
                    auto f = findByName (samples, name);
                    if (f.existsAsFile()) return f;
                }
                auto parent = base.getParentDirectory();
                if (parent == base || base.getFullPathName().length() < 4) break;
                base = parent;
                if (base.getFullPathName() == dir.getFullPathName()) break;
            }
            auto f = findByName (dir, name);
            if (f.existsAsFile()) return f;
            f = findByName (dir.getParentDirectory(), name);
            return f.existsAsFile() ? f : File();
        }

        // ----- the part list --------------------------------------------------------------------
        struct Part
        {
            Zone zone;
            int selLo = 0, selHi = 127;
        };

        bool readXml (const File& f, std::unique_ptr<XmlElement>& xml, String& error)
        {
            MemoryBlock raw;
            if (! f.loadFileAsData (raw) || raw.getSize() < 8)
            {
                error = "Couldn't read " + f.getFileName();
                return false;
            }

            MemoryBlock text;
            const auto* b = static_cast<const uint8*> (raw.getData());
            if (b[0] == 0x1f && b[1] == 0x8b)
            {
                GZIPDecompressorInputStream gz (new MemoryInputStream (raw, false), true, GZIPDecompressorInputStream::gzipFormat);
                gz.readIntoMemoryBlock (text);
            }
            else
                text = raw;

            if (text.getSize() == 0)
            {
                error = "The preset is empty or damaged (couldn't decompress it)";
                return false;
            }
            // Live 12 also has a self-describing binary record format (used for packs).
            if (text.getSize() >= 4 && ByteOrder::littleEndianInt (text.getData()) == 0x78561EAB)
            {
                error = "This Ableton file uses Live's binary format, which can't be read yet";
                return false;
            }

            xml = XmlDocument::parse (String::fromUTF8 (static_cast<const char*> (text.getData()), (int) text.getSize()));
            if (xml == nullptr || ! xml->hasTagName ("Ableton"))
            {
                error = "Not an Ableton Live preset";
                return false;
            }
            return true;
        }
    }

    bool loadAbleton (const File& file, SampleCache& cache, KeysMap& out, String& error)
    {
        std::unique_ptr<XmlElement> xml;
        if (! readXml (file, xml, error))
            return false;

        const auto* device = findDevice (*xml);
        if (device == nullptr)
        {
            error = "This Ableton preset has no Sampler or Simpler in it";
            return false;
        }

        const auto* map = findDescendant (child (device, "Player"), "MultiSampleMap");
        const auto* parts = child (map, "SampleParts");
        if (parts == nullptr)
        {
            error = "This Ableton preset has no samples in it";
            return false;
        }

        std::vector<Part> list;
        String firstMissing;
        int missing = 0;
        for (auto* p : parts->getChildWithTagNameIterator ("MultiSamplePart"))
        {
            const auto* fileRef = child (child (p, "SampleRef"), "FileRef");
            if (fileRef == nullptr) { ++missing; continue; }

            String err;
            auto sampleFile = resolveSample (fileRef, file);
            SamplePtr sample = sampleFile.existsAsFile() ? cache.get (sampleFile, err) : nullptr;
            if (sample == nullptr)
            {
                ++missing;
                if (firstMissing.isEmpty())
                {
                    auto n = valueOf (fileRef, "Name");
                    firstMissing = n.isNotEmpty() ? n : relativePathOf (fileRef);
                }
                continue;
            }

            Part part;
            auto& z = part.zone;
            z.sample = sample;

            const auto* key = child (p, "KeyRange");
            z.loKey = jlimit (0, 127, intOf (key, "Min", 0));
            z.hiKey = jlimit (z.loKey, 127, intOf (key, "Max", 127));
            const auto* vel = child (p, "VelocityRange");
            z.loVel = jlimit (1, 127, intOf (vel, "Min", 1));
            z.hiVel = jlimit (z.loVel, 127, intOf (vel, "Max", 127));
            if (auto* sel = child (p, "SelectorRange"))
            {
                part.selLo = intOf (sel, "Min", 0);
                part.selHi = intOf (sel, "Max", 127);
            }

            z.root = jlimit (0, 127, intOf (p, "RootKey", 60));
            z.tuneCents = (float) numberOf (p, "Detune", 0.0);
            z.pan = jlimit (-1.0f, 1.0f, (float) numberOf (p, "Panorama", 0.0));
            z.gainDb = Decibels::gainToDecibels ((float) jmax (0.0001, numberOf (p, "Volume", 1.0)), -100.0f);
            z.start = jmax (0, (int) numberOf (p, "SampleStart", 0.0));

            if (auto* loop = child (p, "SustainLoop"))
            {
                const int mode = intOf (loop, "Mode", 0);      // 0 off, 1 forward, 2 alternating
                const int len = sample->length();
                if (mode > 0 && len > 1)
                {
                    z.loop = true;
                    z.loopStart = jlimit (0, len - 2, intOf (loop, "Start", 0));
                    int end = intOf (loop, "End", len - 1);
                    if (end <= z.loopStart) end = len - 1;
                    z.loopEnd = jlimit (z.loopStart + 1, len - 1, end);
                }
            }
            list.push_back (std::move (part));
        }

        if (list.empty())
        {
            error = firstMissing.isNotEmpty() ? "Couldn't find the samples of this preset, e.g. " + firstMissing
                                              : "This Ableton preset has no samples in it";
            return false;
        }

        // Round robin: the Live 12 flag, or zones that share key and velocity range but have separate
        // (non-overlapping) sample-selector ranges.
        std::map<std::array<int, 4>, std::vector<size_t>> clusters;
        for (size_t i = 0; i < list.size(); ++i)
        {
            const auto& z = list[i].zone;
            clusters[{ z.loKey, z.hiKey, z.loVel, z.hiVel }].push_back (i);
        }
        const bool rrFlag = valueOf (map, "RoundRobin").trim() == "true";
        for (auto& [k, idx] : clusters)
        {
            if (idx.size() < 2) continue;
            std::stable_sort (idx.begin(), idx.end(), [&] (size_t a, size_t b) { return list[a].selLo < list[b].selLo; });
            bool disjoint = true;
            for (size_t i = 1; i < idx.size(); ++i)
                if (list[idx[i]].selLo <= list[idx[i - 1]].selHi) disjoint = false;
            if (rrFlag || disjoint)
                for (size_t i = 0; i < idx.size(); ++i)
                    list[idx[i]].zone.rrPos = (int) i + 1;
        }

        KeysMap km;
        km.name = file.getFileNameWithoutExtension();
        for (auto& p : list) km.zones.push_back (std::move (p.zone));

        // Amplitude envelope (times in ms) and the instrument transpose.
        if (auto* env = child (child (device, "VolumeAndPan"), "Envelope"))
        {
            km.voicing.env.attack  = (float) jmax (0.0005, manualOf (env, "AttackTime", 0.1) / 1000.0);
            km.voicing.env.decay   = (float) jmax (0.01, manualOf (env, "DecayTime", 300.0) / 1000.0);
            km.voicing.env.sustain = (float) jlimit (0.0, 1.0, manualOf (env, "SustainLevel", 1.0));
            km.voicing.env.release = (float) jmax (0.01, manualOf (env, "ReleaseTime", 50.0) / 1000.0);
        }
        if (auto* pitch = child (device, "Pitch"))
            km.voicing.tuneSemis = (float) (numberOf (pitch, "TransposeKey", 0.0) + numberOf (pitch, "TransposeFine", 0.0) / 100.0);

        out.zones = std::move (km.zones);
        out.name = km.name;
        out.voicing.env = km.voicing.env;
        out.voicing.tuneSemis = km.voicing.tuneSemis;
        if (missing > 0)
            error = String (missing) + " of the preset's samples couldn't be found, e.g. " + firstMissing;
        return true;
    }
}
