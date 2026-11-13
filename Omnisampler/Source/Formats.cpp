#include "Formats.h"
#include <regex>
#include <algorithm>
#include <cmath>

namespace omni
{
    using namespace juce;

    // ---------------------------------------------------------------------------------------
    static void analyse (Sample& s)
    {
        const int n = s.data.getNumSamples(), ch = s.data.getNumChannels();
        float peak = 0;
        for (int c = 0; c < ch; ++c) peak = jmax (peak, s.data.getMagnitude (c, 0, n));
        s.peakDb = Decibels::gainToDecibels (peak, -100.0f);
        const int win = jmax (16, (int) (s.sampleRate * 0.01)), hop = win / 2;
        double best = 0;
        for (int start = 0; start + win <= n || start == 0; start += hop)
        {
            const int len = jmin (win, n - start);
            if (len <= 0) break;
            double sum = 0;
            for (int c = 0; c < ch; ++c)
            {
                auto* d = s.data.getReadPointer (c, start);
                for (int i = 0; i < len; ++i) sum += (double) d[i] * d[i];
            }
            best = jmax (best, sum / (len * ch));
            if (start + win > n) break;
        }
        s.loudDb = (float) (10.0 * std::log10 (best + 1e-12));
    }

    SamplePtr SampleCache::get (const File& f, String& error)
    {
        const auto key = f.getFullPathName();
        if (auto it = cache.find (key); it != cache.end())
            if (auto sp = it->second.lock()) return sp;
        if (! f.existsAsFile()) { error = "Missing: " + f.getFileName(); return nullptr; }
        std::unique_ptr<AudioFormatReader> r (formats.createReaderFor (f));
        if (r == nullptr) { error = "Can't read: " + f.getFileName(); return nullptr; }
        auto s = std::make_shared<Sample>();
        const int ch = (int) jmin (2u, r->numChannels);
        const int len = (int) jmin<int64> (r->lengthInSamples, (int64) (r->sampleRate * 3600.0));
        s->numChannels = ch;
        s->totalFrames = len;
        s->streamed = streaming.load() && len > jmax ((double) kHeadFrames * 2, r->sampleRate * streamSeconds);
        if (s->streamed)
        {
            // Keep the head in memory, and a small overview of the whole file for drawing.
            s->data.setSize (ch, kHeadFrames);
            r->read (&s->data, 0, kHeadFrames, 0, true, ch > 1);
            const int step = Sample::kOverviewStep;
            s->overview.setSize (1, len / step + 1);
            s->overview.clear();
            AudioBuffer<float> chunk (ch, step * 256);
            for (int pos = 0; pos < len; pos += chunk.getNumSamples())
            {
                const int n = jmin (chunk.getNumSamples(), len - pos);
                r->read (&chunk, 0, n, pos, true, ch > 1);
                for (int k = 0; k * step < n; ++k)
                {
                    float m = 0.0f;
                    for (int c = 0; c < ch; ++c) m = jmax (m, chunk.getMagnitude (c, k * step, jmin (step, n - k * step)));
                    s->overview.setSample (0, (pos + k * step) / step, m);
                }
            }
        }
        else
        {
            s->data.setSize (ch, len);
            r->read (&s->data, 0, len, 0, true, ch > 1);
        }
        s->sampleRate = r->sampleRate;
        s->file = f;
        const auto& md = r->metadataValues;
        if (md.containsKey ("MidiUnityNote")) s->metaRoot = md["MidiUnityNote"].getIntValue();
        if (md.getValue ("NumSampleLoops", "0").getIntValue() > 0 || md.containsKey ("Loop0Start"))
        {
            s->metaLoopStart = md.getValue ("Loop0Start", "-1").getIntValue();
            s->metaLoopEnd = md.getValue ("Loop0End", "-1").getIntValue();
        }
        analyse (*s);
        cache[key] = s;
        return s;
    }

    // ---------------------------------------------------------------------------------------
    static int noteFromParts (char letter, const std::string& acc, int octave)
    {
        static const int base[] = { 9, 11, 0, 2, 4, 5, 7 };   // a b c d e f g
        int n = base[std::tolower (letter) - 'a'];
        if (acc == "#" || acc == "s") ++n;
        else if (acc == "b") --n;
        return (octave + 1) * 12 + n;   // C4 = 60
    }

    int rootFromFileName (const String& nameIn)
    {
        const std::string name = nameIn.toStdString();
        // Note names: letter, optional #/b, octave -1..9, not followed by another digit.
        static const std::regex noteRe (R"((?:^|[^A-Za-z0-9])([A-Ga-g])(#|b|s)?(-1|[0-9])(?![0-9]))");
        int found = -1;
        for (auto it = std::sregex_iterator (name.begin(), name.end(), noteRe); it != std::sregex_iterator(); ++it)
        {
            const auto& m = *it;
            found = noteFromParts (m.str (1)[0], m.str (2), std::stoi (m.str (3)));
        }
        if (found >= 0 && found <= 127) return found;
        // Bare MIDI numbers between separators.
        static const std::regex numRe (R"((?:^|[ _\-.])([0-9]{2,3})(?=$|[ _\-.]))");
        for (auto it = std::sregex_iterator (name.begin(), name.end(), numRe); it != std::sregex_iterator(); ++it)
        {
            const int v = std::stoi ((*it).str (1));
            if (v >= 12 && v <= 120) found = v;
        }
        return found;
    }

    int detectRoot (const Sample& s)
    {
        const double sr = s.sampleRate;
        const int maxTau = (int) (sr / 30.0), minTau = (int) (sr / 2000.0), W = 2048;
        const int n = s.data.getNumSamples();
        // Start a little after the loudest point so the attack transient is skipped.
        int peakPos = 0; float pk = 0;
        for (int i = 0; i < n; ++i) { const float a = std::abs (s.data.getSample (0, i)); if (a > pk) { pk = a; peakPos = i; } }
        int start = peakPos + (int) (sr * 0.03);
        if (start + W + maxTau > n) start = jmax (0, n - W - maxTau);
        if (start + W + maxTau > n || pk < 1e-4f) return -1;
        std::vector<float> x ((size_t) (W + maxTau));
        for (int i = 0; i < W + maxTau; ++i)
        {
            float v = 0;
            for (int c = 0; c < s.data.getNumChannels(); ++c) v += s.data.getSample (c, start + i);
            x[(size_t) i] = v;
        }
        std::vector<double> d ((size_t) maxTau + 1, 0.0);
        for (int tau = 1; tau <= maxTau; ++tau)
        {
            double sum = 0;
            for (int i = 0; i < W; ++i) { const double diff = x[(size_t) i] - x[(size_t) (i + tau)]; sum += diff * diff; }
            d[(size_t) tau] = sum;
        }
        double running = 0;
        std::vector<double> cm ((size_t) maxTau + 1, 1.0);
        for (int tau = 1; tau <= maxTau; ++tau)
        {
            running += d[(size_t) tau];
            cm[(size_t) tau] = running > 0 ? d[(size_t) tau] * tau / running : 1.0;
        }
        int tauBest = -1;
        for (int tau = jmax (2, minTau); tau < maxTau; ++tau)
            if (cm[(size_t) tau] < 0.15)
            {
                while (tau + 1 < maxTau && cm[(size_t) tau + 1] < cm[(size_t) tau]) ++tau;
                tauBest = tau;
                break;
            }
        if (tauBest < 0) return -1;
        double t = tauBest;
        const double a = cm[(size_t) tauBest - 1], b = cm[(size_t) tauBest], c = cm[(size_t) tauBest + 1];
        const double den = a - 2 * b + c;
        if (std::abs (den) > 1e-12) t += 0.5 * (a - c) / den;
        const double f = sr / t;
        const int midi = (int) std::round (69.0 + 12.0 * std::log2 (f / 440.0));
        return (midi >= 0 && midi <= 127) ? midi : -1;
    }

    static int rootFor (const Sample& s)
    {
        if (s.metaRoot >= 0 && s.metaRoot <= 127) return s.metaRoot;
        const int byName = rootFromFileName (s.file.getFileNameWithoutExtension());
        const int byPitch = detectRoot (s);
        if (byName >= 0)
        {
            // Names disagree on which C is C4; trust the audio's octave when it is exactly one off.
            if (byPitch >= 0 && std::abs (std::abs (byPitch - byName) - 12) <= 1) return byName + (byPitch > byName ? 12 : -12);
            return byName;
        }
        return byPitch >= 0 ? byPitch : 60;
    }

    // ---------------------------------------------------------------------------------------
    void assignVelocityByLoudness (std::vector<Zone*>& zones)
    {
        const int n = (int) zones.size();
        if (n == 0) return;
        std::sort (zones.begin(), zones.end(), [] (const Zone* a, const Zone* b) { return a->sample->loudDb < b->sample->loudDb; });
        if (n == 1) { zones[0]->loVel = 1; zones[0]->hiVel = 127; return; }
        const float lo = zones.front()->sample->loudDb, hi = zones.back()->sample->loudDb;
        std::vector<int> target ((size_t) n);
        bool equal = hi - lo < 3.0f;
        if (! equal)
        {
            for (int i = 0; i < n; ++i)
                target[(size_t) i] = 1 + (int) std::round (126.0 * (zones[(size_t) i]->sample->loudDb - lo) / (hi - lo));
            for (int i = 1; i < n; ++i) if (target[(size_t) i] <= target[(size_t) i - 1]) { equal = true; break; }
        }
        int prevHi = 0;
        for (int i = 0; i < n; ++i)
        {
            auto* z = zones[(size_t) i];
            z->loVel = prevHi + 1;
            if (i == n - 1) z->hiVel = 127;
            else if (equal) z->hiVel = (i + 1) * 127 / n;
            else z->hiVel = (target[(size_t) i] + target[(size_t) i + 1]) / 2;
            z->hiVel = jlimit (z->loVel, 127, z->hiVel);
            prevHi = z->hiVel;
        }
    }

    static Zone zoneFor (const SamplePtr& s)
    {
        Zone z;
        z.sample = s;
        if (s->metaLoopEnd > s->metaLoopStart && s->metaLoopStart >= 0)
        {
            z.loop = true; z.loopStart = s->metaLoopStart; z.loopEnd = s->metaLoopEnd;
        }
        return z;
    }

    std::vector<Zone> mapPadFiles (const std::vector<SamplePtr>& samples, DropMode mode)
    {
        std::vector<Zone> zones;
        for (auto& s : samples) if (s) zones.push_back (zoneFor (s));
        for (auto& z : zones) z.loop = false;   // drum hits play through
        std::stable_sort (zones.begin(), zones.end(), [] (const Zone& a, const Zone& b) { return a.sample->loudDb < b.sample->loudDb; });
        if (mode == DropMode::RoundRobin)
        {
            int i = 0;
            for (auto& z : zones) { z.loVel = 1; z.hiVel = 127; z.rrPos = zones.size() > 1 ? ++i : 0; }
        }
        else
        {
            std::vector<Zone*> ptrs;
            for (auto& z : zones) ptrs.push_back (&z);
            assignVelocityByLoudness (ptrs);
        }
        return zones;
    }

    std::vector<Zone> mapKeyFiles (const std::vector<SamplePtr>& samples)
    {
        std::map<int, std::vector<Zone>> byRoot;
        for (auto& s : samples)
        {
            if (! s) continue;
            auto z = zoneFor (s);
            z.root = rootFor (*s);
            byRoot[z.root].push_back (z);
        }
        std::vector<int> roots;
        for (auto& [r, v] : byRoot) roots.push_back (r);
        std::vector<Zone> out;
        for (size_t i = 0; i < roots.size(); ++i)
        {
            const int lo = i == 0 ? 0 : (roots[i - 1] + roots[i]) / 2 + 1;
            const int hi = i + 1 == roots.size() ? 127 : (roots[i] + roots[i + 1]) / 2;
            auto& group = byRoot[roots[i]];
            std::vector<Zone*> ptrs;
            for (auto& z : group) { z.loKey = lo; z.hiKey = hi; ptrs.push_back (&z); }
            assignVelocityByLoudness (ptrs);
            for (auto& z : group) out.push_back (z);
        }
        return out;
    }

    // ---------------------------------------------------------------------------------------
    static int parseNote (const String& v)
    {
        const auto t = v.trim();
        if (t.isEmpty()) return -1;
        if (t.containsOnly ("-0123456789")) return t.getIntValue();
        const auto s = t.toLowerCase().toStdString();
        static const std::regex re (R"(^([a-g])(#|b)?(-?[0-9])$)");
        std::smatch m;
        if (std::regex_match (s, m, re)) return noteFromParts (m.str (1)[0], m.str (2), std::stoi (m.str (3)));
        return -1;
    }

    using Ops = std::map<String, String>;

    static void parseOpcodes (const String& seg, Ops& into)
    {
        const std::string s = seg.toStdString();
        static const std::regex opRe (R"(([A-Za-z0-9_]+)=)");
        std::vector<std::pair<std::string, std::pair<size_t, size_t>>> found;   // name, (valueStart, matchStart)
        for (auto it = std::sregex_iterator (s.begin(), s.end(), opRe); it != std::sregex_iterator(); ++it)
            found.push_back ({ (*it).str (1), { (size_t) ((*it).position (0) + (*it).length (0)), (size_t) (*it).position (0) } });
        for (size_t i = 0; i < found.size(); ++i)
        {
            const size_t vs = found[i].second.first;
            const size_t ve = i + 1 < found.size() ? found[i + 1].second.second : s.size();
            into[String (found[i].first).toLowerCase()] = String (s.substr (vs, ve - vs)).trim();
        }
    }

    static Zone zoneFromOps (const Ops& o, const File& baseDir, SampleCache& cache, String& firstError, bool& ok)
    {
        auto get = [&o] (const char* k, const String& def = {}) { auto it = o.find (k); return it != o.end() ? it->second : def; };
        Zone z;
        ok = false;
        auto path = get ("sample").replaceCharacter ('\\', '/');
        if (path.isEmpty()) return z;
        const auto def = get ("default_path").replaceCharacter ('\\', '/');
        File f = File::isAbsolutePath (path) ? File (path) : baseDir.getChildFile (def + path);
        String err;
        z.sample = cache.get (f, err);
        if (! z.sample) { if (firstError.isEmpty()) firstError = err; return z; }
        if (o.count ("key")) { const int k = parseNote (get ("key")); z.loKey = z.hiKey = z.root = k; }
        if (o.count ("lokey")) z.loKey = parseNote (get ("lokey"));
        if (o.count ("hikey")) z.hiKey = parseNote (get ("hikey"));
        if (o.count ("pitch_keycenter")) z.root = parseNote (get ("pitch_keycenter"));
        else if (! o.count ("key") && z.sample->metaRoot >= 0) z.root = z.sample->metaRoot;
        z.loVel = jlimit (1, 127, get ("lovel", "1").getIntValue());
        z.hiVel = jlimit (1, 127, get ("hivel", "127").getIntValue());
        z.tuneCents = get ("tune", "0").getFloatValue() + 100.0f * get ("transpose", "0").getFloatValue();
        z.gainDb = get ("volume", "0").getFloatValue();
        z.pan = jlimit (-1.0f, 1.0f, get ("pan", "0").getFloatValue() / 100.0f);
        z.start = jmax (0, get ("offset", "0").getIntValue());
        if (get ("seq_length", "1").getIntValue() > 1) z.rrPos = jmax (1, get ("seq_position", "1").getIntValue());
        const auto mode = get ("loop_mode", get ("loopmode"));
        const int ls = get ("loop_start", get ("loopstart", "-1")).getIntValue();
        const int le = get ("loop_end", get ("loopend", "-1")).getIntValue();
        if (mode == "loop_continuous" || mode == "loop_sustain" || (mode.isEmpty() && z.sample->metaLoopEnd > z.sample->metaLoopStart))
        {
            z.loop = true;
            z.loopStart = ls >= 0 ? ls : jmax (0, z.sample->metaLoopStart);
            z.loopEnd = le > 0 ? le : (z.sample->metaLoopEnd > 0 ? z.sample->metaLoopEnd : z.sample->length() - 1);
        }
        z.loKey = jlimit (0, 127, z.loKey); z.hiKey = jlimit (z.loKey, 127, z.hiKey); z.root = jlimit (0, 127, z.root);
        ok = true;
        return z;
    }

    bool loadSfz (const File& file, SampleCache& cache, KeysMap& out, String& error)
    {
        String text = file.loadFileAsString();
        if (text.isEmpty()) { error = "Empty or unreadable SFZ"; return false; }
        // Strip comments.
        {
            String clean;
            int i = 0;
            while (i < text.length())
            {
                if (text.substring (i, i + 2) == "//") { while (i < text.length() && text[i] != '\n') ++i; continue; }
                if (text.substring (i, i + 2) == "/*") { const int e = text.indexOf (i + 2, "*/"); i = e < 0 ? text.length() : e + 2; continue; }
                clean += text[i++];
            }
            text = clean;
        }
        // #define $NAME value
        StringPairArray defines;
        StringArray lines = StringArray::fromLines (text);
        for (auto& l : lines)
        {
            auto t = l.trim();
            if (t.startsWith ("#define"))
            {
                auto rest = t.fromFirstOccurrenceOf ("#define", false, false).trim();
                defines.set (rest.upToFirstOccurrenceOf (" ", false, false).trim(), rest.fromFirstOccurrenceOf (" ", false, false).trim());
                l.clear();
            }
            else if (t.startsWith ("#include")) l.clear();
        }
        text = lines.joinIntoString ("\n");
        for (auto& k : defines.getAllKeys()) text = text.replace (k, defines[k]);

        Ops control, global, master, group, region;
        String header;
        int regions = 0;
        KeysMap km;
        km.name = file.getFileNameWithoutExtension();
        km.voicing.env = out.voicing.env;
        String firstErr;
        bool envTaken = false;
        const auto baseDir = file.getParentDirectory();

        auto emit = [&]
        {
            Ops merged = control;
            for (auto* m : { &global, &master, &group, &region }) for (auto& kv : *m) merged[kv.first] = kv.second;
            if (merged.count ("trigger") && merged["trigger"] != "attack" && merged["trigger"] != "first") return;
            bool ok = false;
            auto z = zoneFromOps (merged, baseDir, cache, firstErr, ok);
            if (! ok) return;
            if (! envTaken)
            {
                auto g = [&merged] (const char* k, float d) { return merged.count (k) ? merged[k].getFloatValue() : d; };
                km.voicing.env = { jmax (0.0005f, g ("ampeg_attack", 0.002f)), jmax (0.01f, g ("ampeg_decay", 0.4f)),
                                   jlimit (0.0f, 1.0f, g ("ampeg_sustain", 100.0f) / 100.0f), jmax (0.01f, g ("ampeg_release", 0.3f)) };
                envTaken = true;
            }
            km.zones.push_back (z);
            ++regions;
        };

        int pos = 0;
        while (pos < text.length())
        {
            const int lt = text.indexOf (pos, "<");
            const String seg = text.substring (pos, lt < 0 ? text.length() : lt);
            Ops* target = header == "control" ? &control : header == "global" ? &global : header == "master" ? &master
                        : header == "group" ? &group : header == "region" ? &region : nullptr;
            if (target != nullptr) parseOpcodes (seg, *target);
            if (lt < 0) break;
            const int gt = text.indexOf (lt, ">");
            if (gt < 0) break;
            if (header == "region") emit();
            header = text.substring (lt + 1, gt).trim().toLowerCase();
            if (header == "global") { global.clear(); master.clear(); group.clear(); }
            else if (header == "master") { master.clear(); group.clear(); }
            else if (header == "group") group.clear();
            else if (header == "control") control.clear();
            region.clear();
            pos = gt + 1;
        }
        if (header == "region") emit();
        if (regions == 0) { error = firstErr.isNotEmpty() ? firstErr : "No playable regions found"; return false; }
        out.zones = std::move (km.zones);
        out.name = km.name;
        out.voicing.env = km.voicing.env;
        if (firstErr.isNotEmpty()) error = "Some samples were missing, e.g. " + firstErr;
        return true;
    }

    // ---------------------------------------------------------------------------------------
    bool loadDecentSampler (const File& file, SampleCache& cache, KeysMap& out, String& error)
    {
        auto xml = XmlDocument::parse (file);
        if (xml == nullptr || ! xml->hasTagName ("DecentSampler")) { error = "Not a DecentSampler preset"; return false; }
        auto* groups = xml->getChildByName ("groups");
        if (groups == nullptr) { error = "Preset has no <groups>"; return false; }
        const auto baseDir = file.getParentDirectory();

        auto attr = [] (const XmlElement* s, const XmlElement* g, const XmlElement* gs, const char* name) -> String
        {
            for (auto* e : { s, g, gs }) if (e != nullptr && e->hasAttribute (name)) return e->getStringAttribute (name);
            return {};
        };
        auto volDb = [] (const String& v) -> float
        {
            if (v.isEmpty()) return 0.0f;
            if (v.endsWithIgnoreCase ("db")) return v.dropLastCharacters (2).getFloatValue();
            return Decibels::gainToDecibels (jmax (0.0001f, v.getFloatValue()));
        };

        KeysMap km;
        String firstErr;
        bool envTaken = false;
        for (auto* g : groups->getChildWithTagNameIterator ("group"))
        {
            for (auto* s : g->getChildWithTagNameIterator ("sample"))
            {
                const auto trig = attr (s, g, groups, "trigger");
                if (trig == "release") continue;
                const auto path = attr (s, g, groups, "path").replaceCharacter ('\\', '/');
                if (path.isEmpty()) continue;
                String err;
                Zone z;
                z.sample = cache.get (baseDir.getChildFile (path), err);
                if (! z.sample) { if (firstErr.isEmpty()) firstErr = err; continue; }
                auto num = [&] (const char* n, const String& def) { auto v = attr (s, g, groups, n); return v.isNotEmpty() ? v : def; };
                z.root = jlimit (0, 127, parseNote (num ("rootNote", "60")));
                z.loKey = jlimit (0, 127, parseNote (num ("loNote", "0")));
                z.hiKey = jlimit (z.loKey, 127, parseNote (num ("hiNote", "127")));
                z.loVel = jlimit (1, 127, num ("loVel", "1").getIntValue());
                z.hiVel = jlimit (z.loVel, 127, num ("hiVel", "127").getIntValue());
                z.gainDb = volDb (attr (s, nullptr, nullptr, "volume")) + volDb (attr (nullptr, g, nullptr, "volume"));
                z.tuneCents = 100.0f * num ("tuning", "0").getFloatValue();
                z.pan = jlimit (-1.0f, 1.0f, num ("pan", "0").getFloatValue() / 100.0f);
                z.start = jmax (0, num ("start", "0").getIntValue());
                const auto seqMode = attr (nullptr, g, groups, "seqMode");
                if (seqMode == "round_robin" || seqMode == "random" || seqMode == "true_random")
                    z.rrPos = jmax (1, num ("seqPosition", "1").getIntValue());
                const bool loopOn = num ("loopEnabled", "false") == "true";
                const int ls = num ("loopStart", "-1").getIntValue(), le = num ("loopEnd", "-1").getIntValue();
                if (loopOn || (ls >= 0 && le > ls && num ("loopEnabled", "") != "false"))
                {
                    z.loop = true;
                    z.loopStart = ls >= 0 ? ls : jmax (0, z.sample->metaLoopStart);
                    z.loopEnd = le > 0 ? le : (z.sample->metaLoopEnd > 0 ? z.sample->metaLoopEnd : z.sample->length() - 1);
                }
                if (! envTaken)
                {
                    auto f = [&] (const char* n, float d) { auto v = attr (s, g, groups, n); return v.isNotEmpty() ? v.getFloatValue() : d; };
                    km.voicing.env = { jmax (0.0005f, f ("attack", 0.002f)), jmax (0.01f, f ("decay", 0.4f)),
                                       jlimit (0.0f, 1.0f, f ("sustain", 1.0f)), jmax (0.01f, f ("release", 0.3f)) };
                    envTaken = true;
                }
                km.zones.push_back (z);
            }
        }
        if (km.zones.empty()) { error = firstErr.isNotEmpty() ? firstErr : "No samples in preset"; return false; }
        out.zones = std::move (km.zones);
        out.name = file.getFileNameWithoutExtension();
        out.voicing.env = km.voicing.env;
        if (firstErr.isNotEmpty()) error = "Some samples were missing, e.g. " + firstErr;
        return true;
    }

    bool loadDsLibrary (const File& zipFile, SampleCache& cache, KeysMap& out, String& error)
    {
        auto dest = File::getSpecialLocation (File::userApplicationDataDirectory)
                        .getChildFile ("Omnisampler/Libraries").getChildFile (zipFile.getFileNameWithoutExtension());
        if (! dest.isDirectory() || dest.findChildFiles (File::findFiles, true, "*.dspreset").isEmpty())
        {
            ZipFile zip (zipFile);
            if (zip.getNumEntries() == 0) { error = "Couldn't open the .dslibrary"; return false; }
            dest.createDirectory();
            const auto r = zip.uncompressTo (dest, true);
            if (r.failed()) { error = r.getErrorMessage(); return false; }
        }
        auto presets = dest.findChildFiles (File::findFiles, true, "*.dspreset");
        if (presets.isEmpty()) { error = "No .dspreset inside the library"; return false; }
        presets.sort();
        return loadDecentSampler (presets[0], cache, out, error);
    }

    bool saveSfz (const KeysMap& keys, const File& f, String& error)
    {
        String s;
        s << "// Saved by Omnisampler\n<control>\n<global> ampeg_attack=" << keys.voicing.env.attack << " ampeg_decay=" << keys.voicing.env.decay
          << " ampeg_sustain=" << keys.voicing.env.sustain * 100.0f << " ampeg_release=" << keys.voicing.env.release << "\n";
        int seqLen = 0;
        for (auto& z : keys.zones) seqLen = jmax (seqLen, z.rrPos);
        for (auto& z : keys.zones)
        {
            if (! z.sample) continue;
            auto path = z.sample->file.isAChildOf (f.getParentDirectory()) ? z.sample->file.getRelativePathFrom (f.getParentDirectory()) : z.sample->file.getFullPathName();
            s << "<region> sample=" << path.replaceCharacter ('\\', '/') << " lokey=" << z.loKey << " hikey=" << z.hiKey
              << " pitch_keycenter=" << z.root << " lovel=" << z.loVel << " hivel=" << z.hiVel;
            if (z.tuneCents != 0.0f) s << " tune=" << (int) std::round (z.tuneCents);
            if (z.gainDb != 0.0f) s << " volume=" << z.gainDb;
            if (z.pan != 0.0f) s << " pan=" << (int) std::round (z.pan * 100.0f);
            if (z.start > 0) s << " offset=" << z.start;
            if (z.rrPos > 0) s << " seq_length=" << seqLen << " seq_position=" << z.rrPos;
            if (z.loop) s << " loop_mode=loop_continuous loop_start=" << z.loopStart << " loop_end=" << z.loopEnd;
            s << "\n";
        }
        if (! f.replaceWithText (s)) { error = "Couldn't write " + f.getFullPathName(); return false; }
        return true;
    }

    // ---------------------------------------------------------------------------------------
    static void voicingToXml (const Voicing& v, XmlElement& e)
    {
        e.setAttribute ("vol", v.volumeDb); e.setAttribute ("pan", v.pan); e.setAttribute ("tune", v.tuneSemis);
        e.setAttribute ("a", v.env.attack); e.setAttribute ("d", v.env.decay); e.setAttribute ("s", v.env.sustain); e.setAttribute ("r", v.env.release);
        e.setAttribute ("ftype", (int) v.filter.type); e.setAttribute ("cut", v.filter.cutoff); e.setAttribute ("res", v.filter.resonance);
        e.setAttribute ("vs", v.velSens); e.setAttribute ("rr", (int) v.rr);
    }
    static void voicingFromXml (Voicing& v, const XmlElement& e)
    {
        v.volumeDb = (float) e.getDoubleAttribute ("vol", v.volumeDb); v.pan = (float) e.getDoubleAttribute ("pan", v.pan);
        v.tuneSemis = (float) e.getDoubleAttribute ("tune", v.tuneSemis);
        v.env.attack = (float) e.getDoubleAttribute ("a", v.env.attack); v.env.decay = (float) e.getDoubleAttribute ("d", v.env.decay);
        v.env.sustain = (float) e.getDoubleAttribute ("s", v.env.sustain); v.env.release = (float) e.getDoubleAttribute ("r", v.env.release);
        v.filter.type = (FilterType) e.getIntAttribute ("ftype", (int) v.filter.type);
        v.filter.cutoff = (float) e.getDoubleAttribute ("cut", v.filter.cutoff); v.filter.resonance = (float) e.getDoubleAttribute ("res", v.filter.resonance);
        v.velSens = (float) e.getDoubleAttribute ("vs", v.velSens); v.rr = (RRMode) e.getIntAttribute ("rr", (int) v.rr);
    }
    // iOS gives the app a new container folder on every update, so paths inside the container are
    // stored relative to it ("~app/Documents/...") and resolved again on load.
    static String storedPath (const File& f)
    {
       #if JUCE_IOS
        const auto home = File::getSpecialLocation (File::userDocumentsDirectory).getParentDirectory();
        if (f.isAChildOf (home)) return "~app/" + f.getRelativePathFrom (home);
       #endif
        return f.getFullPathName();
    }
    static File fileFromStoredPath (const String& path)
    {
       #if JUCE_IOS
        if (path.startsWith ("~app/"))
            return File::getSpecialLocation (File::userDocumentsDirectory).getParentDirectory().getChildFile (path.substring (5));
       #endif
        return File::isAbsolutePath (path) ? File (path) : File();
    }

    static void zonesToXml (const std::vector<Zone>& zones, XmlElement& parent)
    {
        for (auto& z : zones)
        {
            if (! z.sample) continue;
            auto* e = parent.createNewChildElement ("Zone");
            e->setAttribute ("path", storedPath (z.sample->file));
            e->setAttribute ("lokey", z.loKey); e->setAttribute ("hikey", z.hiKey); e->setAttribute ("root", z.root);
            e->setAttribute ("lovel", z.loVel); e->setAttribute ("hivel", z.hiVel); e->setAttribute ("rr", z.rrPos);
            e->setAttribute ("tune", z.tuneCents); e->setAttribute ("gain", z.gainDb); e->setAttribute ("pan", z.pan);
            e->setAttribute ("loop", z.loop); e->setAttribute ("ls", z.loopStart); e->setAttribute ("le", z.loopEnd); e->setAttribute ("start", z.start);
        }
    }
    static std::vector<Zone> zonesFromXml (const XmlElement& parent, SampleCache& cache, StringArray& missing)
    {
        std::vector<Zone> out;
        for (auto* e : parent.getChildWithTagNameIterator ("Zone"))
        {
            String err;
            Zone z;
            z.sample = cache.get (fileFromStoredPath (e->getStringAttribute ("path")), err);
            if (! z.sample) { missing.addIfNotAlreadyThere (e->getStringAttribute ("path")); continue; }
            z.loKey = e->getIntAttribute ("lokey", 0); z.hiKey = e->getIntAttribute ("hikey", 127); z.root = e->getIntAttribute ("root", 60);
            z.loVel = e->getIntAttribute ("lovel", 1); z.hiVel = e->getIntAttribute ("hivel", 127); z.rrPos = e->getIntAttribute ("rr", 0);
            z.tuneCents = (float) e->getDoubleAttribute ("tune"); z.gainDb = (float) e->getDoubleAttribute ("gain"); z.pan = (float) e->getDoubleAttribute ("pan");
            z.loop = e->getBoolAttribute ("loop"); z.loopStart = e->getIntAttribute ("ls"); z.loopEnd = e->getIntAttribute ("le"); z.start = e->getIntAttribute ("start");
            out.push_back (z);
        }
        return out;
    }

    std::unique_ptr<XmlElement> instrumentToXml (const Instrument& inst)
    {
        auto x = std::make_unique<XmlElement> ("Omnisampler");
        x->setAttribute ("version", 1);
        for (int i = 0; i < kPads; ++i)
        {
            const auto& p = inst.pads[i];
            auto* e = x->createNewChildElement ("Pad");
            e->setAttribute ("index", i); e->setAttribute ("note", p.note); e->setAttribute ("name", p.name);
            e->setAttribute ("choke", p.chokeGroup); e->setAttribute ("oneshot", p.oneShot);
            voicingToXml (p.voicing, *e);
            if (p.output != 0) e->setAttribute ("out", p.output);
            if (p.sampleLevelDb != 0.0f) e->setAttribute ("slevel", p.sampleLevelDb);
            zonesToXml (p.zones, *e);
            for (auto& l : p.synths)
            {
                auto* y = e->createNewChildElement ("Synth");
                y->setAttribute ("model", (int) l.model); y->setAttribute ("on", l.on);
                y->setAttribute ("level", l.levelDb); y->setAttribute ("pan", l.pan); y->setAttribute ("tune", l.tuneSemis);
                y->setAttribute ("decay", l.decay); y->setAttribute ("tone", l.tone); y->setAttribute ("penv", l.pitchEnv);
                y->setAttribute ("pdecay", l.pitchDecay); y->setAttribute ("click", l.click); y->setAttribute ("noise", l.noise);
                y->setAttribute ("drive", l.drive); y->setAttribute ("vs", l.velSens);
            }
        }
        auto* k = x->createNewChildElement ("Keys");
        k->setAttribute ("name", inst.keys.name);
        voicingToXml (inst.keys.voicing, *k);
        if (inst.keys.output != 0) k->setAttribute ("out", inst.keys.output);
        zonesToXml (inst.keys.zones, *k);
        return x;
    }

    Instrument instrumentFromXml (const XmlElement& x, SampleCache& cache, StringArray& missing)
    {
        Instrument inst;
        for (auto* e : x.getChildWithTagNameIterator ("Pad"))
        {
            const int i = e->getIntAttribute ("index", -1);
            if (i < 0 || i >= kPads) continue;
            auto& p = inst.pads[i];
            p.note = e->getIntAttribute ("note", p.note); p.name = e->getStringAttribute ("name");
            p.chokeGroup = e->getIntAttribute ("choke"); p.oneShot = e->getBoolAttribute ("oneshot", true);
            voicingFromXml (p.voicing, *e);
            p.output = jlimit (0, kOutputs - 1, e->getIntAttribute ("out", 0));
            p.sampleLevelDb = (float) e->getDoubleAttribute ("slevel", 0.0);
            p.zones = zonesFromXml (*e, cache, missing);
            for (auto* y : e->getChildWithTagNameIterator ("Synth"))
            {
                if ((int) p.synths.size() >= kMaxSynthLayers) break;
                SynthLayer l;
                l.model = (DrumModel) jlimit (0, (int) DrumModel::NumModels - 1, y->getIntAttribute ("model", 0));
                l.on = y->getBoolAttribute ("on", true);
                auto f = [y] (const char* n, float d) { return (float) y->getDoubleAttribute (n, d); };
                l.levelDb = f ("level", l.levelDb); l.pan = f ("pan", l.pan); l.tuneSemis = f ("tune", l.tuneSemis);
                l.decay = f ("decay", l.decay); l.tone = f ("tone", l.tone); l.pitchEnv = f ("penv", l.pitchEnv);
                l.pitchDecay = f ("pdecay", l.pitchDecay); l.click = f ("click", l.click); l.noise = f ("noise", l.noise);
                l.drive = f ("drive", l.drive); l.velSens = f ("vs", l.velSens);
                p.synths.push_back (l);
            }
        }
        if (auto* k = x.getChildByName ("Keys"))
        {
            inst.keys.name = k->getStringAttribute ("name");
            voicingFromXml (inst.keys.voicing, *k);
            inst.keys.output = jlimit (0, kOutputs - 1, k->getIntAttribute ("out", 0));
            inst.keys.zones = zonesFromXml (*k, cache, missing);
        }
        return inst;
    }
}
