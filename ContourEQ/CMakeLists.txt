juce_add_plugin(ContourEQ
    PRODUCT_NAME "Contour EQ"
    COMPANY_NAME "emac87"
    BUNDLE_ID "${EVAN_BUNDLE_PREFIX}.contoureq"
    PLUGIN_MANUFACTURER_CODE Emac
    PLUGIN_CODE Cteq
    FORMATS ${PLUGIN_FORMATS}
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    AU_MAIN_TYPE kAudioUnitType_Effect
    ICON_BIG "${CMAKE_SOURCE_DIR}/Shared/icons/contoureq.png"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "Contour EQ processes the audio you play into it."
    ${EVAN_COMMON_OPTIONS}
    VST3_CATEGORIES Fx EQ)

target_sources(ContourEQ PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_compile_definitions(ContourEQ PUBLIC JUCE_USE_CURL=0)
evan_plugin_defaults(ContourEQ)
evan_ios_archive(ContourEQ)
