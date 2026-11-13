#include "MoreFormats.h"
#include <cmath>
#include <map>

namespace omni
{
    using namespace juce;

    File extractedSamplesFolder (const String& instrumentName)
    {
        auto name = File::createLegalFileName (instrumentName).trim();
        if (name.isEmpty()) name = "Instrument";
       #if JUCE_IOS
        auto root = File::getSpecialLocation (File::userDocumentsDirectory).getChildFile ("Imported/Extracted");
       #else
        auto root = File::getSpecialLocation (File::userApplicationDataDirectory).getChildFile ("Omnisampler/Extracted");
       #endif
        auto dir = root.getChildFile (name);
        dir.createDirectory();
        return dir;
    }

    bool saveDecentSampler (const KeysMap& keys, const File& f, String& error)
    {
        const auto folder = f.getParentDirectory();

        // Group the zones by round-robin position: 0 = plain group, n = group with seqPosition n.
        std::map<int, std::vector<const Zone*>> groupsByPos;
        for (auto& z : keys.zones)
            if (z.sample != nullptr) groupsByPos[jmax (0, z.rrPos)].push_back (&z);
        if (groupsByPos.empty()) { error = "Nothing to export: no samples"; return false; }

        XmlElement root ("DecentSampler");
        root.setAttribute ("minVersion", "1.0.0");
        auto* ui = root.createNewChildElement ("ui");
        ui->setAttribute ("width", 812);
        ui->setAttribute ("height", 375);

        auto* groups = root.createNewChildElement ("groups");
        const auto& env = keys.voicing.env;
        groups->setAttribute ("attack", (double) env.attack);
        groups->setAttribute ("decay", (double) env.decay);
        groups->setAttribute ("sustain", (double) env.sustain);
        groups->setAttribute ("release", (double) env.release);

        for (auto& [pos, zones] : groupsByPos)
        {
            auto* g = groups->createNewChildElement ("group");
            if (pos > 0)
            {
                g->setAttribute ("seqMode", "round_robin");
                g->setAttribute ("seqPosition", pos);
            }

            for (auto* z : zones)
            {
                const auto& file = z->sample->file;
                const String path = file.isAChildOf (folder) ? file.getRelativePathFrom (folder) : file.getFullPathName();

                auto* s = g->createNewChildElement ("sample");
                s->setAttribute ("path", path.replaceCharacter ('\\', '/'));
                s->setAttribute ("rootNote", z->root);
                s->setAttribute ("loNote", z->loKey);
                s->setAttribute ("hiNote", z->hiKey);
                s->setAttribute ("loVel", z->loVel);
                s->setAttribute ("hiVel", z->hiVel);
                s->setAttribute ("tuning", (double) z->tuneCents / 100.0);
                s->setAttribute ("volume", String ((double) z->gainDb, 2) + "dB");
                s->setAttribute ("pan", (int) std::round (jlimit (-1.0f, 1.0f, z->pan) * 100.0f));
                s->setAttribute ("start", z->start);
                s->setAttribute ("loopEnabled", String (z->loop ? "true" : "false"));
                s->setAttribute ("loopStart", z->loopStart);
                s->setAttribute ("loopEnd", z->loopEnd);
            }
        }

        if (! root.writeTo (f)) { error = "Couldn't write " + f.getFullPathName(); return false; }
        return true;
    }

    File unpackInstrumentZip (const File& zipFile, String& error)
    {
        ZipFile zip (zipFile);
        if (zip.getNumEntries() == 0) { error = "Couldn't open the zip"; return {}; }

        const auto dest = extractedSamplesFolder (zipFile.getFileNameWithoutExtension());
        if (dest.findChildFiles (File::findFiles, true).isEmpty())
        {
            const auto r = zip.uncompressTo (dest, true);
            if (r.failed())
            {
                error = "Couldn't open the zip";
                if (r.getErrorMessage().isNotEmpty()) error << ": " << r.getErrorMessage();
                return {};
            }
        }

        const auto all = dest.findChildFiles (File::findFiles, true);
        static const char* const preference[] = { "dspreset", "sfz", "sf2", "exs", "adv", "nki" };
        for (auto* ext : preference)
        {
            Array<File> hits;
            for (auto& f : all)
                if (f.hasFileExtension (ext)) hits.add (f);
            if (hits.isEmpty()) continue;
            hits.sort();
            return hits.getFirst();
        }

        error = "No instrument inside the zip";
        return {};
    }
}
