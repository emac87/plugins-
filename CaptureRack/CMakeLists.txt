juce_add_plugin(CaptureRack
    PRODUCT_NAME "Capture Rack"
    COMPANY_NAME "emac87"
    BUNDLE_ID "${EVAN_BUNDLE_PREFIX}.capturerack"
    PLUGIN_MANUFACTURER_CODE Emac
    PLUGIN_CODE Crak
    FORMATS ${PLUGIN_FORMATS}
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    NEEDS_CURL TRUE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD FALSE
    AU_MAIN_TYPE kAudioUnitType_Effect
    ICON_BIG "${CMAKE_SOURCE_DIR}/Shared/icons/capturerack.png"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "Capture Rack plays your guitar through the amp captures."
    ${EVAN_COMMON_OPTIONS}
    VST3_CATEGORIES Fx Distortion)

# NeuralAmpModelerCore (MIT). Built as an OBJECT library and linked straight into each
# plug-in binary: NAM registers its model types with static initialisers, which a linker
# would drop if the objects only lived inside a static library.
file(GLOB NAM_SOURCES CONFIGURE_DEPENDS ${NAM_SOURCE_DIR}/NAM/*.cpp ${NAM_SOURCE_DIR}/NAM/*/*.cpp)
add_library(nam_core OBJECT ${NAM_SOURCES})
set_target_properties(nam_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(nam_core PUBLIC ${NAM_SOURCE_DIR} ${NAM_SOURCE_DIR}/Dependencies/nlohmann ${EIGEN_INCLUDE_DIR})
target_compile_definitions(nam_core PUBLIC NAM_SAMPLE_FLOAT NAM_ENABLE_A2_FAST)
if(MSVC)
    target_compile_options(nam_core PRIVATE /w /bigobj)
    target_compile_definitions(nam_core PUBLIC NOMINMAX)
else()
    target_compile_options(nam_core PRIVATE -w)
endif()

target_sources(CaptureRack PRIVATE
    Source/NamSlot.cpp
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)
foreach(fmt VST3 Standalone AU AUv3)
    if(TARGET CaptureRack_${fmt})
        target_link_libraries(CaptureRack_${fmt} PRIVATE nam_core)
    endif()
endforeach()

target_include_directories(CaptureRack PRIVATE
    ${NAM_SOURCE_DIR}
    ${NAM_SOURCE_DIR}/Dependencies/nlohmann
    ${EIGEN_INCLUDE_DIR})

target_compile_definitions(CaptureRack PRIVATE NAM_SAMPLE_FLOAT NAM_ENABLE_A2_FAST)
target_compile_definitions(CaptureRack PUBLIC JUCE_USE_CURL=1 JUCE_LOAD_CURL_SYMBOLS_LAZILY=1)
if(MSVC)
    target_compile_options(CaptureRack PRIVATE /bigobj)
endif()
evan_plugin_defaults(CaptureRack)
evan_ios_archive(CaptureRack)
