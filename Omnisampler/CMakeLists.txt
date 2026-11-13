# Omnisampler: the instrument (AU, VST3, Standalone, AUv3 on iOS), and on Mac/Windows also
# Omnisampler FX, the same sampler built as an effect that samples from its audio input.
set(OMNI_SOURCES
    Source/Formats.cpp
    Source/Engine.cpp
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/Streamer.cpp
    Source/DrumSynth.cpp
    Source/Sf2.cpp
    Source/Exs.cpp
    Source/Ableton.cpp
    Source/Kontakt.cpp
    Source/DsExport.cpp
    Source/Capture.cpp
    Source/Sampling.cpp
    Source/SamplingPanel.cpp)

# The standalone app samples from the audio input, so macOS asks for microphone access.
set(OMNI_DESKTOP_OPTIONS "")
if(NOT IOS)
    set(OMNI_DESKTOP_OPTIONS
        MICROPHONE_PERMISSION_ENABLED TRUE
        MICROPHONE_PERMISSION_TEXT "Omnisampler records from the audio input when you sample a sound.")
endif()

function(omni_target_setup target)
    target_sources(${target} PRIVATE ${OMNI_SOURCES})
    target_compile_definitions(${target} PUBLIC JUCE_USE_CURL=0 JUCE_USE_MP3AUDIOFORMAT=1)
    if(NOT IOS)
        # The Auto Sampler can load another instrument plug-in and sample it.
        target_compile_definitions(${target} PUBLIC JUCE_PLUGINHOST_VST3=1)
        if(APPLE)
            target_compile_definitions(${target} PUBLIC JUCE_PLUGINHOST_AU=1)
        endif()
    endif()
    evan_plugin_defaults(${target})
endfunction()

juce_add_plugin(Omnisampler
    PRODUCT_NAME "Omnisampler"
    COMPANY_NAME "emac87"
    BUNDLE_ID "${EVAN_BUNDLE_PREFIX}.omnisampler"
    PLUGIN_MANUFACTURER_CODE Emac
    PLUGIN_CODE Omns
    FORMATS ${PLUGIN_FORMATS}
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    AU_MAIN_TYPE kAudioUnitType_MusicDevice
    ICON_BIG "${CMAKE_SOURCE_DIR}/Shared/icons/omnisampler.png"
    ${EVAN_COMMON_OPTIONS}
    ${OMNI_DESKTOP_OPTIONS}
    VST3_CATEGORIES Instrument Sampler)

omni_target_setup(Omnisampler)

if(IOS)
    # iPhone/iPad: the system file picker, with picked files copied into the app's own storage.
    target_sources(Omnisampler PRIVATE Source/IOSFiles.mm)
    target_link_libraries(Omnisampler PUBLIC "-framework UniformTypeIdentifiers")
endif()
evan_ios_archive(Omnisampler)

if(NOT IOS)
    # Omnisampler FX goes in an effects slot (after an instrument, or on an audio track) and samples
    # what comes in. It sends the Auto Sampler's notes out as MIDI where the host can route them.
    set(OMNI_FX_FORMATS VST3)
    if(APPLE)
        list(APPEND OMNI_FX_FORMATS AU)
    endif()
    juce_add_plugin(OmnisamplerFX
        PRODUCT_NAME "Omnisampler FX"
        COMPANY_NAME "emac87"
        BUNDLE_ID "${EVAN_BUNDLE_PREFIX}.omnisamplerfx"
        PLUGIN_MANUFACTURER_CODE Emac
        PLUGIN_CODE OmFx
        FORMATS ${OMNI_FX_FORMATS}
        IS_SYNTH FALSE
        NEEDS_MIDI_INPUT TRUE
        NEEDS_MIDI_OUTPUT TRUE
        IS_MIDI_EFFECT FALSE
        EDITOR_WANTS_KEYBOARD_FOCUS FALSE
        COPY_PLUGIN_AFTER_BUILD FALSE
        AU_MAIN_TYPE kAudioUnitType_Effect
        VST3_CATEGORIES Fx Tools)
    omni_target_setup(OmnisamplerFX)
    target_compile_definitions(OmnisamplerFX PUBLIC OMNI_FX=1)
endif()
