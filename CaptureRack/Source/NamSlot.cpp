#include "NamSlot.h"
#include "NAM/get_dsp.h"
#include "NAM/dsp.h"

namespace rack
{
    LoadedModel::LoadedModel() = default;
    LoadedModel::~LoadedModel() = default;

    void Resampled::prepare (double hostRate, double modelRate, int maxBlock)
    {
        active = modelRate > 0 && std::abs (hostRate - modelRate) > 0.5;
        if (! active) return;
        speedUp = hostRate / modelRate;     // host samples per model sample
        speedDown = modelRate / hostRate;   // model samples per host sample
        const size_t cap = (size_t) (maxBlock * 8 + 1024);
        for (auto* v : { &inFifo, &mid, &modelIn, &modelOut, &outFifo, &tmp }) v->assign (cap, 0.0f);
        up.reset(); down.reset();
        inCount = midCount = 0;
        // Pre-fill the output so the chain never runs dry: a fixed, small latency.
        outCount = 64;
        std::fill (outFifo.begin(), outFifo.begin() + (long) outCount, 0.0f);
    }

    void Resampled::process (nam::DSP& model, float* io, int n) noexcept
    {
        const size_t cap = inFifo.size();
        if (inCount + (size_t) n > cap) inCount = 0;   // should never happen; drop rather than overflow
        std::copy (io, io + n, inFifo.begin() + (long) inCount);
        inCount += (size_t) n;

        // Host rate -> model rate.
        int m = (int) std::floor (((double) inCount - 6.0) / speedUp);
        m = juce::jlimit (0, (int) modelIn.size(), m);
        if (m > 0)
        {
            const int used = up.process (speedUp, inFifo.data(), modelIn.data(), m);
            std::move (inFifo.begin() + used, inFifo.begin() + (long) inCount, inFifo.begin());
            inCount -= (size_t) used;
            float* ip = modelIn.data();
            float* op = modelOut.data();
            model.process (&ip, &op, m);
            if (midCount + (size_t) m > mid.size()) midCount = 0;
            std::copy (modelOut.begin(), modelOut.begin() + m, mid.begin() + (long) midCount);
            midCount += (size_t) m;
        }

        // Model rate -> host rate.
        int k = (int) std::floor (((double) midCount - 6.0) / speedDown);
        k = juce::jlimit (0, (int) tmp.size(), k);
        if (k > 0)
        {
            const int used = down.process (speedDown, mid.data(), tmp.data(), k);
            std::move (mid.begin() + used, mid.begin() + (long) midCount, mid.begin());
            midCount -= (size_t) used;
            if (outCount + (size_t) k > outFifo.size()) outCount = 0;
            std::copy (tmp.begin(), tmp.begin() + k, outFifo.begin() + (long) outCount);
            outCount += (size_t) k;
        }

        const int avail = (int) juce::jmin ((size_t) n, outCount);
        const int missing = n - avail;
        std::fill (io, io + missing, 0.0f);
        std::copy (outFifo.begin(), outFifo.begin() + avail, io + missing);
        std::move (outFifo.begin() + avail, outFifo.begin() + (long) outCount, outFifo.begin());
        outCount -= (size_t) avail;
    }

    std::unique_ptr<LoadedModel> loadModel (const juce::File& f, double hostRate, int maxBlock, juce::String& error)
    {
        auto lm = std::make_unique<LoadedModel>();
        try
        {
            nam::DspLoadOptions opts;
            opts.prewarm = false;
           #if JUCE_WINDOWS
            const std::filesystem::path path (f.getFullPathName().toWideCharPointer());
           #else
            const std::filesystem::path path (f.getFullPathName().toStdString());
           #endif
            lm->dsp = nam::get_dsp (path, opts);
        }
        catch (const std::exception& e)
        {
            error = e.what();
            return nullptr;
        }
        if (lm->dsp == nullptr) { error = "Not a NAM model"; return nullptr; }

        double modelRate = lm->dsp->GetExpectedSampleRate();
        if (modelRate <= 0) modelRate = 48000.0;   // older files don't say; NAM's default is 48 kHz
        const bool arbitrary = lm->dsp->SupportsArbitrarySampleRate();
        const double runRate = arbitrary ? hostRate : modelRate;
        lm->dsp->ResetAndPrewarm (runRate, maxBlock * 8 + 1024);
        if (! arbitrary) lm->resampler.prepare (hostRate, modelRate, maxBlock);
        lm->scratch.assign ((size_t) maxBlock, 0.0f);

        lm->file = f;
        lm->name = f.getFileNameWithoutExtension();
        lm->hasLoudness = lm->dsp->HasLoudness();
        lm->loudnessDb = lm->hasLoudness ? lm->dsp->GetLoudness() : 0.0;

        // Label from the file itself: architecture, and A2 for the new slimmable format.
        juce::String arch = "NAM";
        const auto json = juce::JSON::parse (f);
        if (auto* obj = json.getDynamicObject())
        {
            const auto a = obj->getProperty ("architecture").toString();
            const auto v = obj->getProperty ("version").toString();
            arch = (a == "SlimmableContainer" || v.startsWith ("0.7")) ? "A2" : (a.isNotEmpty() ? a : arch);
            if (auto* meta = obj->getProperty ("metadata").getDynamicObject())
            {
                const auto by = meta->getProperty ("modeled_by").toString();
                if (by.isNotEmpty()) lm->author = by;
            }
        }
        lm->info = arch + "  " + juce::String (modelRate / 1000.0, 1) + " kHz" + (lm->resampler.needed() ? " (resampled)" : "");
        return lm;
    }

    void processModel (LoadedModel& m, float* io, int n) noexcept
    {
        if (m.resampler.needed())
            m.resampler.process (*m.dsp, io, n);
        else
        {
            const int cap = (int) m.scratch.size();
            for (int start = 0; start < n; start += cap)
            {
                const int len = juce::jmin (cap, n - start);
                float* ip = io + start;
                float* op = m.scratch.data();
                m.dsp->process (&ip, &op, len);
                std::copy (op, op + len, io + start);
            }
        }
    }
}
