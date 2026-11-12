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
    VST3_CATEGORIES Instrument Sampler)

target_sources(Omnisampler PRIVATE
    Source/Formats.cpp
    Source/Engine.cpp
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

if(IOS)
    # iPhone/iPad: the system file picker, with picked files copied into the app's own storage.
    target_sources(Omnisampler PRIVATE Source/IOSFiles.mm)
    target_link_libraries(Omnisampler PUBLIC "-framework UniformTypeIdentifiers")
endif()

target_compile_definitions(Omnisampler PUBLIC JUCE_USE_CURL=0 JUCE_USE_MP3AUDIOFORMAT=1)
evan_plugin_defaults(Omnisampler)
evan_ios_archive(Omnisampler)
