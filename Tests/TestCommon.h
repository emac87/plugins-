#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <cstdio>

static int failures = 0;
#define CHECK(cond, msg) do { if (cond) std::printf ("  ok   %s\n", msg); else { std::printf ("  FAIL %s\n", msg); ++failures; } } while (0)

inline void setParam (juce::AudioProcessorValueTreeState& s, const juce::String& id, float v)
{
    auto* p = s.getParameter (id);
    jassert (p != nullptr);
    p->setValueNotifyingHost (p->convertTo0to1 (v));
}

inline double rms (const juce::AudioBuffer<float>& b, int ch, int start, int n)
{
    double s = 0;
    for (int i = 0; i < n; ++i) s += (double) b.getSample (ch, start + i) * b.getSample (ch, start + i);
    return std::sqrt (s / juce::jmax (1, n));
}

inline void snapshot (juce::AudioProcessor& p, const juce::File& out, std::function<void (juce::Component&)> prep = {})
{
    std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
    if (prep) prep (*ed);
    auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 1.0f);
    out.deleteFile();
    juce::FileOutputStream fos (out);
    juce::PNGImageFormat().writeImageToStream (img, fos);
    std::printf ("  wrote %s (%dx%d)\n", out.getFullPathName().toRawUTF8(), img.getWidth(), img.getHeight());
}

inline bool writeWav (const juce::File& f, const juce::AudioBuffer<float>& b, double sr, juce::StringPairArray meta = {})
{
    f.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> os (new juce::FileOutputStream (f));
    std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (os.get(), sr, (unsigned) b.getNumChannels(), 24, meta, 0));
    if (w == nullptr) return false;
    os.release();
    return w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
}
