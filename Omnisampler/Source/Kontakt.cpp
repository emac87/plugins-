// Native Instruments Kontakt instruments (.nki), unlocked ones only.
//
// Supported containers (the layouts follow the open-source ConvertWithMoss project,
// git-moss/ConvertWithMoss, package format.ni.kontakt; nothing here is tested against NI software):
//   - Kontakt 1 and 1.5   : 36-byte header + zlib-compressed XML  (<NiSS_Program>)
//   - Kontakt 2 - 4.1     : 170-byte header + zlib-compressed XML (<K2_Container><K2_Program>)
//   - Kontakt 4.2         : 222-byte header + FastLZ-compressed binary "preset chunks"
//   - Kontakt 5 - 8       : NI container ("hsin" items) holding the same binary preset chunks
// Samples are .wav/.aif/.flac or NI's compressed .ncw (decoded here to WAV). Encrypted Player
// libraries (.nkx, licensed) are detected and refused with a clear message.
#include "MoreFormats.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <vector>

namespace omni
{
    using namespace juce;

    namespace
    {
        using Bytes = std::vector<uint8>;

        String encryptedMessage()
        {
            return String::fromUTF8 ("This Kontakt instrument is encrypted (Kontakt Player library) \xe2\x80\x94 it can only be opened in Kontakt.");
        }

        // ---------------------------------------------------------------------------------------
        // A bounds-checked reader: reading past the end sets `bad` and returns zeros, never crashes.
        struct Reader
        {
            const uint8* d = nullptr;
            size_t n = 0, pos = 0;
            bool bad = false;
            bool bigEndian = false;

            Reader() = default;
            Reader (const uint8* data, size_t size, bool be = false) : d (data), n (size), bigEndian (be) {}

            size_t left() const noexcept { return bad ? 0 : n - pos; }
            bool atEnd() const noexcept { return bad || pos >= n; }
            bool need (size_t k) { if (bad || k > n - pos) { bad = true; return false; } return true; }
            void skip (size_t k) { if (need (k)) pos += k; }

            uint32 u8()  { if (! need (1)) return 0; return d[pos++]; }
            uint32 u16()
            {
                if (! need (2)) return 0;
                const uint32 v = bigEndian ? (uint32) ((d[pos] << 8) | d[pos + 1]) : (uint32) (d[pos] | (d[pos + 1] << 8));
                pos += 2;
                return v;
            }
            uint32 u32()
            {
                if (! need (4)) return 0;
                uint32 v;
                if (bigEndian) v = ((uint32) d[pos] << 24) | ((uint32) d[pos + 1] << 16) | ((uint32) d[pos + 2] << 8) | d[pos + 3];
                else           v = ((uint32) d[pos + 3] << 24) | ((uint32) d[pos + 2] << 16) | ((uint32) d[pos + 1] << 8) | d[pos];
                pos += 4;
                return v;
            }
            uint64 u64()
            {
                if (! need (8)) return 0;
                uint64 v = 0;
                for (int i = 0; i < 8; ++i)
                    v |= (uint64) d[pos + (size_t) (bigEndian ? 7 - i : i)] << (8 * i);
                pos += 8;
                return v;
            }
            int32 i32() { return (int32) u32(); }
            int i16()   { return (int) (int16) (uint16) u16(); }
            float f32()
            {
                const uint32 v = u32();
                float f; std::memcpy (&f, &v, 4);
                return std::isfinite (f) ? f : 0.0f;
            }
            const uint8* bytes (size_t k)
            {
                if (! need (k)) return nullptr;
                auto* p = d + pos;
                pos += k;
                return p;
            }
            Bytes take (size_t k)
            {
                auto* p = bytes (k);
                return p != nullptr ? Bytes (p, p + k) : Bytes();
            }
            // UTF-16LE with a 32-bit character count in front.
            String utf16()
            {
                const uint32 count = u32();
                if (count > 100000 || ! need ((size_t) count * 2)) { bad = true; return {}; }
                String s;
                for (uint32 i = 0; i < count; ++i)
                {
                    const uint32 c = (uint32) (d[pos + i * 2] | (d[pos + i * 2 + 1] << 8));
                    if (c != 0) s += String::charToString ((juce_wchar) c);
                }
                pos += (size_t) count * 2;
                return s;
            }
        };

        // ---------------------------------------------------------------------------------------
        // FastLZ (levels 1 and 2), bounds-checked.
        bool fastLzUncompress (const uint8* in, size_t inSize, Bytes& out, size_t outSize)
        {
            if (inSize == 0 || outSize == 0 || outSize > (size_t) 1 << 30) return false;
            const int level = (in[0] >> 5) + 1;
            if (level != 1 && level != 2) return false;
            out.assign (outSize, 0);

            size_t ip = 0, op = 0;
            int64 ctrl = in[ip++] & 31;
            bool loop = true;
            while (loop)
            {
                if (ctrl >= 32)
                {
                    int64 len = (ctrl >> 5) - 1;
                    int64 ofs = (ctrl & 31) << 8;
                    int64 ref = (int64) op - ofs;
                    if (len == 6)
                    {
                        if (level == 1)
                        {
                            if (ip >= inSize) return false;
                            len += in[ip++];
                        }
                        else
                        {
                            int code;
                            do
                            {
                                if (ip >= inSize) return false;
                                code = in[ip++];
                                len += code;
                            } while (code == 255);
                        }
                    }
                    if (level == 1)
                    {
                        if (ip >= inSize) return false;
                        ref -= in[ip++];
                    }
                    else
                    {
                        if (ip >= inSize) return false;
                        const int code = in[ip++];
                        ref -= code;
                        if (code == 255 && ofs == (31 << 8))
                        {
                            if (ip + 2 > inSize) return false;
                            ofs = (int64) (in[ip++]) << 8;
                            ofs += in[ip++];
                            ref = (int64) op - ofs - 8191;
                        }
                    }
                    if ((int64) op + len + 3 > (int64) outSize || ref - 1 < 0) return false;
                    if (ip < inSize) ctrl = in[ip++]; else loop = false;

                    if (ref == (int64) op)
                    {
                        const uint8 b = out[(size_t) ref - 1];
                        for (int64 i = 0; i < len + 3; ++i) out[op++] = b;
                    }
                    else
                    {
                        --ref;
                        if (ref < 0 || ref >= (int64) op) return false;
                        for (int64 i = 0; i < len + 3; ++i) out[op++] = out[(size_t) ref++];
                    }
                }
                else
                {
                    ++ctrl;
                    if (op + (size_t) ctrl > outSize || ip + (size_t) ctrl > inSize) return false;
                    for (int64 i = 0; i < ctrl; ++i) out[op++] = in[ip++];
                    loop = ip < inSize;
                    if (loop) ctrl = in[ip++];
                }
            }
            out.resize (op);
            return true;
        }

        // ---------------------------------------------------------------------------------------
        // NCW (NI Compressed Wave): 512-sample blocks per channel, delta or bit-truncated.
        bool decodeNcw (const File& ncw, const File& wavOut, String& error)
        {
            MemoryBlock mb;
            if (! ncw.loadFileAsData (mb)) { error = "Couldn't read " + ncw.getFileName(); return false; }
            Reader r (static_cast<const uint8*> (mb.getData()), mb.getSize());
            if (r.u32() != 0xD69EA801u) { error = ncw.getFileName() + " isn't an NCW file"; return false; }
            const uint32 version = r.u32();
            if (version != 0x130 && version != 0x131) { error = "Unknown NCW version in " + ncw.getFileName(); return false; }
            const int channels = (int) r.u16();
            const int bits = (int) r.u16();
            const int rate = (int) r.u32();
            const int64 numSamples = r.u32();
            const uint32 offAddr = r.u32(), offData = r.u32();
            r.u32();
            r.skip (88);
            if (r.bad || channels < 1 || channels > 2 || bits < 8 || bits > 32 || rate < 1000 || rate > 400000
                || numSamples < 1 || numSamples > 200000000 || offData <= offAddr)
            { error = "Unsupported NCW header in " + ncw.getFileName(); return false; }

            const int numOffsets = (int) ((offData - offAddr) / 4);
            if (numOffsets < 2 || numOffsets > 4000000) { error = "Damaged NCW file " + ncw.getFileName(); return false; }
            std::vector<uint32> offsets ((size_t) numOffsets);
            for (auto& o : offsets) o = r.u32();
            if (r.bad || offsets[0] != 0) { error = "Damaged NCW file " + ncw.getFileName(); return false; }

            constexpr int kBlock = 512;
            std::vector<std::vector<int32>> pcm ((size_t) channels, std::vector<int32> ((size_t) numSamples, 0));
            bool anyFloat = false;
            for (int blk = 1; blk < numOffsets; ++blk)
            {
                if (offsets[(size_t) blk] < offsets[(size_t) blk - 1]) { error = "Damaged NCW file " + ncw.getFileName(); return false; }
                const size_t blockSize = offsets[(size_t) blk] - offsets[(size_t) blk - 1];
                const uint8* bp = r.bytes (blockSize);
                if (bp == nullptr) { error = "Damaged NCW file " + ncw.getFileName(); return false; }
                Reader b (bp, blockSize);

                const int64 start = (int64) (blk - 1) * kBlock;
                const int64 len = jmin<int64> (kBlock, numSamples - start);
                bool midSide = false, isFloat = false;
                for (int c = 0; c < channels; ++c)
                {
                    if (b.u32() != 0x3E9A0C16u) { error = "Damaged NCW block in " + ncw.getFileName(); return false; }
                    const int32 base = b.i32();
                    const int bps = b.i16();
                    const uint32 flags = b.u16();
                    b.skip (4);
                    if (flags > 3 || bps == 0 || std::abs (bps) > 32) { error = "Unsupported NCW block in " + ncw.getFileName(); return false; }
                    if (flags & 1) midSide = true;
                    if (flags & 2) isFloat = true;
                    const size_t dataBytes = (size_t) kBlock * (size_t) std::abs (bps) / 8;
                    const uint8* data = b.bytes (dataBytes);
                    if (data == nullptr) { error = "Damaged NCW block in " + ncw.getFileName(); return false; }

                    int32 samples[kBlock] = {};
                    if (bps > 0)
                    {
                        // delta: packed signed values, LSB first
                        int32 prev = base;
                        uint64 acc = 0; int accBits = 0, count = 0;
                        for (size_t i = 0; i < dataBytes && count < kBlock; ++i)
                        {
                            acc |= (uint64) data[i] << accBits;
                            accBits += 8;
                            while (accBits >= bps && count < kBlock)
                            {
                                int64 v = (int64) (acc & ((1ull << bps) - 1));
                                if (v & (1ll << (bps - 1))) v -= (1ll << bps);
                                samples[count++] = prev;
                                prev = (int32) ((uint32) prev + (uint32) v);
                                acc >>= bps;
                                accBits -= bps;
                            }
                        }
                    }
                    else
                    {
                        // truncated: |bps|-bit signed samples, packed LSB first
                        const int pb = -bps;
                        size_t bitOff = 0;
                        for (int i = 0; i < kBlock && bitOff + (size_t) pb <= dataBytes * 8; ++i, bitOff += (size_t) pb)
                        {
                            uint64 v = 0;
                            for (int k = 0; k < 5 && bitOff / 8 + (size_t) k < dataBytes; ++k)
                                v |= (uint64) data[bitOff / 8 + (size_t) k] << (8 * k);
                            v >>= (bitOff % 8);
                            v &= (pb == 64 ? ~0ull : ((1ull << pb) - 1));
                            int64 sv = (int64) v;
                            if (pb < 32 && (sv & (1ll << (pb - 1)))) sv -= (1ll << pb);
                            samples[i] = (int32) (uint32) sv;
                        }
                    }
                    for (int64 i = 0; i < len; ++i)
                        pcm[(size_t) c][(size_t) (start + i)] = samples[i];
                }
                if (isFloat) anyFloat = true;
                if (midSide && channels == 2 && ! isFloat)
                    for (int64 i = 0; i < len; ++i)
                    {
                        const int32 mid = pcm[0][(size_t) (start + i)], side = pcm[1][(size_t) (start + i)];
                        pcm[0][(size_t) (start + i)] = mid + side;
                        pcm[1][(size_t) (start + i)] = mid - side;
                    }
                else if (midSide && channels == 2 && isFloat)
                    for (int64 i = 0; i < len; ++i)
                    {
                        float m, s;
                        const int32 mi = pcm[0][(size_t) (start + i)], si = pcm[1][(size_t) (start + i)];
                        std::memcpy (&m, &mi, 4); std::memcpy (&s, &si, 4);
                        const float l = m + s, rr = m - s;
                        std::memcpy (&pcm[0][(size_t) (start + i)], &l, 4);
                        std::memcpy (&pcm[1][(size_t) (start + i)], &rr, 4);
                    }
            }

            AudioBuffer<float> buf (channels, (int) numSamples);
            const float scale = 1.0f / (float) (1ull << (jmin (bits, 32) - 1));
            for (int c = 0; c < channels; ++c)
            {
                auto* dst = buf.getWritePointer (c);
                for (int64 i = 0; i < numSamples; ++i)
                {
                    const int32 v = pcm[(size_t) c][(size_t) i];
                    if (anyFloat) { float f; std::memcpy (&f, &v, 4); dst[i] = std::isfinite (f) ? f : 0.0f; }
                    else dst[i] = (float) v * scale;
                }
            }

            wavOut.getParentDirectory().createDirectory();
            wavOut.deleteFile();
            std::unique_ptr<FileOutputStream> os (wavOut.createOutputStream());
            if (os == nullptr) { error = "Couldn't write " + wavOut.getFullPathName(); return false; }
            WavAudioFormat wav;
            const int outBits = anyFloat ? 32 : (bits <= 16 ? 16 : (bits <= 24 ? 24 : 32));
            std::unique_ptr<AudioFormatWriter> w (wav.createWriterFor (os.get(), (double) rate, (unsigned) channels, outBits, StringPairArray(), 0));
            if (w == nullptr) { error = "Couldn't write " + wavOut.getFileName(); return false; }
            os.release();
            if (! w->writeFromAudioSampleBuffer (buf, 0, (int) numSamples)) { error = "Couldn't write " + wavOut.getFileName(); return false; }
            return true;
        }

        // ---------------------------------------------------------------------------------------
        // Locating the sample files of an instrument.
        struct Locator
        {
            File nki, dir, parent;
            String instName;
            bool indexed = false;
            std::multimap<String, File> byName;       // lower-case file name
            std::multimap<String, File> byStem;       // lower-case stem of audio files

            explicit Locator (const File& f) : nki (f), dir (f.getParentDirectory()), parent (f.getParentDirectory().getParentDirectory()),
                                              instName (f.getFileNameWithoutExtension()) {}

            static bool isAudioName (const String& n) { return File (n).hasFileExtension ("wav;aif;aiff;flac;ncw;ogg;mp3"); }

            void buildIndex()
            {
                if (indexed) return;
                indexed = true;
                int visited = 0;
                auto scan = [&] (const File& root)
                {
                    if (! root.isDirectory()) return;
                    for (const auto& e : RangedDirectoryIterator (root, true, "*", File::findFiles, File::FollowSymlinks::no))
                    {
                        if (++visited > 40000) return;
                        const auto f = e.getFile();
                        const auto n = f.getFileName().toLowerCase();
                        byName.emplace (n, f);
                        if (isAudioName (n)) byStem.emplace (f.getFileNameWithoutExtension().toLowerCase(), f);
                    }
                };
                scan (dir);
                // the parent folder is searched too, unless it is a drive root or the home folder
                if (parent != dir && parent.getFullPathName().length() > 3 && parent != File::getSpecialLocation (File::userHomeDirectory))
                    scan (parent);
            }

            static File withAltExtension (const File& f)
            {
                if (f.existsAsFile()) return f;
                for (auto* ext : { ".wav", ".ncw", ".aif", ".aiff", ".flac", ".WAV", ".NCW", ".AIF" })
                {
                    auto g = f.withFileExtension (ext);
                    if (g.existsAsFile()) return g;
                }
                return {};
            }

            File find (const String& rawPath)
            {
                auto path = rawPath.replaceCharacter ('\\', '/').trim();
                if (path.isEmpty()) return {};

                if (File::isAbsolutePath (path))
                    if (auto f = withAltExtension (File (path)); f.existsAsFile()) return f;

                StringArray segs;
                segs.addTokens (path, "/", "");
                segs.removeEmptyStrings();
                for (int i = segs.size(); --i >= 0;)
                    if (segs[i] == "." || segs[i].endsWithChar (':')) segs.remove (i);
                if (segs.isEmpty()) return {};

                const File bases[] = { dir, parent, dir.getChildFile ("Samples"), parent.getChildFile ("Samples"),
                                       parent.getParentDirectory() };
                // as written (relative, possibly with ../), then ever shorter tails of the path
                if (! File::isAbsolutePath (path))
                    for (auto& b : { dir, parent })
                        if (auto f = withAltExtension (b.getChildFile (path)); f.existsAsFile()) return f;

                StringArray clean;
                for (auto& s : segs) { if (s == "..") continue; clean.add (s); }
                for (int len = clean.size(); len >= 1; --len)
                {
                    StringArray tail;
                    for (int i = clean.size() - len; i < clean.size(); ++i) tail.add (clean[i]);
                    const auto rel = tail.joinIntoString ("/");
                    for (auto& b : bases)
                        if (b.isDirectory())
                            if (auto f = withAltExtension (b.getChildFile (rel)); f.existsAsFile()) return f;
                }

                buildIndex();
                const auto name = clean[clean.size() - 1];
                if (auto it = byName.find (name.toLowerCase()); it != byName.end()) return it->second;
                if (auto it = byStem.find (File (name).getFileNameWithoutExtension().toLowerCase()); it != byStem.end()) return it->second;
                return {};
            }
        };

        // ---------------------------------------------------------------------------------------
        // The instrument as read from either format, before its samples are loaded.
        struct Candidate
        {
            Zone zone;
            String path;
        };

        struct Parsed
        {
            String name;
            std::vector<Candidate> zones;
            bool hasEnv = false;
            Envelope env;
            bool library = false;        // a sample sits in an NI library (Player) -> encrypted
            int programs = 1;
        };

        double ratioToSemis (double ratio)
        {
            return ratio > 0.0001 && std::isfinite (ratio) ? 12.0 * std::log2 (ratio) : 0.0;
        }

        float linearToDb6 (double v)       // Kontakt: the volume value doubles every 6 dB
        {
            return v > 0.0 && std::isfinite (v) ? (float) jmax (-100.0, 6.0 * std::log2 (v)) : -100.0f;
        }

        // ---------------------------------------------------------------------------------------
        // Kontakt 1 - 4.1: XML
        using ParamMap = std::map<String, String>;

        ParamMap readValues (const XmlElement* e)
        {
            ParamMap m;
            if (e == nullptr) return m;
            for (auto* v : e->getChildWithTagNameIterator ("V"))
                m[v->getStringAttribute ("name")] = v->getStringAttribute ("value");
            return m;
        }

        ParamMap readParams (const XmlElement* e)
        {
            return e != nullptr ? readValues (e->getChildByName ("Parameters")) : ParamMap();
        }

        double getNum (const ParamMap& m, const char* key, double def)
        {
            auto it = m.find (key);
            if (it == m.end() || it->second.isEmpty()) return def;
            const auto v = it->second.getDoubleValue();
            return std::isfinite (v) ? v : def;
        }

        String getStr (const ParamMap& m, const char* key)
        {
            auto it = m.find (key);
            return it == m.end() ? String() : it->second;
        }

        // Sample paths: plain, or "@" + a chain of d/b/v/m folder items and "F" + file name.
        bool decodeSamplePath (const String& enc, String& path, bool& library)
        {
            library = false;
            path = {};
            if (enc.isEmpty()) return true;
            if (enc[0] != '@') { path = enc.replaceCharacter ('\\', '/'); return true; }

            int i = 1;
            const int len = enc.length();
            auto readFolder = [&] (String& folder) -> bool
            {
                if (i + 3 > len) return false;
                const int n = enc.substring (i, i + 3).getIntValue();
                i += 3;
                if (n < 0 || i + n > len) return false;
                folder = enc.substring (i, i + n);
                i += n;
                return true;
            };
            while (i < len)
            {
                const auto c = enc[i++];
                String folder;
                if (c == 'v')       { if (! readFolder (folder)) return false; if (! folder.contains (":")) path << "/"; path << folder << "/"; }
                else if (c == 'b')  { path << "../"; }
                else if (c == 'd')  { if (! readFolder (folder)) return false; path << folder << "/"; }
                else if (c == 'm')  { if (! readFolder (folder)) return false; library = true; path = {}; }
                else if (c == 'F' || c == 'f')
                {
                    i += (c == 'f' ? 6 : 11);
                    if (i > len) return false;
                    path << enc.substring (i);
                    // %XXXX escapes (UTF-16 code units)
                    String out;
                    for (int k = 0; k < path.length(); ++k)
                    {
                        if (path[k] == '%' && k + 4 < path.length() && path.substring (k + 1, k + 5).containsOnly ("0123456789abcdefABCDEF"))
                        {
                            out += String::charToString ((juce_wchar) path.substring (k + 1, k + 5).getHexValue32());
                            k += 4;
                        }
                        else out += String::charToString (path[k]);
                    }
                    path = out;
                    return true;
                }
                else return false;
            }
            return false;
        }

        // The first volume envelope of an XML group (K1: <V name="target">, K2: <Targets><Target><V ...>).
        bool readXmlAmpEnvelope (const XmlElement& group, Envelope& env)
        {
            auto* mods = group.getChildByName ("IntModulators");
            if (mods == nullptr) return false;
            for (auto* mod : mods->getChildIterator())
            {
                auto* e = mod->getChildByName ("Envelope");
                if (e == nullptr) continue;
                auto self = readValues (mod);
                if (getStr (self, "bypass") == "yes") continue;
                String target = getStr (self, "target");
                double intensity = getNum (self, "intensity", 0.0);
                if (target.isEmpty())
                    if (auto* t = mod->getChildByName ("Targets"))
                        if (auto* t0 = t->getChildByName ("Target"))
                        {
                            auto tv = readValues (t0);
                            target = getStr (tv, "target");
                            intensity = getNum (tv, "intensity", 0.0);
                        }
                if (target != "volume" || std::abs (intensity) < 1.0e-9) continue;
                const auto type = e->getStringAttribute ("type");
                if (type != "ahdsr" && type != "ahd") continue;
                auto v = readValues (e);
                const bool ahd = (type == "ahd");
                env.attack  = (float) jmax (0.0005, (getNum (v, "attack", 1.0) + getNum (v, "hold", 0.0)) / 1000.0);
                env.decay   = (float) jmax (0.01, getNum (v, "decay", 300.0) / 1000.0);
                env.sustain = ahd ? 0.0f : (float) jlimit (0.0, 1.0, getNum (v, "sustain", 1.0));
                env.release = (float) jmax (0.01, (ahd ? getNum (v, "decay", 300.0) : getNum (v, "release", 250.0)) / 1000.0);
                return true;
            }
            return false;
        }

        bool parseXmlProgram (const XmlElement& prog, bool isK1, Parsed& out, String& error)
        {
            const auto pp = readParams (&prog);
            const auto progName = prog.getStringAttribute ("name");
            if (progName.isNotEmpty() && progName != "Instrument 1" && progName != "(null)") out.name = progName;

            // K2 program version >= 0.8 (Kontakt 3 and 4) stores program and group tuning as an offset in
            // octaves; Kontakt 1/2 stores a frequency ratio. The zone tuning is always a ratio.
            const bool additiveTune = ! isK1 && prog.getStringAttribute ("version").getDoubleValue() >= 0.8;
            auto tuneRatio = [&] (double t) { return additiveTune ? std::pow (2.0, t) : t; };
            auto pan = [&] (double v) { return isK1 ? (v - 0.5) / 0.5 : v; };

            const double progVol = getNum (pp, "masterVolume", getNum (pp, "volume", 1.0));
            const double progPan = pan (getNum (pp, "masterPan", getNum (pp, "pan", isK1 ? 0.5 : 0.0)));
            const double progTune = tuneRatio (getNum (pp, "masterTune", getNum (pp, "tune", additiveTune ? 0.0 : 1.0)));

            auto* groups = prog.getChildByName ("Groups");
            auto* zones = prog.getChildByName ("Zones");
            if (groups == nullptr || zones == nullptr) return false;

            const char* groupTag = isK1 ? "NiSS_Group" : "K2_Group";
            const char* zoneTag = isK1 ? "NiSS_Zone" : "K2_Zone";

            for (auto* g : groups->getChildWithTagNameIterator (groupTag))
            {
                const auto gp = readParams (g);
                if (getStr (gp, "releaseTrigger") == "yes") continue;                  // note-off samples
                if (getStr (gp, "muted") == "yes" || getStr (gp, "m_bMuted") == "yes") continue;

                const int index = g->getIntAttribute ("index", -1);
                const double gVol = getNum (gp, "volume", 1.0);
                const double gPan = pan (getNum (gp, "pan", isK1 ? 0.5 : 0.0));
                const double gTune = tuneRatio (getNum (gp, "tune", additiveTune ? 0.0 : 1.0));

                int rrPos = 0;
                if (auto* gs = g->getChildByName ("GroupStart"))
                    for (auto* sc : gs->getChildWithTagNameIterator ("StartCriteria"))
                    {
                        auto v = readValues (sc);
                        if (getStr (v, "mode").startsWith ("cycle"))
                        {
                            const int cc = (int) getNum (v, "cycleClass", -1.0);
                            if (cc >= 0) rrPos = cc + 1;
                            break;
                        }
                    }

                if (! out.hasEnv && readXmlAmpEnvelope (*g, out.env)) out.hasEnv = true;

                for (auto* ze : zones->getChildWithTagNameIterator (zoneTag))
                {
                    if (ze->getIntAttribute ("groupIdx", -2) != index) continue;
                    const auto zp = readParams (ze);

                    Candidate c;
                    auto& z = c.zone;
                    z.root = jlimit (0, 127, (int) getNum (zp, "rootKey", 60));
                    z.loKey = jlimit (0, 127, (int) getNum (zp, "lowKey", 0));
                    z.hiKey = jlimit (z.loKey, 127, (int) getNum (zp, "highKey", 127));
                    z.loVel = jlimit (1, 127, (int) getNum (zp, "lowVelocity", 1));
                    z.hiVel = jlimit (z.loVel, 127, (int) getNum (zp, "highVelocity", 127));
                    z.start = jmax (0, (int) getNum (zp, "sampleStart", 0));
                    z.gainDb = linearToDb6 (getNum (zp, "zoneVolume", 1.0) * gVol * progVol);
                    z.pan = jlimit (-1.0f, 1.0f, (float) (pan (getNum (zp, "zonePan", isK1 ? 0.5 : 0.0)) + gPan + progPan));
                    z.tuneCents = (float) jlimit (-4800.0, 4800.0, 100.0 * ratioToSemis (getNum (zp, "zoneTune", 1.0) * gTune * progTune));
                    z.rrPos = rrPos;

                    if (auto* loops = ze->getChildByName ("Loops"))
                        for (auto* l : loops->getChildWithTagNameIterator ("Loop"))
                        {
                            auto lv = readValues (l);
                            if (getStr (lv, "mode").contains ("until"))
                            {
                                z.loop = true;
                                z.loopStart = jmax (0, (int) getNum (lv, "loopStart", 0));
                                z.loopEnd = z.loopStart + jmax (1, (int) getNum (lv, "loopLength", 1)) - 1;
                                break;
                            }
                        }

                    if (auto* s = ze->getChildByName ("Sample"))
                    {
                        auto sv = readValues (s);
                        String enc = getStr (sv, "file_ex2");
                        if (enc.isEmpty()) enc = getStr (sv, "file_ex");
                        if (enc.isEmpty()) enc = getStr (sv, "file");
                        bool library = false;
                        if (! decodeSamplePath (enc, c.path, library))
                        {
                            error = "A sample path in this instrument has a format that can't be read yet";
                            return false;
                        }
                        if (library) out.library = true;
                    }
                    if (c.path.isNotEmpty()) out.zones.push_back (std::move (c));
                }
            }
            return true;
        }

        bool parseXmlInstrument (const String& text, bool isK1, Parsed& out, String& error)
        {
            auto xml = XmlDocument::parse (text.trim());
            if (xml == nullptr) { error = "The instrument's XML couldn't be parsed"; return false; }

            std::vector<const XmlElement*> programs;
            if (isK1)
            {
                if (xml->hasTagName ("NiSS_Program")) programs.push_back (xml.get());
                else for (auto* p : xml->getChildWithTagNameIterator ("NiSS_Program")) programs.push_back (p);
            }
            else
            {
                std::vector<const XmlElement*> containers;
                std::function<void (const XmlElement*)> walk = [&] (const XmlElement* e)
                {
                    if (e->hasTagName ("K2_Container")) containers.push_back (e);
                    else for (auto* c : e->getChildIterator()) walk (c);
                };
                walk (xml.get());
                for (auto* c : containers)
                    if (auto* ps = c->getChildByName ("Programs"))
                        for (auto* p : ps->getChildWithTagNameIterator ("K2_Program")) programs.push_back (p);
            }
            if (programs.empty()) { error = "No program found in this Kontakt instrument"; return false; }

            bool sawLibrary = false;
            for (auto* p : programs)
            {
                Parsed one;
                String e;
                if (! parseXmlProgram (*p, isK1, one, e))
                {
                    if (e.isNotEmpty()) { error = e; return false; }
                    continue;
                }
                if (one.library) sawLibrary = true;
                if (! one.zones.empty())
                {
                    one.programs = (int) programs.size();
                    out = std::move (one);
                    return true;
                }
            }
            out.library = sawLibrary;
            error = "This Kontakt instrument has no zones with samples";
            return false;
        }

        // ---------------------------------------------------------------------------------------
        // Kontakt 4.2+: binary preset chunks
        namespace ids
        {
            constexpr int bank = 0x03, group = 0x04, parScript = 0x06, fxSend = 0x17, program = 0x28, programContainer = 0x29,
                          voiceGroups = 0x32, groupList = 0x33, zoneList = 0x34, loopArray = 0x39, par8 = 0x3A, par16 = 0x3B,
                          par32 = 0x3C, fileList = 0x3D, insertBus = 0x45, saveSettings = 0x47, fileListEx = 0x4B,
                          starCrit = 0x38, quickBrowse = 0x4E;
        }

        struct PChunk
        {
            int id = -1;
            int version = -1;
            Bytes pub, priv;
            std::vector<PChunk> children;
        };

        bool readStructure (Reader& in, size_t objectSize, PChunk& c, int depth);

        bool readChunk (Reader& in, PChunk& c, int depth)
        {
            if (depth > 24) return false;
            c.id = (int) in.u16();
            const size_t size = in.u32();
            const uint8* p = in.bytes (size);
            if (in.bad || p == nullptr) return false;
            Reader sub (p, size);

            switch (c.id)
            {
                case ids::groupList:
                case ids::zoneList:
                {
                    const bool hasRef = (c.id == ids::zoneList);
                    const uint32 count = sub.u32();
                    if (count > 200000) return false;
                    for (uint32 i = 0; i < count; ++i)
                    {
                        PChunk el;
                        el.id = hasRef ? (int) sub.u32() : ids::group;
                        if (sub.bad || ! readStructure (sub, size, el, depth + 1)) return false;
                        c.children.push_back (std::move (el));
                    }
                    return ! sub.bad;
                }
                case ids::bank: case ids::programContainer: case ids::program: case ids::parScript: case ids::fxSend:
                case ids::voiceGroups: case ids::par8: case ids::insertBus: case ids::saveSettings: case ids::quickBrowse:
                    return readStructure (sub, size, c, depth + 1);
                case ids::par16: case ids::par32:
                {
                    if (sub.u8() != 0) return false;
                    c.version = (int) sub.u16();
                    size_t slots = (c.id == ids::par16 ? 16 : 32);
                    if (c.version >= 0x13)
                    {
                        slots = sub.u32();
                        if (slots > 4096) return false;
                    }
                    for (size_t i = 0; i < slots && ! sub.bad; ++i)
                        if (sub.u8() > 0)
                        {
                            PChunk ch;
                            ch.id = (int) sub.u16();
                            ch.pub = sub.take (sub.u32());
                            c.children.push_back (std::move (ch));
                        }
                    return ! sub.bad;
                }
                default:
                    c.pub.assign (p, p + size);
                    return true;
            }
        }

        bool readStructure (Reader& in, size_t objectSize, PChunk& c, int depth)
        {
            const bool structured = in.u8() > 0;
            if (in.bad) return false;
            if (! structured)
            {
                c.pub = in.take (jmin (objectSize > 0 ? objectSize - 1 : 0, in.left()));
                return ! in.bad;
            }
            c.version = (int) in.u16();
            c.priv = in.take (in.u32());
            c.pub = in.take (in.u32());
            const size_t childBytes = in.u32();
            const uint8* cp = in.bytes (childBytes);
            if (in.bad || cp == nullptr) return false;
            Reader kids (cp, childBytes);
            while (! kids.atEnd())
            {
                PChunk k;
                if (! readChunk (kids, k, depth + 1)) return false;
                c.children.push_back (std::move (k));
            }
            return true;
        }

        void findPrograms (const std::vector<PChunk>& list, std::vector<const PChunk*>& out)
        {
            for (auto& c : list)
            {
                if (c.id == ids::program) out.push_back (&c);
                else findPrograms (c.children, out);
            }
        }

        // The file list: segments of UTF-16 strings; segment types 0/1 drive, 2 folder, 3 parent, 4/8/9 file.
        bool readFileName (Reader& in, String& out)
        {
            const uint32 segs = in.u32();
            if (segs > 1000) return false;
            for (uint32 s = 0; s < segs; ++s)
            {
                const auto t = in.u8();
                switch (t)
                {
                    case 0: { auto* a = in.bytes (2); if (a == nullptr) return false;
                              String d; for (int i = 0; i < 2; ++i) if (a[i] > 32) d += String::charToString ((juce_wchar) a[i]);
                              out << (d.isEmpty() ? String ("/") : d + ":/"); break; }
                    case 1: { auto d = in.utf16(); out << (d.isEmpty() ? String ("/") : d + ":/"); break; }
                    case 2: out << in.utf16() << "/"; break;
                    case 3: out << "../"; break;
                    case 4: case 8: case 9: out << in.utf16(); break;
                    case 6: break;
                    default: return false;
                }
                if (in.bad) return false;
            }
            return true;
        }

        bool readFileGroup (Reader& in, StringArray& files)
        {
            if (in.atEnd()) return true;
            const int count = in.i32();
            if (count < 0 || count > 200000) return false;
            for (int i = 0; i < count; ++i)
            {
                String f;
                if (! readFileName (in, f)) return false;
                files.add (f);
            }
            return true;
        }

        bool readFileList (const PChunk& c, StringArray& files)
        {
            Reader in (c.pub.data(), c.pub.size());
            if (c.id == ids::fileListEx)
            {
                const uint32 v = in.u16();
                if (v < 2 || v > 3) return false;
                if (v == 3)
                {
                    if (in.atEnd()) return true;
                    const int count = in.i32();
                    in.skip (8);
                    if (count < 0 || count > 200000) return false;
                    for (int i = 0; i < count - 1; ++i)
                    {
                        String f;
                        if (! readFileName (in, f)) return false;
                        files.add (f);
                        if (! in.atEnd()) in.skip (4 + 4 + 20);
                    }
                    return ! in.bad;
                }
            }
            StringArray special;
            if (! readFileGroup (in, special)) return false;      // special files
            return readFileGroup (in, files);                     // sample files
        }

        struct KGroup
        {
            float vol = 1, pan = 0, tune = 1;
            bool releaseTrigger = false, muted = false, soloed = false;
            int rrPos = 0;
        };

        struct KLoop { int mode = 0, start = 0, length = 0; };

        struct KZone
        {
            int groupIdx = 0, sampleStart = 0, loVel = 1, hiVel = 127, loKey = 0, hiKey = 127, root = 60;
            float vol = 1, pan = 0, tune = 1;
            int fileId = -1;
            std::vector<KLoop> loops;
        };

        bool parseGroup (const PChunk& c, KGroup& g, String& error)
        {
            if (c.version > 0x9C)
            {
                error = "This Kontakt version (group format 0x" + String::toHexString (c.version).toUpperCase() + ") can't be read yet";
                return false;
            }
            Reader in (c.pub.data(), c.pub.size());
            in.utf16();
            g.vol = in.f32(); g.pan = in.f32(); g.tune = in.f32();
            in.u8();                      // key tracking
            in.u8();                      // reverse
            g.releaseTrigger = in.u8() > 0;
            in.u8();
            in.u32(); in.u16(); in.u32(); in.u32();
            g.muted = in.u8() > 0;
            g.soloed = in.u8() > 0;
            if (in.bad) { error = "A Kontakt group couldn't be read"; return false; }

            for (auto& ch : c.children)
                if (ch.id == ids::starCrit && ! ch.pub.empty())
                {
                    const auto& d = ch.pub;
                    const int count = d[0];
                    for (int i = 0; i < count; ++i)
                    {
                        const size_t off = 1 + (size_t) i * 34;
                        if (off + 34 > d.size() || (d[off + 1] | (d[off + 2] << 8)) != 0x70) break;
                        const uint32 mode = (uint32) d[off + 3] | ((uint32) d[off + 4] << 8) | ((uint32) d[off + 5] << 16) | ((uint32) d[off + 6] << 24);
                        if (mode == 3 || mode == 4)
                        {
                            g.rrPos = (int) (d[off + 21] | (d[off + 22] << 8)) + 1;
                            break;
                        }
                    }
                }
            return true;
        }

        bool parseZone (const PChunk& c, KZone& z, String& error)
        {
            if (c.version > 0x9C)
            {
                error = "This Kontakt version (zone format 0x" + String::toHexString (c.version).toUpperCase() + ") can't be read yet";
                return false;
            }
            z.groupIdx = c.id;
            Reader in (c.pub.data(), c.pub.size());
            z.sampleStart = (int) in.u32();
            in.u32(); in.u32();
            z.loVel = (int) in.u16(); z.hiVel = (int) in.u16();
            z.loKey = (int) in.u16(); z.hiKey = (int) in.u16();
            in.skip (8);                  // crossfades
            z.root = (int) in.u16();
            z.vol = in.f32(); z.pan = in.f32(); z.tune = in.f32();
            if (c.version >= 0x9A) { in.u8(); in.u8(); in.u32(); }
            if (in.bad) { error = "A Kontakt zone couldn't be read"; return false; }
            z.fileId = in.atEnd() ? -1 : (int) in.u32();     // -1: a zone without a sample

            for (auto& ch : c.children)
                if (ch.id == ids::loopArray && ch.pub.size() >= 2)
                {
                    Reader l (ch.pub.data(), ch.pub.size());
                    const uint32 enabled = l.u16();
                    for (int i = 0; i < 8 && ! l.bad; ++i)
                        if (enabled & (1u << i))
                        {
                            if (l.u16() != 0x60) break;
                            KLoop k;
                            k.mode = l.i32(); k.start = (int) l.u32(); k.length = (int) l.u32();
                            l.u32(); l.u8(); l.f32(); l.u32();
                            if (l.left() > 0) l.skip (1);
                            if (l.bad) break;
                            z.loops.push_back (k);
                        }
                }
            return true;
        }

        bool parseBinaryPreset (const Bytes& presetData, const String& versionText, Parsed& out, String& error)
        {
            auto cantRead = [&] (const String& why)
            {
                error = "This Kontakt version (" + (versionText.isNotEmpty() ? versionText : String ("unknown")) + ") can't be read yet" + (why.isNotEmpty() ? " - " + why : String());
            };

            std::vector<PChunk> top;
            Reader in (presetData.data(), presetData.size());
            while (! in.atEnd())
            {
                PChunk c;
                if (! readChunk (in, c, 0)) { cantRead ("unknown preset layout"); return false; }
                top.push_back (std::move (c));
            }

            std::vector<const PChunk*> programs;
            findPrograms (top, programs);
            if (programs.empty()) { error = "No program found in this Kontakt instrument"; return false; }

            StringArray files;
            for (auto& c : top)
                if (c.id == ids::fileListEx || c.id == ids::fileList)
                {
                    files.clear();
                    if (! readFileList (c, files)) { cantRead ("unknown file list layout"); return false; }
                    if (c.id == ids::fileListEx) break;
                }

            out.programs = (int) programs.size();
            for (auto* prog : programs)
            {
                Reader pr (prog->pub.data(), prog->pub.size());
                const auto name = pr.utf16();
                pr.skip (8);              // size of all samples (double)
                pr.skip (1);              // transpose
                float progVol = pr.f32(), progPan = pr.f32(), progTune = pr.f32();
                pr.skip (4 + 2 + 16);     // key/velocity clip, default key switch, dfd size, library id, fingerprint, flags
                const bool groupSolo = pr.u8() > 0;
                if (pr.bad) { cantRead ("unknown program layout"); return false; }
                if (progVol <= 0) progVol = 1;
                if (progTune <= 0) progTune = 1;

                std::map<int, KGroup> groups;
                std::vector<KZone> zones;
                int gi = 0;
                for (auto& c : prog->children)
                {
                    if (c.id == ids::groupList)
                        for (auto& g : c.children)
                        {
                            KGroup kg;
                            if (! parseGroup (g, kg, error)) return false;
                            groups[gi++] = kg;
                        }
                    else if (c.id == ids::zoneList)
                        for (auto& z : c.children)
                        {
                            KZone kz;
                            if (! parseZone (z, kz, error)) return false;
                            zones.push_back (std::move (kz));
                        }
                }

                bool anySolo = false;
                if (groupSolo) for (auto& g : groups) if (g.second.soloed) anySolo = true;

                for (auto& kz : zones)
                {
                    if (kz.fileId < 0) continue;
                    auto git = groups.find (kz.groupIdx);
                    if (git == groups.end()) { cantRead ("a zone refers to a missing group"); return false; }
                    const auto& g = git->second;
                    if (g.releaseTrigger || (anySolo ? ! g.soloed : g.muted)) continue;
                    if (kz.fileId >= files.size()) { cantRead ("a zone refers to a missing sample file"); return false; }

                    Candidate c;
                    auto& z = c.zone;
                    z.root = jlimit (0, 127, kz.root);
                    z.loKey = jlimit (0, 127, kz.loKey);
                    z.hiKey = jlimit (z.loKey, 127, kz.hiKey);
                    z.loVel = jlimit (1, 127, kz.loVel);
                    z.hiVel = jlimit (z.loVel, 127, kz.hiVel);
                    z.start = jmax (0, kz.sampleStart);
                    z.gainDb = linearToDb6 ((double) progVol * g.vol * kz.vol);
                    z.pan = jlimit (-1.0f, 1.0f, progPan + g.pan + kz.pan);
                    z.tuneCents = (float) jlimit (-4800.0, 4800.0, 100.0 * ratioToSemis ((double) kz.tune * (g.tune > 0 ? g.tune : 1.0f) * progTune));
                    z.rrPos = g.rrPos;
                    for (auto& l : kz.loops)
                        if (l.mode == 1 || l.mode == 2)
                        {
                            z.loop = true;
                            z.loopStart = jmax (0, l.start);
                            z.loopEnd = z.loopStart + jmax (1, l.length) - 1;
                            break;
                        }
                    c.path = files[kz.fileId];
                    out.zones.push_back (std::move (c));
                }
                if (! out.zones.empty())
                {
                    out.name = name;
                    return true;
                }
            }
            error = "This Kontakt instrument has no zones with samples";
            return false;
        }

        // ---------------------------------------------------------------------------------------
        // The NI container (Kontakt 5 and later): nested "hsin" items made of chunk chains.
        struct Container
        {
            std::deque<Bytes> keep;            // decompressed sub trees stay alive while we walk
            Bytes preset;
            bool havePreset = false, encrypted = false, hasSerial = false, notKontakt = false;
            String appVersion;
            int depth = 0;
        };

        bool walkItem (const uint8* d, size_t n, Container& c, size_t& consumed);

        bool walkChunkChain (const uint8* d, size_t n, Container& c, size_t& consumed)
        {
            struct Hdr { uint32 type = 0; size_t blockEnd = 0, dataBegin = 0; };
            std::vector<Hdr> chain;
            size_t pos = 0;
            for (int level = 0; level < 64; ++level)
            {
                if (pos + 8 > n) return false;
                Reader r (d + pos, n - pos);
                const uint64 size = r.u64();
                if (size < 8 + 12 || size > n - pos) return false;
                const size_t blockBegin = pos + 8, blockEnd = pos + (size_t) size;
                r.skip (4);                                   // domain
                Hdr h;
                h.type = r.u32();
                if (r.u32() != 1) return false;               // chunk version
                h.blockEnd = blockEnd;
                h.dataBegin = blockBegin + 12;
                chain.push_back (h);
                if (h.type == 1) break;                       // terminator
                pos = blockBegin + 12;
                if (level == 63) return false;
            }
            if (chain.empty() || chain.back().type != 1) return false;
            consumed = chain.front().blockEnd;

            for (size_t k = 0; k < chain.size(); ++k)
            {
                const size_t begin = (k + 1 < chain.size()) ? chain[k + 1].blockEnd : chain[k].dataBegin;
                const size_t end = chain[k].blockEnd;
                if (begin > end) return false;
                Reader r (d + begin, end - begin);
                switch (chain[k].type)
                {
                    case 101:                                 // authoring application
                    {
                        r.u32(); r.u8();
                        const uint32 app = r.u32();
                        r.u32();
                        c.appVersion = r.utf16();
                        if (! r.bad && app != 2) c.notKontakt = true;
                        break;
                    }
                    case 106:                                 // authorization: serial numbers mean a licensed library
                    {
                        r.u32();
                        if (r.u32() == 1)
                        {
                            r.u32();
                            const uint32 pids = r.u32();
                            for (uint32 i = 0; i < jmin<uint32> (pids, 64) && ! r.bad; ++i)
                                if (r.utf16().trim().isNotEmpty()) c.hasSerial = true;
                        }
                        break;
                    }
                    case 109:                                 // preset chunk item
                    {
                        if (c.havePreset) break;
                        r.u32(); r.u32();
                        if (r.u32() != 1) break;
                        const uint32 size = r.u32();
                        r.u32();
                        auto bytes = r.take (size);
                        if (! r.bad) { c.preset = std::move (bytes); c.havePreset = true; }
                        break;
                    }
                    case 115:                                 // sub tree
                    {
                        r.u32();
                        const bool compressed = r.u8() > 0;
                        if (c.depth > 8) break;
                        ++c.depth;
                        size_t used = 0;
                        if (compressed)
                        {
                            const uint32 usize = r.u32(), csize = r.u32();
                            const uint8* cp = r.bytes (csize);
                            if (r.bad || cp == nullptr) { c.encrypted = true; --c.depth; break; }
                            Bytes plain;
                            if (! fastLzUncompress (cp, csize, plain, usize)) { c.encrypted = true; --c.depth; break; }
                            c.keep.push_back (std::move (plain));
                            const auto& kept = c.keep.back();
                            if (! walkItem (kept.data(), kept.size(), c, used)) c.encrypted = true;
                        }
                        else if (! walkItem (d + begin + r.pos, r.left(), c, used)) c.encrypted = true;
                        --c.depth;
                        break;
                    }
                    default: break;
                }
            }
            return true;
        }

        bool walkItem (const uint8* d, size_t n, Container& c, size_t& consumed)
        {
            Reader r (d, n);
            const uint64 size = r.u64();
            if (size < 8 + 4 + 4 + 8 + 16 || size > n) return false;
            Reader b (d + 8, (size_t) size - 8);
            if (b.u32() != 1) return false;
            const auto* dom = b.bytes (4);
            if (dom == nullptr || std::memcmp (dom, "hsin", 4) != 0) return false;
            b.skip (8 + 16);
            size_t used = 0;
            if (b.bad || ! walkChunkChain (d + 8 + b.pos, b.left(), c, used)) return false;
            b.skip (used);
            b.u32();                                        // item version
            const uint32 children = b.u32();
            if (b.bad || children > 100000) return false;
            for (uint32 i = 0; i < children; ++i)
            {
                b.u32(); b.skip (4); b.u32();               // index, domain, type
                if (b.bad) return false;
                size_t childUsed = 0;
                if (! walkItem (d + 8 + b.pos, b.left(), c, childUsed)) return false;
                b.skip (childUsed);
            }
            consumed = (size_t) size;
            return true;
        }

        String versionString (const uint8* v, bool bigEndian)
        {
            uint8 b[4] = { v[0], v[1], v[2], v[3] };
            if (! bigEndian) { std::swap (b[0], b[3]); std::swap (b[1], b[2]); }
            String s;
            s << (int) b[0] << "." << (int) b[1] << "." << (int) b[2] << ".";
            s << (b[3] == 0xFF ? String ("?") : String ((int) b[3]).paddedLeft ('0', 3));
            return s;
        }
    }

    // -------------------------------------------------------------------------------------------
    bool loadKontakt (const File& file, SampleCache& cache, KeysMap& out, String& error)
    {
        MemoryBlock raw;
        if (! file.loadFileAsData (raw) || raw.getSize() < 16)
        {
            error = "Couldn't read " + file.getFileName();
            return false;
        }
        const auto* data = static_cast<const uint8*> (raw.getData());
        const size_t size = raw.getSize();
        const uint32 id = ((uint32) data[0] << 24) | ((uint32) data[1] << 16) | ((uint32) data[2] << 8) | data[3];

        Parsed parsed;
        String versionText;

        if (std::memcmp (data + 12, "hsin", 4) == 0)
        {
            // Kontakt 5 and later
            Container c;
            size_t used = 0;
            const bool ok = walkItem (data, size, c, used);
            if (c.notKontakt) { error = "This NI file isn't a Kontakt instrument"; return false; }
            if (! c.havePreset)
            {
                error = (c.encrypted || c.hasSerial) ? encryptedMessage()
                                                     : (ok ? String ("This Kontakt file has no instrument data in it")
                                                           : String ("This Kontakt version can't be read yet (unknown container layout)"));
                return false;
            }
            if (! parseBinaryPreset (c.preset, c.appVersion, parsed, error)) return false;
        }
        else if (id == 0x2F5C204E)
        {
            error = "This Kontakt instrument has its samples embedded in the file (monolith) - that can't be read yet";
            return false;
        }
        else if (id == 0x5EE56EB3 || id == 0xB36EE55E || id == 0x1290A87F || id == 0x7FA89012)
        {
            // Kontakt 1, 1.5, 2 - 4.2: the first 4 bytes tell the byte order of the numbers in the file.
            const bool bigEndian = (id == 0xB36EE55E || id == 0x7FA89012);
            Reader r (data, size, bigEndian);
            r.skip (4);
            const uint32 first = r.u32();                     // 0x24 = Kontakt 1 header, otherwise the Kontakt 2+ header

            if (id == 0x5EE56EB3 || id == 0xB36EE55E || first == 0x24)
            {
                versionText = "1";
                MemoryInputStream mis (data + 36, size - 36, false);
                GZIPDecompressorInputStream gz (&mis, false, GZIPDecompressorInputStream::zlibFormat);
                MemoryBlock xml;
                gz.readIntoMemoryBlock (xml);
                if (xml.getSize() == 0) { error = "Couldn't decompress this Kontakt 1 instrument (damaged file?)"; return false; }
                auto text = String::fromUTF8 (static_cast<const char*> (xml.getData()), (int) xml.getSize());
                if (! parseXmlInstrument (text, true, parsed, error))
                {
                    if (parsed.library) { error = encryptedMessage(); }
                    return false;
                }
            }
            else
            {
                if (size < 222) { error = "This Kontakt file is too short"; return false; }
                Reader h (data, size, bigEndian);
                h.skip (4);
                const uint32 compressedLength = h.u32();
                const uint32 headerVersion = h.u16();
                h.skip (4 + 2);                                // magic, patch type
                versionText = versionString (data + 16, bigEndian);
                String library;
                for (int i = 0; i < 4; ++i)
                {
                    const char ch = (char) data[20 + (bigEndian ? i : 3 - i)];
                    library += String::charToString ((juce_wchar) ((ch >= 32 && ch < 127) ? ch : '?'));
                }
                const bool monolith = Reader (data + 42, 4, bigEndian).u32() == 1;
                const bool fourDotTwo = (headerVersion == 0x110);
                const size_t dataStart = fourDotTwo ? 222 : 170;
                if (monolith)
                {
                    error = "This Kontakt instrument has its samples embedded in the file (monolith) - that can't be read yet";
                    return false;
                }
                if (dataStart >= size) { error = "This Kontakt file is too short"; return false; }

                if (fourDotTwo)
                {
                    Reader f (data + 182, size - 182, bigEndian);
                    f.u32();                                  // crc32
                    const uint32 decompressedLength = f.u32();
                    const size_t len = (compressedLength > 0 && compressedLength <= size - dataStart) ? compressedLength : size - dataStart;
                    Bytes plain;
                    if (! fastLzUncompress (data + dataStart, len, plain, decompressedLength))
                    {
                        error = library.startsWith ("Kon") ? String ("Couldn't decompress this Kontakt instrument (damaged file?)") : encryptedMessage();
                        return false;
                    }
                    if (! parseBinaryPreset (plain, versionText, parsed, error)) return false;
                }
                else
                {
                    const size_t len = (compressedLength > 0 && compressedLength <= size - dataStart) ? compressedLength : size - dataStart;
                    MemoryInputStream mis (data + dataStart, len, false);
                    GZIPDecompressorInputStream gz (&mis, false, GZIPDecompressorInputStream::zlibFormat);
                    MemoryBlock xml;
                    gz.readIntoMemoryBlock (xml);
                    if (xml.getSize() == 0)
                    {
                        error = library.startsWith ("Kon") ? String ("Couldn't decompress this Kontakt instrument (damaged file?)") : encryptedMessage();
                        return false;
                    }
                    auto text = String::fromUTF8 (static_cast<const char*> (xml.getData()), (int) xml.getSize());
                    if (XmlDocument::parse (text.trim()) == nullptr)
                    {
                        text = {};
                        const auto* bp = static_cast<const uint8*> (xml.getData());
                        for (size_t i = 0; i < xml.getSize(); ++i) text += String::charToString ((juce_wchar) bp[i]);
                    }
                    if (! parseXmlInstrument (text, false, parsed, error))
                    {
                        if (parsed.library) { error = encryptedMessage(); }
                        return false;
                    }
                }
            }
        }
        else if (id == 0x5AE5D6A4 || id == 0xA4D6E55A || id == 0x01EF85AB || id == 0xAB85EF01)
        {
            error = "This is a Kontakt multi (.nkm), not an instrument";
            return false;
        }
        else
        {
            error = "Not a Kontakt instrument (unknown file ID 0x" + String::toHexString ((int) id).toUpperCase() + ")";
            return false;
        }

        if (parsed.library)
        {
            error = encryptedMessage();
            return false;
        }
        if (parsed.zones.empty())
        {
            error = "This Kontakt instrument has no zones with samples";
            return false;
        }

        // ---- load the samples
        Locator loc (file);
        KeysMap km;
        String firstMissing, firstFail;
        int missing = 0, failed = 0;
        for (auto& c : parsed.zones)
        {
            auto sampleFile = loc.find (c.path);
            if (! sampleFile.existsAsFile())
            {
                ++missing;
                if (firstMissing.isEmpty()) firstMissing = c.path.replaceCharacter ('\\', '/').fromLastOccurrenceOf ("/", false, false);
                continue;
            }
            String err;
            if (sampleFile.hasFileExtension ("ncw"))
            {
                auto wav = extractedSamplesFolder (loc.instName).getChildFile (sampleFile.getFileNameWithoutExtension() + ".wav");
                if (! wav.existsAsFile() || wav.getSize() < 64)
                    if (! decodeNcw (sampleFile, wav, err))
                    {
                        ++failed;
                        if (firstFail.isEmpty()) firstFail = err;
                        continue;
                    }
                sampleFile = wav;
            }
            c.zone.sample = cache.get (sampleFile, err);
            if (! c.zone.sample)
            {
                ++failed;
                if (firstFail.isEmpty()) firstFail = err;
                continue;
            }
            if (c.zone.loop)
            {
                const int len = c.zone.sample->length();
                if (len < 2) c.zone.loop = false;
                else
                {
                    c.zone.loopStart = jlimit (0, len - 2, c.zone.loopStart);
                    c.zone.loopEnd = jlimit (c.zone.loopStart + 1, len - 1, c.zone.loopEnd);
                }
            }
            km.zones.push_back (std::move (c.zone));
        }

        if (km.zones.empty())
        {
            error = firstFail.isNotEmpty() ? firstFail
                                           : "Couldn't find the samples of this instrument, e.g. " + firstMissing + " (keep them next to the .nki or in a Samples folder)";
            return false;
        }

        out.zones = std::move (km.zones);
        out.name = parsed.name.isNotEmpty() ? parsed.name : file.getFileNameWithoutExtension();
        if (parsed.hasEnv) out.voicing.env = parsed.env;

        StringArray notes;
        if (missing > 0) notes.add (String (missing) + " sample(s) couldn't be found, e.g. " + firstMissing);
        if (failed > 0) notes.add (String (failed) + " sample(s) couldn't be loaded: " + firstFail);
        if (parsed.programs > 1) notes.add ("the file has " + String (parsed.programs) + " programs; the first one was loaded");
        error = notes.joinIntoString ("; ");
        return true;
    }
}
