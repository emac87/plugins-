juce_add_plugin(ContourEQ
    PRODUCT_NAME "Contour EQ"
    COMPANY_NAME "emac87"
    BUNDLE_ID "com.emac87.contoureq"
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
    VST3_CATEGORIES Fx EQ)

target_sources(ContourEQ PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_compile_definitions(ContourEQ PUBLIC JUCE_USE_CURL=0)
evan_plugin_defaults(ContourEQ)
