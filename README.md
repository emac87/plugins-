# Plugins

Three audio plug-ins, each built as **AU (Mac), VST3 (Mac + Windows) and a Standalone app** with JUCE 8.

| Plug-in | What it is |
| --- | --- |
| **Contour EQ** | Visual 24-band EQ: big analyser, click-to-add bands, 9 shapes, cuts to 96 dB/oct, stereo/L/R/mid/side per band, dynamic EQ with external sidechain, Match EQ (reference from a file or the sidechain), A/B, undo, presets. |
| **Capture Rack** | NAM loader: 3 slots (series in any order, or parallel with level/pan), A1 and the newer A2 models, automatic resampling, per-slot gate, cab IR slot, tone EQ, a local library with category folders and favourites, and a TONE3000 browser that downloads captures into those folders. |
| **Omnisampler** | Sampler with a Pads section (4 banks x 16) and a Keys section. Drop many files and it sets velocity layers by loudness (or round robins); on Keys it reads notes from file names, metadata or the audio itself. Drum-synth layers on pads (kick, snare, tom, hats, cymbal, clap, perc/FM, noise, 808) stack with the samples. 16 stereo outputs (pick one per pad and for the keys). Long samples stream from disk. Opens SFZ, DecentSampler (.dspreset, .dslibrary), SoundFont (.sf2), EXS24 (.exs), Ableton Sampler/Simpler (.adv), unlocked Kontakt (.nki, incl. .ncw samples), zips of those, and audio files; saves kits, SFZ and DecentSampler. The **Sample** page records from the audio input onto a pad or the keys (record/stop, start on sound, or a number of bars) and has an **Auto Sampler** that plays an instrument across a note range at several velocities and builds a velocity-layered instrument for the Keys or a kit for the Pads. **Omnisampler FX** (AU and VST3, Mac and Windows) is the same sampler as an effect: it sits in an effects slot, passes the audio through and samples what comes in. |

## Getting the builds

Every push builds everything on GitHub (Actions tab -> latest run -> Artifacts):
`plugins-macos` (one zip per plug-in with .component, .vst3 and .app) and `plugins-windows` (.vst3 and .exe).
Builds are not signed/notarised; each zip has a README with install steps.

## Building locally

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release        # or -G Xcode on a Mac
cmake --build build
```

Options: `-DBUILD_CONTOUR_EQ=OFF`, `-DBUILD_CAPTURE_RACK=OFF`, `-DBUILD_OMNISAMPLER=OFF` to skip one,
`-DBUILD_TESTS=ON` for the headless tests (`ctest --test-dir build`), and `-DJUCE_DIR_LOCAL=...`,
`-DNAM_DIR_LOCAL=...`, `-DEIGEN_DIR_LOCAL=...` to use local copies instead of downloading.

## Notes

- **TONE3000:** sign-in uses TONE3000's device flow. You need a publishable key (client_id) from
  tone3000.com/api, pasted into the TONE3000 tab once. The free API tier is for non-commercial use;
  selling the plug-in needs TONE3000's commercial terms and their sign-off.
- **Licences:** JUCE is AGPLv3 or commercial; NeuralAmpModelerCore is MIT; Eigen is MPL2.
- **Omnisampler MIDI:** Pads and Keys both listen on every channel by default. Set "Pads MIDI" to a channel
  (e.g. 10) to split them; Keys on "Any" then ignore that channel. Encrypted Kontakt libraries (.nkx, Kontakt Player) can't be opened.
- **Omnisampler sampling:** the instrument has no audio input inside a DAW, so record with Omnisampler FX (in an effects slot)
  or the standalone app. The Auto Sampler works two ways. "A plug-in loaded here" loads an AU/VST3 instrument inside
  Omnisampler and renders it faster than real time. "The audio input" sends the notes out as MIDI (the plug-in's MIDI out,
  a virtual port called "Omnisampler" on the Mac, or any MIDI device) and records what comes back: for hardware synths,
  or for an instrument track with Omnisampler FX after it. In Logic the virtual port is picked for you, because Logic
  doesn't route a plug-in's MIDI output; keep the instrument's track selected. Everything sampled is saved under
  Music/Omnisampler (Recordings, Auto Sampled), which both versions share (Sample > Library...).
- **Omnisampler outputs:** in Logic pick the "Multi-Output" version of the instrument, then set a pad's Output (Out 2..16) and add aux channels with the mixer's + button.
